#include "includes/audio_daemon.hpp"
#include "includes/audio_manager.hpp"
#include "includes/utils.hpp"
#include "pipewire/keys.h"
#include "pipewire/pipewire.h"
#include <build.h>

struct AudioDaemon::registry_event_global_data {
    struct pw_main_loop *main_loop;
    struct pw_registry *reg;
};

void AudioDaemon::reg_event_find_chromium_and_mic_nodes(void *data, uint32_t id, uint32_t permissions, const char *type,
                                                        uint32_t version, const struct spa_dict *props) {

    auto *reg_data = (struct registry_event_global_data *)data;

    if (strcmp(type, PW_TYPE_INTERFACE_Port) == 0) {
        AudioManager::enlist_registry_port_event(id, props);
    }

    if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {

        string source_or_sink_name = "";
        bool is_source_or_sink = false;
        for (uint32_t i = 0; i < props->n_items; i++) {

            // find microphone
            if (source_or_sink_name != "" && is_source_or_sink)
                break;

            if (strcmp(props->items[i].key, PW_KEY_NODE_DESCRIPTION) == 0) {
                source_or_sink_name = props->items[i].value;
            }
            if (strcmp(props->items[i].key, PW_KEY_MEDIA_CLASS) == 0 &&
                (strcmp(props->items[i].value, "Audio/Source") == 0 ||
                 strcmp(props->items[i].value, "Audio/Source/Virtual") == 0
                 //  ||
                 //  strcmp(props->items[i].value, "Audio/Sink") == 0 ||
                 //  strcmp(props->items[i].value, "Audio/Sink/Virtual") == 0
                 )) {
                is_source_or_sink = true;
            }
            // END FIND MIC

            // find electron nodes
            if (strcmp(props->items[i].key, PW_KEY_APP_NAME) == 0 &&
                (strcmp(props->items[i].value, "Chromium") == 0
                 // ||strcmp(props->items[i].value, "Chromium input") == 0
                 )) {

                // process electron node
                AudioManager::process_elec_node(reg_data->reg, pw_main_loop_get_loop(reg_data->main_loop), id, type);
                break;
            }
            // END FIND ELECTRON NODES
        }

        // process microphone
        if (is_source_or_sink) {
            if (source_or_sink_name.find(PROJECT_NAME) == string::npos) {
                // AudioManager::process_mic_node(reg_data->reg, pw_main_loop_get_loop(reg_data->main_loop), id, type);
            }
        }
    }
}

void AudioDaemon::on_global_remove(void *data, uint32_t id) {
    AudioManager::enlist_registry_node_remove_event(id);
}

void AudioDaemon::start() {
    Utils::check_lock();

    pw_init(nullptr, nullptr);

    // getting context to connect to pipewire daemon
    pw_main_loop *loop = pw_main_loop_new(nullptr);
    pw_context *context = pw_context_new(pw_main_loop_get_loop(loop), nullptr, 0);

    // connect context to daemon
    pw_core *core = pw_context_connect(context, nullptr, 0);
    Utils::raise_error(core == nullptr, string("failed to connect to pipewire daemon, ") + strerror(errno), errno);

    pw_registry *registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);

    spa_hook *listener = new spa_hook();

    static const pw_registry_events registry_events = {
        .version = PW_VERSION_REGISTRY_EVENTS,
        .global = reg_event_find_chromium_and_mic_nodes,
        .global_remove = on_global_remove,
    };

    AudioDaemon::registry_event_global_data reg_data = {loop, registry};
    pw_registry_add_listener(registry, listener, &registry_events, &reg_data);

    pw_main_loop_run(loop);

    pw_proxy_destroy((pw_proxy *)registry);
    pw_core_disconnect(core);
    pw_context_destroy(context);
    pw_main_loop_destroy(loop);

    spa_hook_remove(listener);
    delete listener;
    listener = nullptr;

    AudioManager::cleanup();
}