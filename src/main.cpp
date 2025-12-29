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

        ~virtual_node_data() {
            if (stream)
                pw_stream_destroy(stream);
            if (core)
                pw_core_disconnect(core);
            if (context)
                pw_context_destroy(context);
        }
    };

    struct stores_data {
        struct pw_node *vnode;
        struct pw_node *onode;
        const spa_pod *param_data;
        vector<spa_hook *> listeners; // to destroy
    };

    inline static vector<stores_data> stores = {};
    inline static unordered_map<uint32_t, virtual_node_data> onode_to_vnode = {};

    struct onode_info {
        pw_loop *loop;
        pw_registry *reg;
        uint32_t id;
        string app_process_binary;
        string media_class;
        string media_name;
        spa_audio_info_raw audio_info;
    };

    struct post_hook_args {
        onode_info *onode_info_ptr;
        uint32_t *vnode_id;
        stores_data *data;
    };

    struct process_event_data {
        onode_info onode_info_ptr;
        uint32_t vnode_id;
        spa_hook *listener;
        function<void(const post_hook_args &)> post_hook;
        post_hook_args args;
        bool info_flag;
        bool params_flag;
    };

    static void on_node_info_process(void *data, const struct pw_node_info *info) {
        auto *process_data = (struct process_event_data *)data;

        if (process_data->info_flag)
            return;

        const char *app_process_binary = spa_dict_lookup(info->props, PW_KEY_APP_PROCESS_BINARY);
        const char *media_class = spa_dict_lookup(info->props, PW_KEY_MEDIA_CLASS);
        const char *media_name = spa_dict_lookup(info->props, PW_KEY_MEDIA_NAME);

        process_data->onode_info_ptr.app_process_binary = app_process_binary ? string(app_process_binary) : "";
        process_data->onode_info_ptr.media_class = media_class ? string(media_class) : "";
        process_data->onode_info_ptr.media_name = media_name ? string(media_name) : "";

        process_data->info_flag = true;

        maybe_run_post_hook(process_data);
    }

    static void on_node_param_process(void *data, int seq, uint32_t id, uint32_t index, uint32_t next,
                                      const struct spa_pod *param) {
        auto *process_data = (struct process_event_data *)data;

        if (process_data->params_flag)
            return;

        if (id != SPA_PARAM_Format)
            return;

        auto *onode_info = &process_data->onode_info_ptr;
        spa_format_audio_raw_parse(param, &onode_info->audio_info);
        process_data->params_flag = true;

        maybe_run_post_hook(process_data);
    }

    static void maybe_run_post_hook(process_event_data *data) {
        if (!(data->info_flag && data->params_flag))
            return;

        spa_hook_remove(data->listener);
        delete data->listener;

        data->post_hook(data->args);

        delete data;
    }

    struct stream_state_changed_data {
        const uint32_t *onode_id;
        uint32_t *vnode_id;
        spa_hook *listener;
        struct pw_stream *stream;
        const post_hook_args *args;

        ~stream_state_changed_data() {
            if (listener) {
                spa_hook_remove(listener);
                delete listener;
            }
        }
    };

    static void on_stream_state_changed_process_vnode_id(void *data, enum pw_stream_state old,
                                                         enum pw_stream_state state, const char *error) {

        // process when its finished
        if (state != PW_STREAM_STATE_PAUSED)
            return;

        stream_state_changed_data *state_data = (stream_state_changed_data *)data;

        *state_data->vnode_id = pw_stream_get_node_id(state_data->stream);
        onode_to_vnode[*state_data->onode_id].id = *state_data->vnode_id;

        VirtualAppNodesManager::ph_populate_virtual_stores_data(*state_data->args->onode_info_ptr,
                                                                *state_data->args->vnode_id, *state_data->args->data);
        VirtualAppNodesManager::ph_create_sync_listeners(*state_data->args->data);

        delete state_data;
    }

    static void ph_create_virtual_node(const post_hook_args &args) {

        struct pw_properties *context_props =
            pw_properties_new(PW_KEY_APP_NAME, args.onode_info_ptr->app_process_binary.c_str(), nullptr);
        struct pw_context *virtual_context = pw_context_new(args.onode_info_ptr->loop, context_props, 0);
        struct pw_core *virtual_core = pw_context_connect(virtual_context, nullptr, 0);

        struct pw_properties *stream_props = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_APP_NAME, args.onode_info_ptr->app_process_binary.c_str(),
            PW_KEY_MEDIA_CLASS, args.onode_info_ptr->media_class.c_str(), PW_KEY_APP_ICON_NAME,
            args.onode_info_ptr->app_process_binary.c_str(), PW_KEY_APP_PROCESS_BINARY,
            args.onode_info_ptr->app_process_binary.c_str(), nullptr);
        struct pw_stream *virtual_stream =
            pw_stream_new(virtual_core, args.onode_info_ptr->media_name.c_str(), stream_props);

        uint8_t buffer[1024];
        struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        const struct spa_pod *params[1];
        params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &args.onode_info_ptr->audio_info);

        static const struct pw_stream_events stream_events = {
            .version = PW_VERSION_STREAM_EVENTS,
            .state_changed = on_stream_state_changed_process_vnode_id,
        };

        struct spa_hook *listener = new spa_hook();

        stream_state_changed_data *stream_data = new stream_state_changed_data();
        stream_data->vnode_id = args.vnode_id;
        stream_data->stream = virtual_stream;
        stream_data->listener = listener;
        stream_data->onode_id = &args.onode_info_ptr->id;
        stream_data->args = &args;

        pw_stream_add_listener(virtual_stream, listener, &stream_events, (void *)stream_data);

        pw_stream_connect(virtual_stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                          (enum pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), params, 1);

        onode_to_vnode[args.onode_info_ptr->id].context = virtual_context;
        onode_to_vnode[args.onode_info_ptr->id].core = virtual_core;
        onode_to_vnode[args.onode_info_ptr->id].stream = virtual_stream;
    }

    static void ph_populate_virtual_stores_data(const onode_info &onode_info, const uint32_t vnode_id,
                                                stores_data &data) {
        uint32_t onode_id = onode_info.id;
        struct pw_registry *vnode_reg = pw_core_get_registry(onode_to_vnode[onode_id].core, PW_VERSION_REGISTRY, 0);

        data.vnode =
            (struct pw_node *)pw_registry_bind(vnode_reg, vnode_id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0);

        cout << "vnode pointer: " << (void *)data.vnode << endl;
        cout << "vnode_id: " << vnode_id << endl;
    }

    static void on_vnode_param_props(void *data, int seq, uint32_t id, uint32_t index, uint32_t next,
                                     const struct spa_pod *param) {

        cout << "vnode params event" << endl;

        if (id != SPA_PARAM_Props)
            return;

        cout << "sync to onode" << endl;

        auto *stores_data = (struct stores_data *)data;
        stores_data->param_data = param;
        pw_node_set_param(stores_data->onode, SPA_PARAM_Props, 0, stores_data->param_data);
    }

    static void on_onode_param_props(void *data, int seq, uint32_t id, uint32_t index, uint32_t next,
                                     const struct spa_pod *param) {

        cout << "onode params event" << endl;

        if (id != SPA_PARAM_Props)
            return;

        cout << "sync to vnode" << endl;

        auto *stores_data = (struct stores_data *)data;
        pw_node_set_param(stores_data->vnode, SPA_PARAM_Props, 0, stores_data->param_data);
    }

    static void ph_create_sync_listeners(stores_data &data) {
        cout << "Creating sync listeners" << endl;
        cout << "vnode ptr: " << (void *)data.vnode << endl;
        cout << "onode ptr: " << (void *)data.onode << endl;

        struct spa_hook *vnode_listener = new spa_hook();
        struct spa_hook *onode_listener = new spa_hook();

        data.listeners.push_back(vnode_listener);
        data.listeners.push_back(onode_listener);

        uint32_t param_ids_sub[] = {SPA_PARAM_Props};

        pw_node_subscribe_params(data.vnode, param_ids_sub, sizeof(param_ids_sub) / sizeof(param_ids_sub[0]));
        pw_node_subscribe_params(data.onode, param_ids_sub, sizeof(param_ids_sub) / sizeof(param_ids_sub[0]));

        cout << "Subscribed to params" << endl;

        static const struct pw_node_events vnode_events = {
            .version = PW_VERSION_NODE_EVENTS,
            .param = on_vnode_param_props,
        };

        static const struct pw_node_events onode_events = {
            .version = PW_VERSION_NODE_EVENTS,
            .param = on_onode_param_props,
        };

        pw_proxy_add_object_listener((struct pw_proxy *)data.vnode, vnode_listener, &vnode_events, (void *)&data);
        pw_proxy_add_object_listener((struct pw_proxy *)data.onode, onode_listener, &onode_events, (void *)&data);

        cout << "Added listeners" << endl;

        pw_node_enum_params(data.vnode, 0, SPA_PARAM_Props, 0, UINT32_MAX, nullptr);

        cout << "Enumerated vnode params" << endl;
    }

  public:
    static void process_new_node(pw_registry *reg, pw_loop *loop, uint32_t id, const char *type) {

        stores_data *store = new stores_data(); // destroy later
        auto *node = (struct pw_proxy *)pw_registry_bind(reg, id, type, PW_VERSION_NODE, 0);
        store->onode = (struct pw_node *)node;

        process_event_data *data = new process_event_data(); // destroy later

        static const struct pw_node_events node_events = {
            .version = PW_VERSION_NODE_EVENTS,
            .info = on_node_info_process,
            .param = on_node_param_process,
        };

        struct spa_hook *listener = new spa_hook();

        data->listener = listener;
        data->info_flag = false;
        data->params_flag = false;
        data->onode_info_ptr = {
            .loop = loop,
            .reg = reg,
            .id = id,
            .app_process_binary = "",
            .media_class = "",
            .media_name = "",
            .audio_info = {},
        };
        data->vnode_id = 0;

        data->args.onode_info_ptr = &data->onode_info_ptr;
        data->args.data = store;
        data->args.vnode_id = &data->vnode_id;

        data->post_hook = [](const post_hook_args &args) { ph_create_virtual_node(args); };

        uint32_t param_ids_sub[] = {SPA_PARAM_Format};
        pw_node_subscribe_params((struct pw_node *)node, param_ids_sub,
                                 sizeof(param_ids_sub) / sizeof(param_ids_sub[0]));

        pw_proxy_add_object_listener(node, listener, &node_events, data);

        pw_node_enum_params((struct pw_node *)node, 0, SPA_PARAM_Format, 0, UINT32_MAX, nullptr);
    }

    static void on_global_remove(void *data, uint32_t id) {
        auto it = onode_to_vnode.find(id);

        if (it != onode_to_vnode.end())
            onode_to_vnode.erase(it);
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
        .global_remove = VirtualAppNodesManager::on_global_remove,
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