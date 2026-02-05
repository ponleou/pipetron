#include "daemon.hpp"
#include <config.h>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using std::cout;
using std::endl;
using std::invalid_argument;
using std::left;
using std::setw;
using std::stoi;
using std::strcmp;
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

        inline static vector<unordered_map<OPTION_INFO, string>> options = {
            {{OPTION_INFO::SHORT, "-h"},
             {OPTION_INFO::FULL, "--help"},
             {OPTION_INFO::TYPE, ""},
             {OPTION_INFO::DESC, "Show this help message"}},

            {{OPTION_INFO::SHORT, "-v"},
             {OPTION_INFO::FULL, "--version"},
             {OPTION_INFO::TYPE, ""},
             {OPTION_INFO::DESC, "Show version"}},

            {{OPTION_INFO::SHORT, "-d"},
             {OPTION_INFO::FULL, "--daemon"},
             {OPTION_INFO::TYPE, ""},
             {OPTION_INFO::DESC, "Start daemon"}},
        };
    };

    static void unknownFlag(const char *flag) {
        cout << "Error: unknown option " << "\"" << flag << "\"" << endl;
        exit(1);
    }

    // static void unknownValue(unordered_map<Flags::OPTION_POS, string> option, const char *value, string expected) {
    //     cout << "Error: unknown value for " << option[Flags::OPTION_POS::SHORT] << ", " <<
    //     option[Flags::OPTION_POS::FULL]
    //          << " flag: " << "\"" << value << "\"" << ", expected " << expected << endl;
    //     exit(1);
    // }

    static void printHelp() {
        cout << "Usage: " << PROJECT_NAME << " [options]\n\n";
        cout << "Options:\n";

        size_t maxShortFull = 0;
        for (auto &flag : Flags::options) {
            size_t len = flag[Flags::OPTION_INFO::SHORT].length() + 2 + flag[Flags::OPTION_INFO::FULL].length() + 1 +
                         flag[Flags::OPTION_INFO::TYPE].length();
            if (len > maxShortFull)
                maxShortFull = len;
        }

        const int extraGap = 3;

        for (auto &flag : Flags::options) {
            string shortFull = flag[Flags::OPTION_INFO::SHORT] + ", " + flag[Flags::OPTION_INFO::FULL] + " " +
                               flag[Flags::OPTION_INFO::TYPE];
            cout << "  " << left << setw(maxShortFull + extraGap) << shortFull << flag[Flags::OPTION_INFO::DESC]
                 << endl;
        }

        exit(0);
    }

    static void printVersion() {
        cout << PROJECT_NAME << " version " << PROJECT_VERSION << " (" << PROJECT_LICENSE << " License)" << endl;

        exit(0);
    }

    static bool checkFlagCondition(const char *arg, unordered_map<Flags::OPTION_INFO, string> option) {
        return strcmp(arg, option[Flags::OPTION_INFO::SHORT].c_str()) == 0 ||
               strcmp(arg, option[Flags::OPTION_INFO::FULL].c_str()) == 0;
    }

  public:
    static void checkFlags(int argc, char *argv[]) {
        auto options = Flags::options;

        for (int i = 1; i < argc; i++) {
            if (checkFlagCondition(argv[i], options[Flags::HELP]))
                printHelp();
            else if (checkFlagCondition(argv[i], options[Flags::VERSION])) {
                printVersion();
            } else if (checkFlagCondition(argv[i], options[Flags::DAEMON])) {
                Daemon::start();
            } else
                unknownFlag(argv[i]);
        }

        if (argc == 0) {
            printHelp();
        }
    }
};