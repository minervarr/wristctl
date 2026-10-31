#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "app_main.hh"
#include "watch_view.hh"

namespace {

void usage()
{
    std::fputs(
        "usage: wristctl_gui [--connect] [--no-connect] [--timezone ZONE] "
        "[--help]\n"
        "\n"
        "Six pages: Watch, Clock, Alarms, Timer, Notify, Console.\n"
        "Connects to the watch automatically and keeps retrying; --no-connect\n"
        "starts on the recorded demo session instead. --connect forces an\n"
        "immediate attempt (the default is already auto-connect).\n"
        "ZONE is an IANA timezone such as Etc/GMT+5; --timezone overrides\n"
        "$WRISTCTL_TIMEZONE. If neither is set, the host timezone is used.\n"
        "\n"
        "keys: 1-6 pages, C connect/auto, R refresh, T sync time, Esc quit\n",
        stderr);
}

} // namespace

int app_shell_main(int argc, char **argv)
{
    bool auto_connect = true;
    bool connect = false;
    const char *timezone = nullptr;
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
        if (std::strcmp(argv[i], "--no-connect") == 0 ||
            std::strcmp(argv[i], "--demo") == 0) {
            auto_connect = false;
            continue;
        }
        if (std::strcmp(argv[i], "--timezone") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: --timezone needs a zone\n");
                usage();
                return 2;
            }
            timezone = argv[++i];
            continue;
        }
        if (std::strncmp(argv[i], "--timezone=", 11) == 0) {
            timezone = argv[i] + 11;
            continue;
        }
        std::fprintf(stderr, "error: unknown option: %s\n", argv[i]);
        usage();
        return 2;
    }

    if (timezone == nullptr) {
        timezone = std::getenv("WRISTCTL_TIMEZONE");
    }
    if (timezone != nullptr && *timezone != '\0') {
        if (setenv("TZ", timezone, 1) != 0) {
            std::fprintf(stderr, "error: cannot apply timezone %s\n", timezone);
            return 2;
        }
        tzset();
    }

    wristctl::gui::WatchView view(auto_connect, !auto_connect);
    if (!view.create()) {
        return 1;
    }
    if (connect) {
        view.connect_now();
    }
    view.run();
    return 0;
}
