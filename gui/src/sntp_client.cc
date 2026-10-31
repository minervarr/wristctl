#include "sntp_client.hh"

#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>

namespace wristctl::gui {
namespace {

constexpr uint32_t kNtpUnixEpoch = 2208988800U;
constexpr size_t kNtpPacketSize = 48;

using Nanoseconds = std::chrono::nanoseconds;
using SystemTime = std::chrono::system_clock::time_point;
using SteadyTime = std::chrono::steady_clock::time_point;

struct FileDescriptor {
    int value = -1;
    ~FileDescriptor()
    {
        if (value >= 0) {
            close(value);
        }
    }
};

uint32_t read_u32(const uint8_t *bytes)
{
    return (static_cast<uint32_t>(bytes[0]) << 24) |
           (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) |
           static_cast<uint32_t>(bytes[3]);
}

void write_u32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = static_cast<uint8_t>(value >> 24);
    bytes[1] = static_cast<uint8_t>(value >> 16);
    bytes[2] = static_cast<uint8_t>(value >> 8);
    bytes[3] = static_cast<uint8_t>(value);
}

Nanoseconds unix_nanoseconds(SystemTime time)
{
    return std::chrono::duration_cast<Nanoseconds>(time.time_since_epoch());
}

SystemTime system_time(Nanoseconds since_epoch)
{
    return SystemTime(std::chrono::duration_cast<SystemTime::duration>(since_epoch));
}

void write_ntp_time(uint8_t *bytes, Nanoseconds unix_time)
{
    const int64_t whole_seconds = unix_time.count() / 1000000000;
    const int64_t nanos = unix_time.count() % 1000000000;
    write_u32(bytes, static_cast<uint32_t>(whole_seconds + kNtpUnixEpoch));
    write_u32(bytes + 4,
              static_cast<uint32_t>((static_cast<uint64_t>(nanos) << 32) /
                                    1000000000U));
}

Nanoseconds read_ntp_time(const uint8_t *bytes)
{
    const int64_t seconds = static_cast<int64_t>(read_u32(bytes)) - kNtpUnixEpoch;
    const uint64_t fraction = read_u32(bytes + 4);
    const uint64_t nanos = (fraction * 1000000000U) >> 32;
    return Nanoseconds(seconds * 1000000000 + static_cast<int64_t>(nanos));
}

SntpResult failure(const std::string &message)
{
    SntpResult result;
    result.error = message;
    return result;
}

std::string socket_error(const std::string &operation)
{
    return operation + ": " + std::strerror(errno);
}

SntpResult query_address(const addrinfo &address, const SntpRequest &request,
                         SteadyTime deadline)
{
    FileDescriptor socket{::socket(address.ai_family, address.ai_socktype,
                                   address.ai_protocol)};
    if (socket.value < 0) {
        return failure(socket_error("could not create UDP socket"));
    }
    if (connect(socket.value, address.ai_addr, address.ai_addrlen) != 0) {
        return failure(socket_error("could not connect UDP socket"));
    }

    const SystemTime client_send = std::chrono::system_clock::now();
    uint8_t packet[kNtpPacketSize]{};
    packet[0] = 0x23;
    write_ntp_time(packet + 40, unix_nanoseconds(client_send));
    if (send(socket.value, packet, sizeof packet, 0) != sizeof packet) {
        return failure(socket_error("could not send SNTP request"));
    }

    while (true) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) {
            return failure("SNTP request timed out");
        }
        pollfd input{socket.value, POLLIN, 0};
        const int ready = poll(&input, 1, static_cast<int>(remaining.count()));
        if (ready > 0) {
            break;
        }
        if (ready == 0) {
            return failure("SNTP request timed out");
        }
        if (errno != EINTR) {
            return failure(socket_error("could not wait for SNTP response"));
        }
    }

    uint8_t response[512];
    const ssize_t received = recv(socket.value, response, sizeof response, 0);
    const SystemTime client_receive = std::chrono::system_clock::now();
    const SteadyTime sampled_at = std::chrono::steady_clock::now();
    if (received < 0) {
        return failure(socket_error("could not receive SNTP response"));
    }
    if (received < static_cast<ssize_t>(kNtpPacketSize)) {
        return failure("short SNTP response");
    }
    if ((response[0] & 0x07) != 4) {
        return failure("SNTP response is not server mode");
    }
    if ((response[0] >> 6) == 3) {
        return failure("SNTP server clock is unsynchronized");
    }
    if (response[1] == 0 || response[1] > 15) {
        return failure("SNTP server rejected the request or is unsynchronized");
    }
    if (std::memcmp(response + 24, packet + 40, 8) != 0) {
        return failure("SNTP originate timestamp does not match the request");
    }
    if (read_u32(response + 32) == 0 || read_u32(response + 40) == 0) {
        return failure("SNTP response has no receive/transmit timestamp");
    }

    const Nanoseconds t1 = unix_nanoseconds(client_send);
    const Nanoseconds t2 = read_ntp_time(response + 32);
    const Nanoseconds t3 = read_ntp_time(response + 40);
    const Nanoseconds t4 = unix_nanoseconds(client_receive);
    const Nanoseconds offset((t2.count() - t1.count() + t3.count() - t4.count()) /
                             2);
    const Nanoseconds round_trip((t4.count() - t1.count()) -
                                 (t3.count() - t2.count()));

    SntpResult result;
    result.ok = true;
    result.sample.server = request.server;
    result.sample.corrected_at = system_time(t4 + offset);
    result.sample.sampled_at = sampled_at;
    result.sample.offset = offset;
    result.sample.round_trip = round_trip;
    return result;
}

} // namespace

std::chrono::system_clock::time_point SntpSample::corrected_now(
    std::chrono::steady_clock::time_point now) const
{
    return corrected_at + std::chrono::duration_cast<SystemTime::duration>(
                              now - sampled_at);
}

SntpResult query_sntp(const SntpRequest &request)
{
    if (request.server.empty()) {
        return failure("SNTP server is empty");
    }
    if (request.timeout.count() <= 0) {
        return failure("SNTP timeout must be positive");
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    addrinfo *addresses = nullptr;
    const std::string port = std::to_string(request.port);
    const int gai = getaddrinfo(request.server.c_str(), port.c_str(), &hints,
                                &addresses);
    if (gai != 0) {
        return failure("could not resolve SNTP server: " +
                       std::string(gai_strerror(gai)));
    }

    const SteadyTime deadline = std::chrono::steady_clock::now() + request.timeout;
    SntpResult result = failure("SNTP server has no usable addresses");
    for (addrinfo *address = addresses; address != nullptr;
         address = address->ai_next) {
        result = query_address(*address, request, deadline);
        if (result.ok) {
            break;
        }
    }
    freeaddrinfo(addresses);
    return result;
}

std::tm utc_tm(std::chrono::system_clock::time_point time)
{
    const std::time_t seconds = std::chrono::system_clock::to_time_t(time);
    std::tm result{};
    gmtime_r(&seconds, &result);
    return result;
}

} // namespace wristctl::gui
