#include "watch_view.hh"

#include <algorithm>
#include <cstdio>
#include <ctime>

#include "canvas.hh"
#include "sntp_client.hh"
#include "watch_model.hh"

namespace wristctl::gui {

float WatchView::drawSectionTitle(Canvas &canvas, const Layout &layout,
                                  const std::string &title, float x, float y)
{
    canvas.text(title, x, y, layout.page_title_size, col::text);
    return y + layout.page_title_size * 1.6f;
}

float WatchView::drawKeyValue(Canvas &canvas, const Layout &layout,
                              const std::string &label,
                              const std::string &value, float x, float y,
                              Color color)
{
    canvas.text(label, x, y, layout.small_size, col::dim);
    canvas.text(truncateToWidth(canvas, value,
                                layout.content_w - layout.label_w,
                                layout.text_size),
                x + layout.label_w, y, layout.text_size, color);
    return y + layout.row_line;
}

float WatchView::drawNote(Canvas &canvas, const Layout &layout,
                          const std::string &text, float x, float y,
                          Color color)
{
    return drawWrapped(canvas, text, x, y, layout.content_w, layout.small_size,
                       color, layout.small_size * 1.4f);
}

} // namespace wristctl::gui
