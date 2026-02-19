#include "includes/cli.hpp"
#include "includes/audio_daemon.hpp"
#include "includes/volume_daemon.hpp"
#include <build.h>
#include <cstring>
#include <exception>
#include <iomanip>
#include <iostream>
#include <toml++/toml.hpp>

using std::cout;
using std::endl;
using std::exception;
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
    for (auto &flag : flags::options) {
        size_t len = flag[flags::OPTION_INFO::SHORT].length() + 2 + flag[flags::OPTION_INFO::FULL].length() + 1 +
                     flag[flags::OPTION_INFO::TYPE].length();
        if (len > max_short_full)
            max_short_full = len;
    }
    const int extraGap = 3;
    for (auto &flag : flags::options) {
        string shortFull = flag[flags::OPTION_INFO::SHORT] + ", " + flag[flags::OPTION_INFO::FULL] + " " +
                           flag[flags::OPTION_INFO::TYPE];
        cout << "  " << left << setw(max_short_full + extraGap) << shortFull << flag[flags::OPTION_INFO::DESC] << endl;
    }
}

void CLI::print_version() {
    cout << PROJECT_NAME << " version " << PROJECT_VERSION << " (" << PROJECT_LICENSE << " License)" << endl;
}

bool CLI::check_flag_condition(const char *arg, unordered_map<flags::OPTION_INFO, string> option) {
    return strcmp(arg, option[flags::OPTION_INFO::SHORT].c_str()) == 0 ||
           strcmp(arg, option[flags::OPTION_INFO::FULL].c_str()) == 0;
}

vector<string> CLI::config::daemon_type_values = {
    "volume",
    "audio",
};

vector<unordered_map<CLI::flags::OPTION_INFO, string>> CLI::flags::options = {
    {{CLI::flags::OPTION_INFO::SHORT, "-h"},
     {CLI::flags::OPTION_INFO::FULL, "--help"},
     {CLI::flags::OPTION_INFO::TYPE, ""},
     {CLI::flags::OPTION_INFO::DESC, "Show this help message"}},
    {{CLI::flags::OPTION_INFO::SHORT, "-v"},
     {CLI::flags::OPTION_INFO::FULL, "--version"},
     {CLI::flags::OPTION_INFO::TYPE, ""},
     {CLI::flags::OPTION_INFO::DESC, "Show version"}},
    {{CLI::flags::OPTION_INFO::SHORT, "-d"},
     {CLI::flags::OPTION_INFO::FULL, "--daemon"},
     {CLI::flags::OPTION_INFO::TYPE, ""},
     {CLI::flags::OPTION_INFO::DESC, "Start daemon specified in config file (defaults to " +
                                         CLI::config::daemon_type_values[0] + " daemon if not specified)"}},
    {{CLI::flags::OPTION_INFO::SHORT, "-vd"},
     {CLI::flags::OPTION_INFO::FULL, "--volume-daemon"},
     {CLI::flags::OPTION_INFO::TYPE, ""},
     {CLI::flags::OPTION_INFO::DESC,
      "Start volume daemon (mirror volume settings), daemon type \"volume\" in config file"}},
    {{CLI::flags::OPTION_INFO::SHORT, "-ad"},
     {CLI::flags::OPTION_INFO::FULL, "--audio-daemon"},
     {CLI::flags::OPTION_INFO::TYPE, ""},
     {CLI::flags::OPTION_INFO::DESC, "Start audio daemon (mirror audio data), daemon type \"audio\" in config file"}},
};

void CLI::start_config_daemon() {
    string config_dir = getenv("XDG_CONFIG_HOME") ?: string(getenv("HOME")) + "/.config";
    string config_path = config_dir + "/" + PROJECT_NAME + "/config.toml";

    string daemon_type = config::daemon_type_values[0];

    try {
        auto config = toml::parse_file(config_path);
        daemon_type = config["daemon"]["type"].value_or(config::daemon_type_values[0]);
    } catch (const exception &e) {
        cout << "Failed to read config file " + config_path + ": " + e.what() + ", running default" << endl;
    }

    if (daemon_type == config::daemon_type_values[(int)config::DAEMON_TYPE_ORDER::VOLUME]) {
        VolumeDaemon::start();
    } else if (daemon_type == config::daemon_type_values[(int)config::DAEMON_TYPE_ORDER::AUDIO]) {
        AudioDaemon::start();
    } else {
        cout << "Unknown daemon type: \"" + daemon_type + "\", running default" << endl;
        // checking default (index = 0)
        if ((int)config::DAEMON_TYPE_ORDER::VOLUME == 0) {
            VolumeDaemon::start();
        } else if ((int)config::DAEMON_TYPE_ORDER::AUDIO == 0) {
            AudioDaemon::start();
        }
    }
}

void CLI::check_flags(int argc, char *argv[]) {
    auto options = flags::options;
    for (int i = 1; i < argc; i++) {
        if (check_flag_condition(argv[i], options[flags::HELP])) {
            print_help();
        } else if (check_flag_condition(argv[i], options[flags::VERSION])) {
            print_version();
        } else if (check_flag_condition(argv[i], options[flags::DAEMON])) {
            start_config_daemon();
        } else if (check_flag_condition(argv[i], options[flags::VOLUME_DAEMON])) {
            VolumeDaemon::start();
        } else if (check_flag_condition(argv[i], options[flags::AUDIO_DAEMON])) {
            AudioDaemon::start();
        } else
            unknown_flag(argv[i]);
    }
    if (argc < 2) {
        print_version();
        print_help();
    }
}