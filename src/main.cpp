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

TODO: destructors
*/
class VirtualAppNodesManager {
  private:
    struct virtual_node_data {
        uint32_t id;
        pw_context *context;
        pw_core *core;
        pw_stream *stream;
    };

    struct stores_data {
        struct pw_node *vnode;
        struct pw_node *onode;
        const spa_pod *param_data;
        vector<spa_hook *> listeners; // to destroy
    };

    inline static vector<stores_data> stores = {};

    struct node_info {
        pw_loop *loop;
        pw_registry *reg;
        string app_process_binary;
        string media_class;
        string media_name;
        spa_audio_info_raw audio_info;
        virtual_node_data vnode_data; // only populates once a virtual node is created using the node info
    };

    struct collector_data {
        node_info info;
        spa_hook *listener;
        queue<function<void(node_info)>> post_hook;
        bool info_flag;
        bool params_flag;
    };

    static void process_node_info(void *data, const struct pw_node_info *info) {
        auto *collector_data = (struct collector_data *)data;

        if (collector_data->info_flag)
            return;

        const char *app_process_binary = spa_dict_lookup(info->props, PW_KEY_APP_PROCESS_BINARY);
        const char *media_class = spa_dict_lookup(info->props, PW_KEY_MEDIA_CLASS);
        const char *media_name = spa_dict_lookup(info->props, PW_KEY_MEDIA_NAME);

        collector_data->info.app_process_binary = app_process_binary ? string(app_process_binary) : "";
        collector_data->info.media_class = media_class ? string(media_class) : "";
        collector_data->info.media_name = media_name ? string(media_name) : "";

        collector_data->info_flag = true;

        maybe_run_post_hook(collector_data);
    }

    static void process_node_param(void *data, int seq, uint32_t id, uint32_t index, uint32_t next,
                                   const struct spa_pod *param) {
        auto *collector_data = (struct collector_data *)data;

        if (collector_data->params_flag)
            return;

        if (id != SPA_PARAM_Format)
            return;

        auto *node_info = &collector_data->info;
        spa_format_audio_raw_parse(param, &node_info->audio_info);
        collector_data->params_flag = true;

        maybe_run_post_hook(collector_data);
    }

    static void maybe_run_post_hook(collector_data *data) {
        if (!(data->info_flag && data->params_flag))
            return;

        spa_hook_remove(data->listener);
        delete data->listener;

        while (!data->post_hook.empty()) {
            function<void(node_info)> func = data->post_hook.front();
            data->post_hook.pop();
            func(data->info);
        }

        delete data;
    }

    static void create_virtual_node(node_info &info) {

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

        virtual_node_data vnode = {
            .id = pw_stream_get_node_id(virtual_stream),
            .context = virtual_context,
            .core = virtual_core,
            .stream = virtual_stream,
        };
        info.vnode_data = vnode;
    }

    static void populate_virtual_stores_data(const node_info &info, stores_data &data) {
        data.vnode = (struct pw_node *)pw_registry_bind(info.reg, info.vnode_data.id, PW_TYPE_INTERFACE_Node,
                                                        PW_VERSION_NODE, 0);
    }

    static void on_vnode_param_props(void *data, int seq, uint32_t id, uint32_t index, uint32_t next,
                                     const struct spa_pod *param) {

        if (id != SPA_PARAM_Props)
            return;

        auto *stores_data = (struct stores_data *)data;
        stores_data->param_data = param;
        pw_node_set_param(stores_data->onode, SPA_PARAM_Props, 0, stores_data->param_data);
    }

    static void on_onode_param_props(void *data, int seq, uint32_t id, uint32_t index, uint32_t next,
                                     const struct spa_pod *param) {

        if (id != SPA_PARAM_Props)
            return;

        auto *stores_data = (struct stores_data *)data;
        pw_node_set_param(stores_data->vnode, SPA_PARAM_Props, 0, stores_data->param_data);
    }

    static void create_sync_listeners(stores_data &data) {
        struct spa_hook *vnode_listener = new spa_hook();
        struct spa_hook *onode_listener = new spa_hook();

        data.listeners.push_back(vnode_listener);
        data.listeners.push_back(onode_listener);

        uint32_t param_ids_sub[] = {SPA_PARAM_Props};

        pw_node_subscribe_params(data.vnode, param_ids_sub, sizeof(param_ids_sub) / sizeof(param_ids_sub[0]));
        pw_node_subscribe_params(data.onode, param_ids_sub, sizeof(param_ids_sub) / sizeof(param_ids_sub[0]));

        static const struct pw_node_events vnode_events = {
            .version = PW_VERSION_NODE_EVENTS,
            .param = VirtualAppNodesManager::on_vnode_param_props,
        };

        static const struct pw_node_events onode_events = {.version = PW_VERSION_NODE_EVENTS,
                                                           .param = VirtualAppNodesManager::on_onode_param_props};

        pw_proxy_add_object_listener((struct pw_proxy *)data.vnode, vnode_listener, &vnode_events, (void *)&data);
        pw_proxy_add_object_listener((struct pw_proxy *)data.onode, onode_listener, &onode_events, (void *)&data);
    }

  public:
    static void process_new_node(pw_registry *reg, pw_loop *loop, uint32_t id, const char *type) {

        stores_data *store = new stores_data(); // destroy later
        auto *node = (struct pw_proxy *)pw_registry_bind(reg, id, type, PW_VERSION_NODE, 0);
        store->onode = (struct pw_node *)node;

        collector_data *data = new collector_data(); // destroy later

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
            .reg = reg,
            .app_process_binary = "",
            .media_class = "",
            .media_name = "",
            .audio_info = {},
            .vnode_data = {},
        };
        data->post_hook = {};

        auto create_virtual_dev_func = [](VirtualAppNodesManager::node_info info) {
            VirtualAppNodesManager::create_virtual_node(info);
        };
        data->post_hook.push(create_virtual_dev_func);

        uint32_t param_ids_sub[] = {SPA_PARAM_Format};
        pw_node_subscribe_params((struct pw_node *)node, param_ids_sub,
                                 sizeof(param_ids_sub) / sizeof(param_ids_sub[0]));

        pw_proxy_add_object_listener(node, listener, &node_events, data);

        pw_node_enum_params((struct pw_node *)node, 0, SPA_PARAM_Format, 0, UINT32_MAX, nullptr);
    }
};

static void reg_event_find_chromium_nodes(void *data, uint32_t id, uint32_t permissions, const char *type,
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
        .global = reg_event_find_chromium_nodes,
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