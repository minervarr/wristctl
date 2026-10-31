#undef NDEBUG

#include <assert.h>

#include <chrono>

#include "reconnect_policy.hh"

using namespace std::chrono_literals;
using wristctl::gui::ReconnectPolicy;

int main()
{
    ReconnectPolicy policy;
    const auto t0 = ReconnectPolicy::Clock::now();

    assert(policy.desired());
    assert(policy.due(t0));

    policy.on_failure(t0);
    assert(policy.failures() == 1);
    assert(policy.delay() == 2s);
    assert(!policy.due(t0));
    assert(!policy.due(t0 + 1999ms));
    assert(policy.due(t0 + 2s));

    policy.on_failure(t0 + 2s);
    assert(policy.delay() == 5s);
    policy.on_failure(t0 + 7s);
    assert(policy.delay() == 10s);
    policy.on_failure(t0 + 17s);
    assert(policy.delay() == 15s);
    policy.on_failure(t0 + 32s);
    assert(policy.delay() == 15s);
    policy.on_failure(t0 + 47s);
    assert(policy.failures() == 6);
    assert(policy.delay() == 15s);

    policy.on_success();
    assert(policy.failures() == 0);
    assert(policy.delay() == 2s);
    assert(!policy.due(t0));

    policy.set_desired(false, t0);
    assert(!policy.desired());
    assert(!policy.due(t0 + 1h));
    policy.retry_now(t0 + 1h);
    assert(!policy.due(t0 + 1h));

    policy.set_desired(true, t0 + 1h);
    assert(policy.desired());
    assert(policy.due(t0 + 1h));

    policy.retry_now(t0 + 2h);
    assert(!policy.due(t0 + 1h));
    assert(policy.due(t0 + 2h));

    return 0;
}
