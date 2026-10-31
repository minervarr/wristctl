#pragma once

// Pure reconnect timing for the GUI worker. It owns no sockets and no threads,
// so the backoff sequence is testable without BlueZ or a watch.

#include <chrono>

namespace wristctl::gui {

class ReconnectPolicy {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    void set_desired(bool desired, TimePoint now)
    {
        desired_ = desired;
        if (desired) {
            next_attempt_ = now;
        }
    }

    bool desired() const { return desired_; }

    bool due(TimePoint now) const
    {
        return desired_ && now >= next_attempt_;
    }

    void retry_now(TimePoint now) { next_attempt_ = now; }

    void on_success()
    {
        failures_ = 0;
        next_attempt_ = TimePoint::max();
    }

    void on_failure(TimePoint now)
    {
        if (failures_ < 1000) {
            failures_++;
        }
        next_attempt_ = now + delay();
    }

    unsigned failures() const { return failures_; }

    std::chrono::milliseconds delay() const
    {
        static constexpr unsigned kDelaysMs[] = {2000, 5000, 10000, 15000,
                                                 15000};
        constexpr unsigned count = sizeof kDelaysMs / sizeof kDelaysMs[0];
        unsigned index = failures_ == 0 ? 0 : failures_ - 1;
        if (index >= count) {
            index = count - 1;
        }
        return std::chrono::milliseconds(kDelaysMs[index]);
    }

private:
    bool desired_ = true;
    unsigned failures_ = 0;
    TimePoint next_attempt_{};
};

} // namespace wristctl::gui
