#pragma once

#include <spa/param/audio/format-utils.h>

class AudioDaemon {
  private:
    struct registry_event_global_data;
    static void reg_event_find_chromium_and_mic_nodes(void *data, uint32_t id, uint32_t permissions, const char *type,
                                                      uint32_t version, const struct spa_dict *props);

    static void on_global_remove(void *data, uint32_t id);

  public:
    static void start();
};