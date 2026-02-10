#pragma once

#include <string>
using std::string;

class Utils {
  public:
    static void raise_error(bool condition, string message, int status);
    static void check_lock();
};