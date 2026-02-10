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
    struct Flags {
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
        };

        static vector<unordered_map<OPTION_INFO, string>> options;
    };

    static void unknown_flag(const char *flag);
    // static void unknownValue(unordered_map<Flags::OPTION_POS, string> option, const char *value, string expected);
    static void print_help();
    static void print_version();
    static bool check_flag_condition(const char *arg, unordered_map<Flags::OPTION_INFO, string> option);

  public:
    static void check_flags(int argc, char *argv[]);
};