#include "watch_view.hh"

#include <algorithm>
#include <cstdio>

#include "ui_components.hh"
#include "ui_theme.hh"
#include "watch_model.hh"

namespace wristctl::gui {
namespace {

const char *timer_sound(uint8_t beep)
{
    static const char *const names[] = {"Walk1", "Walk2", "Rush", "Rest"};
    return names[beep & 3];
}

} // namespace

float WatchView::drawTimerPage(Canvas &canvas, const Layout &layout)
{
    const auto &palette = dark_palette();
    const float scroll = page_scroll(route_index(route_));
    const float top = layout.top + layout.content_y;
    const float x = layout.left + layout.content_x;
    const float w = layout.content_w;
    const float d = touch_ ? std::max(1.0f, display_density_) : 1.0f;
    const float gap = touch_ ? 10.0f * d : 12.0f;
    const float pad = 18.0f * d;
    float cursor = top;

    auto button = [&](const Rect &rect, const std::string &label, Action action,
                      ButtonVariant variant = ButtonVariant::Secondary,
                      int arg = 0, int arg2 = 0, bool enabled = true) {
        draw_button(canvas, rect, label, variant, layout.small_size, enabled);
        if (enabled) {
            hits_.push_back({rect, action, 0, {}, arg, arg2});
        }
    };

    canvas.text("Program timer", x, cursor - scroll, layout.page_title_size,
                palette.text_primary);
    cursor += layout.page_title_size + 8.0f * d;
    canvas.text("Six steps · one repeatable sequence", x, cursor - scroll,
                layout.small_size, palette.text_secondary);
    cursor += layout.small_size + 18.0f * d;

    if (!snapshot_.has_timer) {
        const Rect empty = {x, cursor - scroll, w, 144.0f * d};
        draw_card(canvas, empty, 12.0f * d);
        draw_empty_state(canvas, empty, "Timer not read yet",
                         "Connect and refresh before editing.",
                         layout.date_size, layout.small_size);
        return empty.h + gap;
    }

    const Rect summary = {x, cursor - scroll, w, 116.0f * d};
    draw_card(canvas, summary, 14.0f * d);
    canvas.text("One round", summary.x + pad, summary.y + pad,
                layout.small_size, palette.text_secondary);
    canvas.text(duration_text(timer_round_seconds(edit_timer_)),
                summary.x + pad, summary.y + pad + 28.0f * d,
                layout.clock_size * 0.65f, palette.text_primary);
    char repeat[48];
    std::snprintf(repeat, sizeof repeat, "Repeat %u times", edit_timer_.repeat);
    canvas.text(repeat, summary.x + pad, summary.y + 88.0f * d,
                layout.small_size, palette.text_secondary);
    const float repeat_w = 92.0f * d;
    button({summary.x + summary.w - pad - repeat_w * 2.0f - gap,
            summary.y + summary.h - pad - layout.action_h, repeat_w,
            layout.action_h},
           "−1", Action::TimerRepeat, ButtonVariant::Secondary, -1);
    button({summary.x + summary.w - pad - repeat_w,
            summary.y + summary.h - pad - layout.action_h, repeat_w,
            layout.action_h},
           "+1", Action::TimerRepeat, ButtonVariant::Secondary, 1);
    cursor += summary.h + gap * 1.5f;

    draw_section_title(canvas, "Sequence", x, cursor - scroll,
                       layout.date_size);
    cursor += layout.date_size + 12.0f * d;
    for (int index = 0; index < 6; ++index) {
        const auto &item = edit_timer_.workouts[index];
        const bool empty = item.minutes == 0 && item.seconds == 0;
        const Rect card = {x, cursor - scroll, w, 88.0f * d};
        draw_card(canvas, card, 12.0f * d);
        char label[32];
        std::snprintf(label, sizeof label, "Step %d", index + 1);
        canvas.text(label, card.x + pad, card.y + 12.0f * d,
                    layout.small_size, palette.text_secondary);
        canvas.text(empty ? "Empty step"
                          : duration_text(static_cast<unsigned>(item.minutes) *
                                              60U +
                                          item.seconds),
                    card.x + pad, card.y + 38.0f * d, layout.date_size,
                    empty ? palette.disabled : palette.text_primary);
        const std::string sound = empty ? "No duration" : timer_sound(item.beep);
        canvas.text(sound, card.x + card.w - pad -
                               canvas.textWidth(sound, layout.small_size),
                    card.y + 42.0f * d, layout.small_size,
                    palette.text_secondary);
        addHit(card, Action::TimerEdit, {}, index);
        cursor += card.h + gap;
    }

    const float half = (w - gap) * 0.5f;
    button({x, cursor - scroll, half, layout.action_h},
           timer_dirty_
               ? (timer_confirm_ ? "Confirm replacement" : "Apply full program")
               : "No timer changes",
           Action::TimerApply, ButtonVariant::Primary, 0, 0, timer_dirty_);
    button({x + half + gap, cursor - scroll, half, layout.action_h},
           timer_dirty_ ? "Revert draft" : "Draft clean", Action::TimerRevert,
           ButtonVariant::Secondary, 0, 0, timer_dirty_);
    cursor += layout.action_h + gap;
    cursor = drawNote(canvas, layout,
                      "Apply replaces the whole program, including repeat and every step. The watch normalises sound; read it back before relying on a value other than Walk1.",
                      x, cursor - scroll, palette.text_secondary) +
             scroll + gap;
    button({x, cursor - scroll, w, layout.action_h},
           "Initialize empty program", Action::TimerInitialize,
           ButtonVariant::Destructive);
    cursor += layout.action_h + gap;
    cursor = drawNote(canvas, layout,
                      "Initialize only clears this draft. Nothing is written until Apply is confirmed.",
                      x, cursor - scroll, palette.text_secondary) +
             scroll;
    return cursor - top;
}

float WatchView::drawTimerStepEditor(Canvas &canvas, const Layout &layout)
{
    const auto &palette = dark_palette();
    const float scroll = page_scroll(route_index(route_));
    const float top = layout.top + layout.content_y;
    const float x = layout.left + layout.content_x;
    const float w = layout.content_w;
    const float d = touch_ ? std::max(1.0f, display_density_) : 1.0f;
    const float gap = touch_ ? 10.0f * d : 12.0f;
    const float half = (w - gap) * 0.5f;
    const float pad = 18.0f * d;
    const int index = detail_index_;
    const auto &item = edit_timer_.workouts[index];
    float cursor = top;

    auto button = [&](const Rect &rect, const std::string &label, Action action,
                      ButtonVariant variant = ButtonVariant::Secondary,
                      int arg = 0, int arg2 = 0, bool enabled = true) {
        draw_button(canvas, rect, label, variant, layout.small_size, enabled);
        if (enabled) {
            hits_.push_back({rect, action, 0, {}, arg, arg2});
        }
    };

    button({x, cursor - scroll, 96.0f * d, layout.action_h}, "Back",
           Action::CloseDetail, ButtonVariant::Quiet);
    char title[32];
    std::snprintf(title, sizeof title, "Edit step %d", index + 1);
    canvas.text(title, x + 112.0f * d,
                cursor - scroll +
                    (layout.action_h - layout.page_title_size) * 0.5f,
                layout.page_title_size, palette.text_primary);
    cursor += layout.action_h + gap * 1.5f;

    const bool empty = item.minutes == 0 && item.seconds == 0;
    const Rect hero = {x, cursor - scroll, w, layout.clock_size + 88.0f * d};
    draw_card(canvas, hero, 14.0f * d);
    canvas.text(empty ? "Empty step" : "Step duration", hero.x + pad,
                hero.y + pad, layout.small_size, palette.text_secondary);
    canvas.text(duration_text(static_cast<unsigned>(item.minutes) * 60U +
                              item.seconds),
                hero.x + pad, hero.y + pad + 26.0f * d, layout.clock_size,
                palette.text_primary);
    canvas.text(timer_sound(item.beep), hero.x + pad,
                hero.y + pad + 32.0f * d + layout.clock_size,
                layout.date_size, palette.text_secondary);
    cursor += hero.h + gap;

    canvas.text("Minutes", x, cursor - scroll, layout.small_size,
                palette.text_secondary);
    cursor += layout.small_size + 8.0f * d;
    button({x, cursor - scroll, half, layout.action_h}, "−1 minute",
           Action::TimerMinutes, ButtonVariant::Secondary, index, -1);
    button({x + half + gap, cursor - scroll, half, layout.action_h},
           "+1 minute", Action::TimerMinutes, ButtonVariant::Secondary, index,
           1);
    cursor += layout.action_h + gap;

    canvas.text("Seconds", x, cursor - scroll, layout.small_size,
                palette.text_secondary);
    cursor += layout.small_size + 8.0f * d;
    button({x, cursor - scroll, half, layout.action_h}, "−5 seconds",
           Action::TimerSeconds, ButtonVariant::Secondary, index, -5);
    button({x + half + gap, cursor - scroll, half, layout.action_h},
           "+5 seconds", Action::TimerSeconds, ButtonVariant::Secondary, index,
           5);
    cursor += layout.action_h + gap * 1.5f;

    draw_section_title(canvas, "Sound", x, cursor - scroll, layout.date_size);
    cursor += layout.date_size + 12.0f * d;
    for (int sound = 0; sound < 4; ++sound) {
        const int row = sound / 2;
        const int column = sound % 2;
        const int delta = (sound - static_cast<int>(item.beep)) & 3;
        button({x + column * (half + gap), cursor - scroll + row *
                   (layout.action_h + gap), half, layout.action_h},
               timer_sound(static_cast<uint8_t>(sound)), Action::TimerBeep,
               (item.beep & 3) == sound ? ButtonVariant::Primary
                                        : ButtonVariant::Secondary,
               index, delta);
    }
    cursor += 2.0f * (layout.action_h + gap) + gap * 0.5f;

    button({x, cursor - scroll, half, layout.action_h},
           timer_dirty_
               ? (timer_confirm_ ? "Confirm replacement" : "Apply full program")
               : "No timer changes",
           Action::TimerApply, ButtonVariant::Primary, 0, 0, timer_dirty_);
    button({x + half + gap, cursor - scroll, half, layout.action_h},
           timer_dirty_ ? "Revert draft" : "Draft clean", Action::TimerRevert,
           ButtonVariant::Secondary, 0, 0, timer_dirty_);
    cursor += layout.action_h + gap;
    cursor = drawNote(canvas, layout,
                      "Back keeps this draft and confirmation. Apply replaces the complete timer and reads it back.",
                      x, cursor - scroll, palette.text_secondary) +
             scroll;
    return cursor - top;
}

} // namespace wristctl::gui
