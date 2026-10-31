#undef NDEBUG

#include <assert.h>

#include "phone_model.hh"
#include "watch_model.hh"

int main()
{
    assert(wristctl::gui::offset_text(0, true) == "UTC+00:00");
    assert(wristctl::gui::offset_text(4, true) == "UTC+01:00");
    assert(wristctl::gui::offset_text(-6, true) == "UTC-01:30");
    assert(wristctl::gui::offset_text(3, false) == "+00:45");

    wc_program_timer timer{};
    assert(wristctl::gui::timer_round_seconds(timer) == 0);
    assert(wristctl::gui::duration_text(0) == "0:00");

    timer.workouts[0].minutes = 25;
    timer.workouts[1].minutes = 1;
    timer.workouts[1].seconds = 5;
    timer.workouts[2].seconds = 59;
    assert(wristctl::gui::timer_round_seconds(timer) == 1624);
    assert(wristctl::gui::duration_text(1624) == "27:04");

    timer.workouts[0].minutes = 59;
    assert(wristctl::gui::timer_round_seconds(timer) == 3664);
    assert(wristctl::gui::duration_text(3664) == "1:01:04");

    wristctl::gui::PhoneConfig phone;
    assert(!wristctl::gui::phone_notification_allowed(
        phone, "com.example.mail", 12 * 60));
    phone.forward = true;
    assert(wristctl::gui::phone_notification_allowed(
        phone, "com.example.mail", 12 * 60));
    phone.allowed_packages = {"com.example.mail"};
    assert(wristctl::gui::phone_notification_allowed(
        phone, "com.example.mail", 12 * 60));
    assert(!wristctl::gui::phone_notification_allowed(
        phone, "com.example.mail.other", 12 * 60));
    phone.quiet_start_min = 22 * 60;
    phone.quiet_end_min = 7 * 60;
    assert(!wristctl::gui::phone_notification_allowed(
        phone, "com.example.mail", 23 * 60));
    assert(!wristctl::gui::phone_notification_allowed(
        phone, "com.example.mail", 6 * 60));
    assert(wristctl::gui::phone_notification_allowed(
        phone, "com.example.mail", 12 * 60));
    return 0;
}
