#pragma once

#include <build.h>
#include <string>
#include <unordered_map>
#include <vector>

using std::string;
using std::unordered_map;
using std::vector;

class CLI {
  private:
    struct flags {
        enum class OPTION_INFO {
            FULL,
            SHORT,
            TYPE,
            DESC,
        };

        enum OPTION_ORDER {
            HELP,
            VERSION,
            DAEMON,
            VOLUME_DAEMON,
            AUDIO_DAEMON,
        };

        static vector<unordered_map<OPTION_INFO, string>> options;
    };

    struct config {
        enum class DAEMON_TYPE_ORDER {
            VOLUME,
            AUDIO,
        };

        static vector<string> daemon_type_values;
    };

    static void unknown_flag(const char *flag);
    // static void unknownValue(unordered_map<Flags::OPTION_POS, string> option, const char *value, string expected);
    static void print_help();
    static void print_version();
    static void start_config_daemon();
    static bool check_flag_condition(const char *arg, unordered_map<flags::OPTION_INFO, string> option);

  public:
    static void check_flags(int argc, char *argv[]);
};