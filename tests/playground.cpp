#include "pipewire/client.h"
#include "pipewire/context.h"
#include "pipewire/core.h"
#include "pipewire/main-loop.h"
#include "pipewire/node.h"
#include "pipewire/pipewire.h"
#include "pipewire/proxy.h"
#include <iostream>
#include <map>
#include <ostream>

using std::cout;
using std::endl;
using std::string;

struct node_data {
    struct pw_proxy *proxy;
    struct spa_hook listener;
};

std::map<uint32_t, node_data> nodes;

void raiseError(bool condition, string message, int status = 1) {
    if (condition) {
        cout << "Error: " << message << endl;
        exit(status);
    }
}

struct roundtrip_data {
    int pending;
    struct pw_main_loop *loop;
};

static void on_core_done(void *data, uint32_t id, int seq) {
    auto *d = (struct roundtrip_data *)data;

    if (id == PW_ID_CORE && seq == d->pending) {
        cout << "done seq: " << seq << endl;
        // pw_main_loop_quit(d->loop);
    }
}

static void roundtrip(struct pw_core *core, struct pw_main_loop *loop) {
    static const struct pw_core_events core_events = {
        .version = PW_VERSION_CORE_EVENTS,
        .done = on_core_done,
    };

    struct roundtrip_data d = {.pending = pw_core_sync(core, PW_ID_CORE, 0), .loop = loop};
    struct spa_hook core_listener;
    int err;

    pw_core_add_listener(core, &core_listener, &core_events, &d);

    if ((err = pw_main_loop_run(loop)) < 0)
        printf("main_loop_run error:%d!\n", err);

    spa_hook_remove(&core_listener);
}

static void node_info(void *data, const struct pw_client_info *info) {
    if (info->props) {
        const struct spa_dict_item *item;
        spa_dict_for_each(item, info->props) { printf("%s = %s\n", item->key, item->value); }
    }
}

static const struct pw_node_events node_events = {
    PW_VERSION_NODE_EVENTS,
    .info = node_info,
};

// Replace registry_event_global with:
static void registry_event_global(void *data, uint32_t id, uint32_t permissions, const char *type, uint32_t version,
                                  const struct spa_dict *props) {

    if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
        struct pw_registry *reg = (struct pw_registry *)data;
        struct pw_proxy *proxy = (struct pw_proxy *)pw_registry_bind(reg, id, type, PW_VERSION_NODE, 0);
        struct spa_hook *listener = new spa_hook();
        spa_zero(*listener);
        pw_proxy_add_object_listener(proxy, listener, &node_events, nullptr);
    }
}

int main() {
    struct pw_main_loop *loop;
    struct pw_context *context;
    struct pw_core *core;
    struct pw_registry *registry;
    struct spa_hook registry_listener;

    // registry_events objects connected to the handler registry_event_global
    static const struct pw_registry_events registry_events = {
        .version = PW_VERSION_REGISTRY_EVENTS,
        .global = registry_event_global,
    };

    pw_init(nullptr, nullptr);

    // getting context to connect to pipewire daemon
    loop = pw_main_loop_new(nullptr);
    context = pw_context_new(pw_main_loop_get_loop(loop), nullptr, 0);

    // connect context to daemon
    core = pw_context_connect(context, nullptr, 0);
    raiseError(core == nullptr, "failed to connect to pipewire daemon");

    // setup for listener
    registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
    spa_zero(registry_listener);

    // listener with required setup and events handler
    pw_registry_add_listener(registry, &registry_listener, &registry_events, registry);

    roundtrip(core, loop);

    pw_proxy_destroy((struct pw_proxy *)registry);
    pw_core_disconnect(core);
    pw_context_destroy(context);
    pw_main_loop_destroy(loop);

    return 0;
}

/*
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
    */