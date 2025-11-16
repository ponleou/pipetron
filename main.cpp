#include "pipewire/context.h"
#include "pipewire/core.h"
#include "pipewire/main-loop.h"
#include "pipewire/node.h"
#include "pipewire/pipewire.h"
#include "pipewire/proxy.h"
#include "spa/debug/types.h"
#include "spa/param/props.h"
#include "spa/pod/iter.h"
#include "spa/pod/parser.h"
#include "spa/utils/hook.h"
#include "spa/utils/type.h"
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <ostream>
using std::cout;
using std::endl;
using std::string;

void raiseError(bool condition, string message, int status = 1) {
    if (condition) {
        cout << "Error: " << message << endl;
        exit(status);
    }
}

struct registry_event_global_data {
    struct pw_registry *reg;
};

static void chromium_proxy_info_event(void *data, const struct pw_node_info *info) {
    if (info->props) {
        const struct spa_dict_item *item;
        spa_dict_for_each(item, info->props) { printf("%s = %s\n", item->key, item->value); }
    }
}

static void chromium_proxy_param_event(void *data, int seq, uint32_t id, uint32_t index, uint32_t next,
                                       const struct spa_pod *param) {
    if (id == SPA_PARAM_Props) {
        float volume = 0.0f;
        bool mute = false;
        struct spa_pod *channel_vols = nullptr;
        struct spa_pod *channel_map = nullptr;

        spa_pod_parse_object(param, SPA_TYPE_OBJECT_Props, NULL, SPA_PROP_volume, SPA_POD_OPT_Float(&volume),
                             SPA_PROP_mute, SPA_POD_OPT_Bool(&mute), SPA_PROP_channelVolumes,
                             SPA_POD_OPT_Pod(&channel_vols), SPA_PROP_channelMap, SPA_POD_OPT_Pod(&channel_map));

        printf("volume: %f, mute: %d\n", volume, mute);

        if (channel_vols) {
            uint32_t n_vals;
            float *vals = (float *)spa_pod_get_array(channel_vols, &n_vals);
            printf("channelVolumes: ");
            for (uint32_t i = 0; i < n_vals; i++) {
                printf("%f ", vals[i]);
            }
            printf("\n");
        }

        if (channel_map) {
            uint32_t n_channels;
            uint32_t *channels = (uint32_t *)spa_pod_get_array(channel_map, &n_channels);
            printf("channelMap: ");
            for (uint32_t i = 0; i < n_channels; i++) {
                const char *name = spa_debug_type_find_name(spa_type_audio_channel, channels[i]);
                printf("%s ", name ? name : "UNKNOWN");
            }
            printf("\n");
        }
    }
}

static void reg_event_set_chromium_listeners(void *data, uint32_t id, uint32_t permissions, const char *type,
                                             uint32_t version, const struct spa_dict *props) {

    auto *reg_data = (struct registry_event_global_data *)data;

    if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
        for (uint32_t i = 0; i < props->n_items; i++) {
            if (strcmp(props->items[i].key, "application.name") != 0)
                continue;

            if (strcmp(props->items[i].value, "Chromium") == 0) {

                auto *proxy = (struct pw_proxy *)pw_registry_bind(reg_data->reg, id, type, PW_VERSION_NODE, 0);
                static const struct pw_node_events chromium_node_events = {.version = PW_VERSION_NODE_EVENTS,
                                                                           .info = chromium_proxy_info_event,
                                                                           .param = chromium_proxy_param_event};

                uint32_t param_ids_sub[] = {SPA_PARAM_Props};
                pw_node_subscribe_params((struct pw_node *)proxy, param_ids_sub,
                                         sizeof(param_ids_sub) / sizeof(param_ids_sub[0]));

                pw_proxy_add_object_listener(proxy, new spa_hook(), &chromium_node_events, nullptr);
            }

            break;
        }
    }
}

int main() {
    pw_init(nullptr, nullptr);

    // getting context to connect to pipewire daemon
    struct pw_main_loop *loop = pw_main_loop_new(nullptr);
    struct pw_context *context = pw_context_new(pw_main_loop_get_loop(loop), nullptr, 0);

    // connect context to daemon
    struct pw_core *core = pw_context_connect(context, nullptr, 0);
    raiseError(core == nullptr, string("failed to connect to pipewire daemon, ") + strerror(errno), errno);

    struct pw_registry *registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);

    struct spa_hook *listener = new spa_hook();

    static const struct pw_registry_events registry_events = {
        .version = PW_VERSION_REGISTRY_EVENTS,
        .global = reg_event_set_chromium_listeners,
    };

    struct registry_event_global_data reg_data = {registry};
    pw_registry_add_listener(registry, listener, &registry_events, &reg_data);

    pw_main_loop_run(loop);

    pw_proxy_destroy((struct pw_proxy *)registry);
    pw_core_disconnect(core);
    pw_context_destroy(context);
    pw_main_loop_destroy(loop);

    return 0;
}