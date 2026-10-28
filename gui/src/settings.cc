#include "settings.hh"

#include <cstdlib>
#include <fstream>
#include <string>

#include "arc/fs/paths.hh"

namespace wristctl::gui {
namespace {

constexpr const char *kAppName = "wristctl";
constexpr const char *kFileName = "gui.conf";

std::string config_path()
{
    const arc::fs::Layout layout = arc::fs::layout(kAppName);
    arc::fs::ensureLayout(layout, arc::fs::LayoutScope::App);
    return arc::fs::join(layout.config, kFileName);
}

} // namespace

GuiSettings load_settings()
{
    GuiSettings settings;
    std::ifstream input(config_path());
    std::string line;
    while (std::getline(input, line)) {
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, equals);
        const std::string value = line.substr(equals + 1);
        if (key == "page") {
            const int page = std::atoi(value.c_str());
            if (page >= 0 && page < 6) {
                settings.page = page;
            }
        }
    }
    return settings;
}

void save_settings(const GuiSettings &settings)
{
    std::ofstream output(config_path(), std::ios::trunc);
    if (!output) {
        return;
    }
    output << "page=" << settings.page << "\n";
}

} // namespace wristctl::gui
