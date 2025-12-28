#include "pipewire/context.h"
#include "pipewire/core.h"
#include "pipewire/keys.h"
#include "pipewire/main-loop.h"
#include "pipewire/node.h"
#include "pipewire/pipewire.h"
#include "pipewire/properties.h"
#include "pipewire/proxy.h"
#include "pipewire/stream.h"
#include "spa/debug/types.h"
#include "spa/param/props.h"
#include "spa/pod/builder.h"
#include "spa/pod/iter.h"
#include "spa/pod/parser.h"
#include "spa/utils/hook.h"
#include "spa/utils/type.h"
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <ostream>
#include <spa/param/audio/format-utils.h>
#include <string>
#include <unordered_map>
using std::cout;
using std::endl;
using std::string;
using std::unordered_map;

void raiseError(bool condition, string message, int status = 1) {
    if (condition) {
        cout << "Error: " << message << endl;
        exit(status);
    }
}

struct registry_event_global_data {
    struct pw_main_loop *main_loop;
    struct pw_registry *reg;
};

class VirtualAppNodesManager {
  private:
    struct virtual_nodes {};

    unordered_map<uint32_t, virtual_nodes> output_nodes;
    unordered_map<uint32_t, virtual_nodes> input_nodes;

    struct node_info {
        string app_process_binary;
        string media_class;
    };

    static void process_node_info(void *data, const struct pw_node_info *info) {
        auto *return_data = (struct node_info *)data;
        return_data->app_process_binary = spa_dict_lookup(info->props, PW_KEY_APP_PROCESS_BINARY);
        return_data->media_class = spa_dict_lookup(info->props, PW_KEY_MEDIA_CLASS);
    }

  public:
    void process_new_node(pw_registry *reg, uint32_t id, const char *type) {
        auto *proxy = (struct pw_proxy *)pw_registry_bind(reg, id, type, PW_VERSION_NODE, 0);

        node_info *return_info;
        static const struct pw_node_events node_events = {.version = PW_VERSION_NODE_EVENTS,
                                                          .info = this->process_node_info};

        struct spa_hook *listener = new spa_hook();
        pw_proxy_add_object_listener(proxy, listener, &node_events, return_info);
    }
};

static void proxy_info_event_add_virtual_streams(void *data, const struct pw_node_info *info) {
    if (info->props) {
        auto *reg_data = (struct registry_event_global_data *)data;

        struct pw_properties *context_props = pw_properties_new(PW_KEY_APP_NAME, "Your App Name", nullptr);
        struct pw_properties *stream_props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY,
                                                               "Playback", PW_KEY_APP_NAME, "Your App Name", nullptr);
        struct pw_context *virutal_context =
            pw_context_new(pw_main_loop_get_loop(reg_data->main_loop), context_props, 0);
        struct pw_core *virtual_core = pw_context_connect(virutal_context, nullptr, 0);
        struct pw_stream *stream = pw_stream_new(virtual_core, "Playback", stream_props);

        uint8_t buffer[1024];
        struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        struct spa_audio_info_raw a_info =
            SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_F32, .rate = 48000, .channels = 2);

        const struct spa_pod *params[1];
        params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &a_info);

        pw_stream_connect(stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                          (enum pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), params, 1);

        const struct spa_dict_item *item;
        spa_dict_for_each(item, info->props) { printf("%s = %s\n", item->key, item->value); }
        cout << endl;
    }
}

// TODO: currently checking volumes
static void proxy_param_props_event(void *data, int seq, uint32_t id, uint32_t index, uint32_t next,
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

            if (strcmp(props->items[i].value, "Chromium") == 0 ||
                strcmp(props->items[i].value, "Chromium input") == 0) {

                cout << "ID: " << id << endl;

                auto *proxy = (struct pw_proxy *)pw_registry_bind(reg_data->reg, id, type, PW_VERSION_NODE, 0);
                static const struct pw_node_events chromium_node_events = {.version = PW_VERSION_NODE_EVENTS,
                                                                           .info = proxy_info_event_add_virtual_streams,
                                                                           .param = proxy_param_props_event};

                uint32_t param_ids_sub[] = {SPA_PARAM_Props};
                pw_node_subscribe_params((struct pw_node *)proxy, param_ids_sub,
                                         sizeof(param_ids_sub) / sizeof(param_ids_sub[0]));

                pw_proxy_add_object_listener(proxy, new spa_hook(), &chromium_node_events, data);
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

    struct registry_event_global_data reg_data = {loop, registry};
    pw_registry_add_listener(registry, listener, &registry_events, &reg_data);

    pw_main_loop_run(loop);

    pw_proxy_destroy((struct pw_proxy *)registry);
    pw_core_disconnect(core);
    pw_context_destroy(context);
    pw_main_loop_destroy(loop);

    return 0;
}