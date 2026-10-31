#undef NDEBUG

#include <assert.h>

#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>

#include "utc_sync.hh"

namespace {

struct FakeTransport {
    std::deque<std::vector<uint8_t>> frames;
    std::vector<std::vector<uint8_t>> writes;
};

int write_frame(void *ctx, wc_write_kind kind, const uint8_t *data, size_t len)
{
    assert(kind == WC_WRITE_REQUEST || kind == WC_WRITE_ALL);
    auto &fake = *static_cast<FakeTransport *>(ctx);
    fake.writes.emplace_back(data, data + len);
    return 0;
}

int read_frame(void *ctx, uint8_t *data, size_t cap, uint32_t)
{
    auto &fake = *static_cast<FakeTransport *>(ctx);
    if (fake.frames.empty()) {
        return 0;
    }
    const auto frame = fake.frames.front();
    fake.frames.pop_front();
    assert(frame.size() <= cap);
    memcpy(data, frame.data(), frame.size());
    return static_cast<int>(frame.size());
}

wc_session *make_session(FakeTransport &fake)
{
    wc_transport transport{};
    transport.ctx = &fake;
    transport.write = write_frame;
    transport.read = read_frame;
    const uint8_t app_id[WC_APP_ID_LEN]{};
    return wc_session_new(&transport, app_id, 10);
}

void add_state(FakeTransport &fake)
{
    fake.frames.push_back({0x1d, 1, 1, 7, 7, 0x12, 0x34, 0x56,
                           0x78, 0x12, 0x34, 0x22, 0x22, 0x33, 0x33});
}

void add_setting(FakeTransport &fake, uint8_t unit)
{
    fake.frames.push_back(
        {0x1e, unit, 0x12, static_cast<uint8_t>(0x34 + unit), 0xec, 4, 2});
}

void add_name(FakeTransport &fake, uint8_t unit, const char *name)
{
    std::vector<uint8_t> frame(20, 0);
    frame[0] = 0x1f;
    frame[1] = unit;
    memcpy(frame.data() + 2, name, strlen(name));
    fake.frames.push_back(std::move(frame));
}

} // namespace

int main()
{
    using wristctl::gui::UtcClock;
    using wristctl::gui::configure_utc_clock;
    using wristctl::gui::read_utc_clock;
    using wristctl::gui::utc_clock_is_configured;

    FakeTransport fake;
    add_state(fake);
    add_setting(fake, 3);
    add_setting(fake, 4);
    add_setting(fake, 0);
    add_setting(fake, 1);
    add_name(fake, 0, "HOME");
    add_name(fake, 1, "WORLD");
    wc_session *session = make_session(fake);
    assert(session != nullptr);

    UtcClock clock;
    assert(read_utc_clock(session, clock));
    assert(fake.frames.empty());
    assert(fake.writes.size() == 7);
    assert(fake.writes[5] == std::vector<uint8_t>({0x1f, 0}));
    assert(fake.writes[6] == std::vector<uint8_t>({0x1f, 1}));
    assert(clock.settings[0].unit == 0);
    assert(clock.settings[1].unit == 1);
    assert(clock.settings[0].city == 0x3412);
    assert(clock.settings[1].city == 0x3512);

    configure_utc_clock(clock);
    assert(utc_clock_is_configured(clock));
    assert(clock.state.watch_no_home == 1);
    assert(clock.state.watch_no_world == 1);
    assert(clock.state.city_home == 0x3412);
    assert(clock.state.city_world == 0x7856);
    assert(clock.state.city_extra_1 == 0x3412);
    assert(clock.settings[0].city == 0x3412);
    assert(clock.settings[1].city == 0x3512);
    assert(strcmp(clock.names[0].name, "UTC") == 0);
    assert(strcmp(clock.names[1].name, "UTC") == 0);

    clock.state.dst_on_world = 1;
    assert(!utc_clock_is_configured(clock));
    clock.state.dst_on_world = 0;
    clock.settings[1].timezone = 4;
    assert(!utc_clock_is_configured(clock));

    wc_session_free(session);

    FakeTransport partial;
    add_state(partial);
    add_setting(partial, 0);
    add_setting(partial, 1);
    partial.frames.push_back({0xff, 0x81, 0x1e});
    add_name(partial, 0, "HOME");
    add_name(partial, 1, "WORLD");
    session = make_session(partial);
    assert(session != nullptr);

    wristctl::gui::ClockRead read;
    std::string error;
    assert(!wristctl::gui::read_clock(session, read, true, &error));
    assert(!error.empty());
    assert(read.has_utc_clock());
    assert(!read.complete());
    assert(partial.frames.empty());
    assert(partial.writes.size() == 6);
    assert(partial.writes[3] == std::vector<uint8_t>({0x1e}));
    assert(partial.writes[4] == std::vector<uint8_t>({0x1f, 0}));
    assert(partial.writes[5] == std::vector<uint8_t>({0x1f, 1}));
    UtcClock home_world = read.utc_clock();
    assert(home_world.settings[0].unit == 0);
    assert(home_world.settings[1].unit == 1);
    assert(strcmp(home_world.names[0].name, "HOME") == 0);
    assert(strcmp(home_world.names[1].name, "WORLD") == 0);

    wc_session_free(session);
    return 0;
}
