#include "includes/utils.hpp"
#include <build.h>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sys/file.h>
using std::cerr;
using std::endl;

void Utils::raise_error(bool condition, string message, int status = 1) {
    if (condition) {
        cerr << "Error: " << message << endl;
        exit(status);
    }
}

void Utils::check_lock() {
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    raise_error(!runtime_dir, "XDG_RUNTIME_DIR not set");

    string lock_file = string(runtime_dir) + "/" + PROJECT_NAME + ".lock";

    int file = open(lock_file.c_str(), O_CREAT | O_RDWR, 0644);
    raise_error(file == -1, string("Open ") + lock_file + ": " + strerror(errno), errno);

    raise_error(flock(file, LOCK_EX | LOCK_NB) != 0, string("Lock file exists " + lock_file + ": ") + strerror(errno),
                errno);
}