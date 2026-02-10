#include "includes/volume_daemon.hpp"
#include "includes/utils.hpp"
#include "includes/volume_manager.hpp"
#include "pipewire/context.h"
#include "pipewire/core.h"
#include "pipewire/keys.h"
#include "pipewire/main-loop.h"
#include "pipewire/pipewire.h"
#include "pipewire/proxy.h"
#include "spa/utils/dict.h"
#include "spa/utils/hook.h"
#include <build.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <spa/param/audio/format-utils.h>
#include <string>
using std::string;

void VolumeDaemon::reg_event_find_chromium_nodes(void *data, uint32_t id, uint32_t permissions, const char *type,
                                                 uint32_t version, const struct spa_dict *props) {

    auto *reg_data = (struct registry_event_global_data *)data;

    if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
        for (uint32_t i = 0; i < props->n_items; i++) {

            if (strcmp(props->items[i].key, PW_KEY_APP_NAME) != 0)
                continue;

            if (strcmp(props->items[i].value, "Chromium") == 0 ||
                strcmp(props->items[i].value, "Chromium input") == 0) {

                VolumeManager::process_new_node(reg_data->reg, pw_main_loop_get_loop(reg_data->main_loop), id, type);
            }

            break;
        }
    }
}

void VolumeDaemon::start() {
    Utils::check_lock();

    pw_init(nullptr, nullptr);

    // getting context to connect to pipewire daemon
    struct pw_main_loop *loop = pw_main_loop_new(nullptr);
    struct pw_context *context = pw_context_new(pw_main_loop_get_loop(loop), nullptr, 0);

    // connect context to daemon
    struct pw_core *core = pw_context_connect(context, nullptr, 0);
    Utils::raise_error(core == nullptr, string("failed to connect to pipewire daemon, ") + strerror(errno), errno);

    struct pw_registry *registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);

    struct spa_hook *listener = new spa_hook();

    static const struct pw_registry_events registry_events = {
        .version = PW_VERSION_REGISTRY_EVENTS,
        .global = reg_event_find_chromium_nodes,
        .global_remove = VolumeManager::on_global_remove,
    };

    struct VolumeDaemon::registry_event_global_data reg_data = {loop, registry};
    pw_registry_add_listener(registry, listener, &registry_events, &reg_data);

    pw_main_loop_run(loop);

    pw_proxy_destroy((struct pw_proxy *)registry);
    pw_core_disconnect(core);
    pw_context_destroy(context);
    pw_main_loop_destroy(loop);

    spa_hook_remove(listener);
    delete listener;
    listener = nullptr;

    VolumeManager::cleanup();
}
