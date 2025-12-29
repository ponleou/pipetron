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
#include "spa/param/param.h"
#include "spa/param/props.h"
#include "spa/pod/builder.h"
#include "spa/pod/iter.h"
#include "spa/pod/parser.h"
#include "spa/utils/dict.h"
#include "spa/utils/hook.h"
#include "spa/utils/type.h"
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <ostream>
#include <queue>
#include <spa/param/audio/format-utils.h>
#include <string>
#include <unordered_map>
#include <vector>
using std::cout;
using std::endl;
using std::function;
using std::queue;
using std::string;
using std::unordered_map;
using std::vector;

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

/*
TODO:
- maintain chromium binary -> node id information
- ensure all chormium binary has a virtual device, if not, create
- ensure all virtual device has a chorium binary, if not, delete

actually, just make a virtual device in sync with one chromium device
*/
class VirtualAppNodesManager {
  private:
    struct virtual_nodes {
        pw_context *context;
        pw_core *core;
        pw_stream *stream;
    };

    inline static vector<virtual_nodes> nodes = {};
    unordered_map<uint32_t, virtual_nodes> output_nodes;
    unordered_map<uint32_t, virtual_nodes> input_nodes;

    struct node_info {
        pw_loop *loop;
        string app_process_binary;
        string media_class;
        string media_name;
        spa_audio_info_raw audio_info;
    };

    struct listener_data {
        node_info info;
        spa_hook *listener;
        queue<function<void(node_info)>> post_hook;
        bool info_flag;
        bool params_flag;
    };

    static void process_node_info(void *data, const struct pw_node_info *info) {
        auto *listener_data = (struct listener_data *)data;

        if (listener_data->info_flag)
            return;

        const char *app_process_binary = spa_dict_lookup(info->props, PW_KEY_APP_PROCESS_BINARY);
        const char *media_class = spa_dict_lookup(info->props, PW_KEY_MEDIA_CLASS);
        const char *media_name = spa_dict_lookup(info->props, PW_KEY_MEDIA_NAME);

        listener_data->info.app_process_binary = app_process_binary ? string(app_process_binary) : "";
        listener_data->info.media_class = media_class ? string(media_class) : "";
        listener_data->info.media_name = media_name ? string(media_name) : "";

        listener_data->info_flag = true;

        maybe_run_post_hook(listener_data);
    }

    static void process_node_param(void *data, int seq, uint32_t id, uint32_t index, uint32_t next,
                                   const struct spa_pod *param) {
        auto *listener_data = (struct listener_data *)data;

        if (listener_data->params_flag)
            return;

        if (id == SPA_PARAM_Format) {
            auto *node_info = &listener_data->info;
            spa_format_audio_raw_parse(param, &node_info->audio_info);
            listener_data->params_flag = true;

            maybe_run_post_hook(listener_data);
        }
    }

    static void maybe_run_post_hook(listener_data *data) {
        if (data->info_flag && data->params_flag) {
            spa_hook_remove(data->listener);
            delete data->listener;

            while (!data->post_hook.empty()) {
                function<void(node_info)> func = data->post_hook.front();
                data->post_hook.pop();
                func(data->info);
            }
        }
    }

    static void create_virtual_node(node_info info) {
        cout << "create node" << endl;
        cout << "app_process_binary: " << (info.app_process_binary.c_str() ? info.app_process_binary : "NULL") << endl;
        cout << "media_class: " << (info.media_class.c_str() ? info.media_class : "NULL") << endl;
        cout << "media_name: " << (info.media_name.c_str() ? info.media_name : "NULL") << endl;
        cout << "rate: " << info.audio_info.rate << " channels: " << info.audio_info.channels << endl;

        struct pw_properties *context_props =
            pw_properties_new(PW_KEY_APP_NAME, info.app_process_binary.c_str(), nullptr);
        struct pw_context *virtual_context = pw_context_new(info.loop, context_props, 0);
        struct pw_core *virtual_core = pw_context_connect(virtual_context, nullptr, 0);

        struct pw_properties *stream_props = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_APP_NAME, info.app_process_binary.c_str(), PW_KEY_MEDIA_CLASS,
            info.media_class.c_str(), PW_KEY_APP_ICON_NAME, info.app_process_binary.c_str(), nullptr);
        struct pw_stream *virtual_stream = pw_stream_new(virtual_core, info.media_name.c_str(), stream_props);

        uint8_t buffer[1024];
        struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        const struct spa_pod *params[1];
        params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info.audio_info);

        pw_stream_connect(virtual_stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                          (enum pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), params, 1);

        virtual_nodes node = {.context = virtual_context, .core = virtual_core, .stream = virtual_stream};
        VirtualAppNodesManager::nodes.push_back(node);
    }

  public:
    static void process_new_node(pw_registry *reg, pw_loop *loop, uint32_t id, const char *type) {

        auto *node = (struct pw_proxy *)pw_registry_bind(reg, id, type, PW_VERSION_NODE, 0);

        listener_data *data = new listener_data();
        static const struct pw_node_events node_events = {
            .version = PW_VERSION_NODE_EVENTS,
            .info = VirtualAppNodesManager::process_node_info,
            .param = VirtualAppNodesManager::process_node_param,
        };

        struct spa_hook *listener = new spa_hook();

        data->listener = listener;
        data->info_flag = false;
        data->params_flag = false;
        data->info = {
            .loop = loop,
            .app_process_binary = "",
            .media_class = "",
            .media_name = "",
            .audio_info = {},
        };
        data->post_hook = {};

        auto create_virtual_dev_func = [](VirtualAppNodesManager::node_info info) {
            VirtualAppNodesManager::create_virtual_node(info);
        };
        data->post_hook.push(create_virtual_dev_func);

        uint32_t param_ids_sub[] = {SPA_PARAM_Props, SPA_PARAM_Format};
        pw_node_subscribe_params((struct pw_node *)node, param_ids_sub,
                                 sizeof(param_ids_sub) / sizeof(param_ids_sub[0]));

        pw_proxy_add_object_listener(node, listener, &node_events, data);

        pw_node_enum_params((struct pw_node *)node, 0, SPA_PARAM_Format, 0, UINT32_MAX, nullptr);
        pw_node_enum_params((struct pw_node *)node, 0, SPA_PARAM_Props, 0, UINT32_MAX, nullptr);
    }
};

static void proxy_info_event_add_virtual_streams(void *data, const struct pw_node_info *info) {
    if (info->props) {
        auto *reg_data = (struct registry_event_global_data *)data;

        struct pw_properties *context_props = pw_properties_new(PW_KEY_APP_NAME, "Your App Name", nullptr);
        struct pw_properties *stream_props =
            pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_APP_NAME, "Your App Name", nullptr);
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

    if (id == SPA_PARAM_Format) {
        struct spa_audio_info_raw info = {};
        spa_format_audio_raw_parse(param, &info);

        printf("Rate: %u\n", info.rate);
        printf("Channels: %u\n", info.channels);
        printf("Format: %u\n", info.format);
        printf("Flags: %u\n", info.flags);
        printf("Position: ");
        for (uint32_t i = 0; i < info.channels; i++) {
            printf("%u ", info.position[i]);
        }
        printf("\n");
    }

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

            if (strcmp(props->items[i].key, PW_KEY_APP_NAME) != 0)
                continue;

            if (strcmp(props->items[i].value, "Chromium") == 0 ||
                strcmp(props->items[i].value, "Chromium input") == 0) {

                VirtualAppNodesManager::process_new_node(reg_data->reg, pw_main_loop_get_loop(reg_data->main_loop), id,
                                                         type);

                // cout << "ID: " << id << endl;

                // auto *proxy = (struct pw_proxy *)pw_registry_bind(reg_data->reg, id, type, PW_VERSION_NODE, 0);
                // static const struct pw_node_events chromium_node_events = {.version = PW_VERSION_NODE_EVENTS,
                //                                                            .info =
                //                                                            proxy_info_event_add_virtual_streams,
                //                                                            .param = proxy_param_props_event};

                // uint32_t param_ids_sub[] = {SPA_PARAM_Props, SPA_PARAM_Format};
                // pw_node_subscribe_params((struct pw_node *)proxy, param_ids_sub,
                //                          sizeof(param_ids_sub) / sizeof(param_ids_sub[0]));

                // pw_proxy_add_object_listener(proxy, new spa_hook(), &chromium_node_events, data);
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