#pragma once

#include <spa/param/audio/format-utils.h>
#include <string>
using std::string;

class VolumeDaemon {
  private:
    struct registry_event_global_data {
        struct pw_main_loop *main_loop;
        struct pw_registry *reg;
    };
    static void reg_event_find_chromium_nodes(void *data, uint32_t id, uint32_t permissions, const char *type,
                                              uint32_t version, const struct spa_dict *props);

  public:
    static void start();
};
