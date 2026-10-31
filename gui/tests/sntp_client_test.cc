#undef NDEBUG

#include <assert.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>

#include "sntp_client.hh"

using namespace std::chrono_literals;
using wristctl::gui::SntpRequest;
using wristctl::gui::SntpSample;
using wristctl::gui::query_sntp;

namespace {

enum class Reply { Valid, BadOriginate, Unsynchronized, WrongMode, Short, None };

uint32_t read_u32(const uint8_t *bytes)
{
    return (static_cast<uint32_t>(bytes[0]) << 24) |
           (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) | bytes[3];
}

void write_time(uint8_t *bytes, uint64_t timestamp)
{
    const uint32_t seconds = static_cast<uint32_t>(timestamp >> 32);
    const uint32_t fraction = static_cast<uint32_t>(timestamp);
    bytes[0] = static_cast<uint8_t>(seconds >> 24);
    bytes[1] = static_cast<uint8_t>(seconds >> 16);
    bytes[2] = static_cast<uint8_t>(seconds >> 8);
    bytes[3] = static_cast<uint8_t>(seconds);
    bytes[4] = static_cast<uint8_t>(fraction >> 24);
    bytes[5] = static_cast<uint8_t>(fraction >> 16);
    bytes[6] = static_cast<uint8_t>(fraction >> 8);
    bytes[7] = static_cast<uint8_t>(fraction);
}

uint64_t add_seconds(uint64_t timestamp, long double seconds)
{
    return timestamp + static_cast<uint64_t>(seconds * 4294967296.0L);
}

class FakeServer {
public:
    explicit FakeServer(Reply reply) : reply_(reply)
    {
        fd_ = socket(AF_INET, SOCK_DGRAM, 0);
        assert(fd_ >= 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        assert(bind(fd_, reinterpret_cast<sockaddr *>(&address), sizeof address) == 0);
        socklen_t length = sizeof address;
        assert(getsockname(fd_, reinterpret_cast<sockaddr *>(&address), &length) == 0);
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] { run(); });
    }

    ~FakeServer()
    {
        thread_.join();
        close(fd_);
    }

    uint16_t port() const { return port_; }
    bool exchanged() const { return exchanged_; }

private:
    void run()
    {
        uint8_t request[48];
        sockaddr_in peer{};
        socklen_t peer_length = sizeof peer;
        const ssize_t count = recvfrom(fd_, request, sizeof request, 0,
                                       reinterpret_cast<sockaddr *>(&peer),
                                       &peer_length);
        assert(count == static_cast<ssize_t>(sizeof request));
        exchanged_ = true;
        if (reply_ == Reply::None) {
            return;
        }

        uint8_t response[48]{};
        response[0] = reply_ == Reply::WrongMode ? 0x23 : 0x24;
        if (reply_ == Reply::Unsynchronized) {
            response[0] |= 0xc0;
        }
        response[1] = 1;
        std::memcpy(response + 24, request + 40, 8);
        if (reply_ == Reply::BadOriginate) {
            response[24]++;
        }
        uint64_t originate = (static_cast<uint64_t>(read_u32(request + 40)) << 32) |
                             read_u32(request + 44);
        write_time(response + 32, add_seconds(originate, 5.25L));
        write_time(response + 40, add_seconds(originate, 5.5L));
        const size_t length = reply_ == Reply::Short ? 20 : sizeof response;
        assert(sendto(fd_, response, length, 0, reinterpret_cast<sockaddr *>(&peer),
                      peer_length) == static_cast<ssize_t>(length));
    }

    int fd_ = -1;
    uint16_t port_ = 0;
    Reply reply_;
    bool exchanged_ = false;
    std::thread thread_;
};

SntpRequest request(const FakeServer &server, std::chrono::milliseconds timeout = 1s)
{
    return {"127.0.0.1", server.port(), timeout};
}

void rejected(Reply reply, const std::string &message)
{
    FakeServer server(reply);
    const auto result = query_sntp(request(server));
    assert(!result.ok);
    assert(result.error.find(message) != std::string::npos);
    assert(server.exchanged());
}

} // namespace

int main()
{
    {
        FakeServer server(Reply::Valid);
        const auto result = query_sntp(request(server));
        assert(result.ok);
        assert(result.error.empty());
        assert(result.sample.server == "127.0.0.1");
        assert(result.sample.offset > 4500ms);
        assert(result.sample.offset < 6300ms);
        assert(result.sample.round_trip > -1s);
        assert(result.sample.round_trip < 2s);
        assert(server.exchanged());
    }

    rejected(Reply::BadOriginate, "originate timestamp");
    rejected(Reply::Unsynchronized, "unsynchronized");
    rejected(Reply::WrongMode, "server mode");
    rejected(Reply::Short, "short SNTP");
    {
        FakeServer server(Reply::None);
        const auto result = query_sntp(request(server, 30ms));
        assert(!result.ok);
        assert(result.error.find("timed out") != std::string::npos);
    }

    assert(!query_sntp({"", 123, 1s}).ok);
    assert(!query_sntp({"127.0.0.1", 123, 0ms}).ok);

    {
        SntpSample sample;
        sample.corrected_at = std::chrono::system_clock::time_point(10s);
        sample.sampled_at = std::chrono::steady_clock::time_point(100s);
        assert(sample.corrected_now(std::chrono::steady_clock::time_point(105s)) ==
               std::chrono::system_clock::time_point(15s));
    }
    {
        const auto utc = wristctl::gui::utc_tm(
            std::chrono::system_clock::time_point(951782400s));
        assert(utc.tm_year == 100);
        assert(utc.tm_mon == 1);
        assert(utc.tm_mday == 29);
        assert(utc.tm_hour == 0);
        assert(utc.tm_wday == 2);
    }
    return 0;
}
