#include "includes/cli.hpp"
#include "includes/audio_daemon.hpp"
#include "includes/volume_daemon.hpp"
#include <build.h>
#include <cstring>
#include <iomanip>
#include <iostream>

using std::cout;
using std::endl;
using std::left;
using std::setw;
using std::strcmp;

void CLI::unknown_flag(const char *flag) {
    cout << "Error: unknown option \"" << flag << "\"" << endl;
    exit(1);
}

void CLI::print_help() {
    cout << "Usage: " << PROJECT_NAME << " [options]\n\n";
    cout << "Options:\n";
    size_t max_short_full = 0;
    for (auto &flag : Flags::options) {
        size_t len = flag[Flags::OPTION_INFO::SHORT].length() + 2 + flag[Flags::OPTION_INFO::FULL].length() + 1 +
                     flag[Flags::OPTION_INFO::TYPE].length();
        if (len > max_short_full)
            max_short_full = len;
    }
    const int extraGap = 3;
    for (auto &flag : Flags::options) {
        string shortFull = flag[Flags::OPTION_INFO::SHORT] + ", " + flag[Flags::OPTION_INFO::FULL] + " " +
                           flag[Flags::OPTION_INFO::TYPE];
        cout << "  " << left << setw(max_short_full + extraGap) << shortFull << flag[Flags::OPTION_INFO::DESC] << endl;
    }
}

void CLI::print_version() {
    cout << PROJECT_NAME << " version " << PROJECT_VERSION << " (" << PROJECT_LICENSE << " License)" << endl;
}

bool CLI::check_flag_condition(const char *arg, unordered_map<Flags::OPTION_INFO, string> option) {
    return strcmp(arg, option[Flags::OPTION_INFO::SHORT].c_str()) == 0 ||
           strcmp(arg, option[Flags::OPTION_INFO::FULL].c_str()) == 0;
}

vector<unordered_map<CLI::Flags::OPTION_INFO, string>> CLI::Flags::options = {
    {{CLI::Flags::OPTION_INFO::SHORT, "-h"},
     {CLI::Flags::OPTION_INFO::FULL, "--help"},
     {CLI::Flags::OPTION_INFO::TYPE, ""},
     {CLI::Flags::OPTION_INFO::DESC, "Show this help message"}},
    {{CLI::Flags::OPTION_INFO::SHORT, "-v"},
     {CLI::Flags::OPTION_INFO::FULL, "--version"},
     {CLI::Flags::OPTION_INFO::TYPE, ""},
     {CLI::Flags::OPTION_INFO::DESC, "Show version"}},
    {{CLI::Flags::OPTION_INFO::SHORT, "-vd"},
     {CLI::Flags::OPTION_INFO::FULL, "--volume-daemon"},
     {CLI::Flags::OPTION_INFO::TYPE, ""},
     {CLI::Flags::OPTION_INFO::DESC, "Start daemon"}},
    {{CLI::Flags::OPTION_INFO::SHORT, "-ad"},
     {CLI::Flags::OPTION_INFO::FULL, "--audio_daemon"},
     {CLI::Flags::OPTION_INFO::TYPE, ""},
     {CLI::Flags::OPTION_INFO::DESC, "Start daemon"}},
};

void CLI::check_flags(int argc, char *argv[]) {
    auto options = Flags::options;
    for (int i = 1; i < argc; i++) {
        if (check_flag_condition(argv[i], options[Flags::HELP])) {
            print_help();
        } else if (check_flag_condition(argv[i], options[Flags::VERSION])) {
            print_version();
        } else if (check_flag_condition(argv[i], options[Flags::DAEMON])) {
            VolumeDaemon::start();
        } else if (check_flag_condition(argv[i], options[Flags::AUDIO])) {
            AudioDaemon::start();
        } else
            unknown_flag(argv[i]);
    }
    if (argc < 2) {
        print_version();
        print_help();
    }
}