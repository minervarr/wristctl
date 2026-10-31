#pragma once

#include <chrono>
#include <cstdint>
#include <ctime>
#include <string>

namespace wristctl::gui {

struct SntpRequest {
    std::string server;
    uint16_t port = 123;
    std::chrono::milliseconds timeout{2000};
};

struct SntpSample {
    std::string server;
    std::chrono::system_clock::time_point corrected_at;
    std::chrono::steady_clock::time_point sampled_at;
    std::chrono::nanoseconds offset{0};
    std::chrono::nanoseconds round_trip{0};

    std::chrono::system_clock::time_point corrected_now(
        std::chrono::steady_clock::time_point now =
            std::chrono::steady_clock::now()) const;
};

struct SntpResult {
    bool ok = false;
    SntpSample sample;
    std::string error;
};

SntpResult query_sntp(const SntpRequest &request);
std::tm utc_tm(std::chrono::system_clock::time_point time);

} // namespace wristctl::gui
