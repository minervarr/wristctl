#include <cstdio>
#include <cstring>

#include "app_main.hh"
#include "watch_view.hh"

namespace {

void usage()
{
    std::fputs(
        "usage: wristctl_gui [--connect] [--help]\n"
        "\n"
        "Six pages: Watch, Clock, Alarms, Timer, Notify, Console.\n"
        "Starts on the recorded demo session; --connect scans immediately.\n"
        "\n"
        "keys: 1-6 pages, C connect, R refresh, T sync time, Esc quit\n",
        stderr);
}

} // namespace

int app_shell_main(int argc, char **argv)
{
    bool connect = false;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--help") == 0 ||
            std::strcmp(argv[i], "-h") == 0) {
            app_shell_restore_console();
            usage();
            return 0;
        }
        if (std::strcmp(argv[i], "--connect") == 0) {
            connect = true;
            continue;
        }
        std::fprintf(stderr, "error: unknown option: %s\n", argv[i]);
        usage();
        return 2;
    }

    wristctl::gui::WatchView view;
    if (!view.create()) {
        return 1;
    }
    if (connect) {
        view.connect_now();
    }
    view.run();
    return 0;
}
