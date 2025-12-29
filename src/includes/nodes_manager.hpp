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
#include <any>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <ostream>
#include <queue>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/compare.h>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>
using std::any;
using std::any_cast;
using std::cout;
using std::endl;
using std::function;
using std::make_tuple;
using std::queue;
using std::string;
using std::tuple;
using std::unordered_map;
using std::vector;

/*
TODO:
- maintain chromium binary -> node id information
- ensure all chormium binary has a virtual device, if not, create
- ensure all virtual device has a chorium binary, if not, delete

actually, just make a virtual device in sync with one chromium device

TODO: destructors
*/

namespace {
struct onode_info {
    pw_loop *loop;
    pw_registry *reg;
    uint32_t id;
    string app_process_binary;
    string media_class;
    string media_name;
    spa_audio_info_raw audio_info;
};

struct process_event_data {
    onode_info onode_info_ptr;
    uint32_t vnode_id;
    spa_hook *listener;
    bool info_flag;
    bool params_flag;
};

class StoreStates {
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

    inline static unordered_map<uint32_t, StoreStates::virtual_node_data> onode_to_vnode = {};

  public:
    static const virtual_node_data &get_vnode(uint32_t onode_id) { return onode_to_vnode[onode_id]; }

    static const void set_vnode(uint32_t onode_id, uint32_t vnode_id, pw_context *context, pw_core *core,
                                pw_stream *stream) {

        onode_to_vnode[onode_id].id = vnode_id;
        onode_to_vnode[onode_id].context = context;
        onode_to_vnode[onode_id].core = core;
        onode_to_vnode[onode_id].stream = stream;
    }

    static void remove_onode_entry(uint32_t onode_id) {
        auto it = onode_to_vnode.find(onode_id);

        if (it != onode_to_vnode.end())
            onode_to_vnode.erase(it);
    }
};

class EventListeners {
  public:
    // for syncing params between vnode and onode
    struct sync_params_data {
        struct pw_node *vnode;
        struct pw_node *onode;
        bool ignore_next_onode_event;
        const spa_pod *param_data;
        vector<spa_hook *> listeners; // to destroy
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
    }

    // temporary data to set vnode's data upon stream state paused (when virtual stream finishes setting up async)
    // self destructed by on_stream_state_changed_process_vnode_id()
    struct stream_state_changed_data {
        const uint32_t *onode_id;
        uint32_t *vnode_id;
        spa_hook *listener;
        bool stream_processed_flag;
        struct pw_context *context;
        struct pw_core *core;
        struct pw_stream *stream;

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
        StoreStates::set_vnode(*state_data->onode_id, *state_data->vnode_id, state_data->context, state_data->core,
                               state_data->stream);

        state_data->stream_processed_flag = true;

        delete state_data;
    }

    static void on_vnode_param_props(void *data, uint32_t id, const struct spa_pod *param) {

        cout << "vnode params event" << endl;

        if (id != SPA_PARAM_Props)
            return;

        cout << "sync to onode" << endl;

        auto *sync_data = (struct EventListeners::sync_params_data *)data;

        sync_data->param_data = param;
        sync_data->ignore_next_onode_event = true;
        pw_node_set_param(sync_data->onode, SPA_PARAM_Props, 0, sync_data->param_data);
    }

    static void on_onode_param_props(void *data, int seq, uint32_t id, uint32_t index, uint32_t next,
                                     const struct spa_pod *param) {

        cout << "onode params event" << endl;

        if (id != SPA_PARAM_Props)
            return;

        cout << "sync to vnode" << endl;

        auto *sync_data = (EventListeners::sync_params_data *)data;

        if (sync_data->ignore_next_onode_event) {
            sync_data->ignore_next_onode_event = false;
            return;
        }

        sync_data->ignore_next_onode_event = true;
        pw_node_set_param(sync_data->onode, SPA_PARAM_Props, 0, sync_data->param_data);
    }
};

// dependent on EventListeners (run after events)
class StaticPostHooks {
  public:
    struct virtual_node_args {
        struct state_change_hook_args {
            const onode_info *onode_info_ptr;
            const uint32_t *vnode_id;
            EventListeners::sync_params_data *sync_data;
        };

        struct state_change_args {
            EventListeners::stream_state_changed_data *callback_args;
            state_change_hook_args *hook_args;
        };

        const onode_info *onode_info_ptr;
        uint32_t *vnode_id;
        void (*state_change_callback)(void *, enum pw_stream_state, enum pw_stream_state, const char *);
        state_change_args *state_change_args;
    };

    static void create_virtual_node(const struct virtual_node_args &args) {

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
            .state_changed = args.state_change_callback,
        };

        struct spa_hook *listener = new spa_hook();

        EventListeners::stream_state_changed_data *stream_data =
            new EventListeners::stream_state_changed_data(); // destory FIXME:
        stream_data->vnode_id = args.vnode_id;
        stream_data->stream = virtual_stream;
        stream_data->listener = listener;
        stream_data->onode_id = &args.onode_info_ptr->id;
        stream_data->context = virtual_context;
        stream_data->core = virtual_core;
        stream_data->stream_processed_flag = false;

        args.state_change_args->callback_args = stream_data;

        pw_stream_add_listener(virtual_stream, listener, &stream_events, (void *)args.state_change_args);

        pw_stream_connect(virtual_stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                          (enum pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), params, 1);
    }

    static void post_virtual_stream_process(const virtual_node_args::state_change_hook_args &args) {
        const uint32_t onode_id = args.onode_info_ptr->id;
        struct pw_registry *vnode_reg =
            pw_core_get_registry(StoreStates::get_vnode(onode_id).core, PW_VERSION_REGISTRY, 0);

        args.sync_data->vnode =
            (struct pw_node *)pw_registry_bind(vnode_reg, *args.vnode_id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0);

        cout << "vnode pointer: " << (void *)args.sync_data->vnode << endl;
        cout << "vnode_id: " << *args.vnode_id << endl;

        cout << "Creating sync listeners" << endl;
        cout << "vnode ptr: " << (void *)args.sync_data->vnode << endl;
        cout << "onode ptr: " << (void *)args.sync_data->onode << endl;

        struct spa_hook *vnode_listener = new spa_hook();
        struct spa_hook *onode_listener = new spa_hook();

        args.sync_data->listeners.push_back(vnode_listener);
        args.sync_data->listeners.push_back(onode_listener);

        uint32_t param_ids_sub[] = {SPA_PARAM_Props};

        pw_node_subscribe_params(args.sync_data->vnode, param_ids_sub,
                                 sizeof(param_ids_sub) / sizeof(param_ids_sub[0]));
        pw_node_subscribe_params(args.sync_data->onode, param_ids_sub,
                                 sizeof(param_ids_sub) / sizeof(param_ids_sub[0]));

        cout << "Subscribed to params" << endl;

        struct pw_stream *vstream = StoreStates::get_vnode(args.onode_info_ptr->id).stream; // FIXME:

        // cout << "vstream ptr: " << (void *)vstream
        //      << " onode id: " << pw_proxy_get_id((struct pw_proxy *)args.sync_data->onode) << endl;

        static const struct pw_stream_events vnode_events = {
            .version = PW_VERSION_STREAM_EVENTS,
            .param_changed = EventListeners::on_vnode_param_props,
        };

        static const struct pw_node_events onode_events = {
            .version = PW_VERSION_NODE_EVENTS,
            .param = EventListeners::on_onode_param_props,
        };

        // pw_proxy_add_object_listener((struct pw_proxy *)data.vnode, vnode_listener, &vnode_events, (void *)&data);
        pw_stream_add_listener(vstream, vnode_listener, &vnode_events, (void *)args.sync_data);
        pw_proxy_add_object_listener((struct pw_proxy *)args.sync_data->onode, onode_listener, &onode_events,
                                     (void *)args.sync_data);

        cout << "Added listeners" << endl;

        pw_node_enum_params(args.sync_data->vnode, 0, SPA_PARAM_Props, 0, UINT32_MAX, nullptr);
        pw_stream_update_params(vstream, nullptr, 0);

        cout << "Enumerated vnode params" << endl;
    }
};

// class EventQueue {

//   public:
//     function<void(any)> func;
//     function<void(void *)> post_hook;
//     void *post_hook_args;

//     EventQueue(function<void(any)> func, function<void(void *)> post_hook, void *post_hook_args) {
//         this->func = func;
//         this->post_hook = post_hook;
//         this->post_hook_args = post_hook_args;
//     }

//     template <typename... Args> static function<void(any)> make_unpacker(void (*f)(Args...)) {
//         return [f](any args) { std::apply(f, any_cast<tuple<Args...>>(args)); };
//     }

//     /**
//     @return void function with args:
//     - void *data,
//     - int seq,
//     - uint32_t id,
//     - uint32_t index,
//     - uint32_t next,
//     - const struct spa_pod *param
//     */
//     function<void(void *, int, uint32_t, uint32_t, uint32_t, const struct spa_pod *)> node_params() {
//         auto compiled = [this](void *data, int seq, uint32_t id, uint32_t index, uint32_t next,
//                                const struct spa_pod *param) {
//             auto args = make_tuple(data, seq, id, index, next, param);
//             this->func(args);
//             post_hook(this->post_hook_args);
//         };

//         return compiled;
//     }

//     /**
//     @return void function with args:
//     - void *data
//     - const struct pw_node_info *info
//     */
//     function<void(void *, const struct pw_node_info *)> node_info() {
//         auto compiled = [this](void *data, const struct pw_node_info *info) {
//             auto args = make_tuple(data, info);
//             this->func(args);
//             post_hook(this->post_hook_args);
//         };

//         return compiled;
//     }

//     template <EventQueue *Instance> static void node_info_static(void *data, const struct pw_node_info *info) {
//         auto args = make_tuple(data, info);
//         Instance->func(args);
//         Instance->post_hook(Instance->post_hook_args);
//     }

//     /**
//     @return void function with args:
//     - void *data
//     - enum pw_stream_state old
//     - enum pw_stream_state state
//     - const char *error
//     */
//     function<void(void *, enum pw_stream_state, enum pw_stream_state, const char *)> stream_state_changed() {
//         auto compiled = [this](void *data, enum pw_stream_state old, enum pw_stream_state state, const char *error) {
//             auto args = make_tuple(data, old, state, error);
//             this->func(args);
//             post_hook(this->post_hook_args);
//         };

//         return compiled;
//     }
// };

} // namespace

class NodesManager {

  private:
    struct process_and_ph_args_data {
        process_event_data *process_data;
        StaticPostHooks::virtual_node_args *ph_args_data;
    };

    static void maybe_run_post_process(process_and_ph_args_data *args) {

        if (!(args->process_data->info_flag && args->process_data->params_flag))
            return;

        spa_hook_remove(args->process_data->listener);
        delete args->process_data->listener;

        StaticPostHooks::create_virtual_node(*args->ph_args_data);

        delete args->process_data;
    }

    static void on_node_info_process_hook(void *data, const struct pw_node_info *info) {
        process_and_ph_args_data *args = (process_and_ph_args_data *)data;

        EventListeners::on_node_info_process((void *)args->process_data, info);
        NodesManager::maybe_run_post_process(args);
    }

    static void on_node_param_process_hook(void *data, int seq, uint32_t id, uint32_t index, uint32_t next,
                                           const struct spa_pod *param) {

        process_and_ph_args_data *args = (process_and_ph_args_data *)data;

        EventListeners::on_node_param_process((void *)args->process_data, seq, id, index, next, param);
        NodesManager::maybe_run_post_process(args);
    }

    static void on_state_change_callback(void *data, enum pw_stream_state old, enum pw_stream_state state,
                                         const char *error) {
        struct StaticPostHooks::virtual_node_args::state_change_args *args =
            (struct StaticPostHooks::virtual_node_args::state_change_args *)data;

        EventListeners::on_stream_state_changed_process_vnode_id((void *)args->callback_args, old, state, error);

        if (!args->callback_args->stream_processed_flag)
            return;

        StaticPostHooks::post_virtual_stream_process(*args->hook_args);
    }

  public:
    static void process_new_node(pw_registry *reg, pw_loop *loop, uint32_t id, const char *type) {

        EventListeners::sync_params_data *sync_data = new EventListeners::sync_params_data(); // destroy later
        auto *node = (struct pw_proxy *)pw_registry_bind(reg, id, type, PW_VERSION_NODE, 0);
        sync_data->onode = (struct pw_node *)node;

        static const struct pw_node_events node_events = {
            .version = PW_VERSION_NODE_EVENTS,
            .info = NodesManager::on_node_info_process_hook,
            .param = NodesManager::on_node_param_process_hook,
        };

        struct spa_hook *listener = new spa_hook();

        process_event_data *data = new process_event_data(); // destroy later FIXME:

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

        StaticPostHooks::virtual_node_args *ph_args_data = new StaticPostHooks::virtual_node_args(); // destroy FIXME:

        ph_args_data->onode_info_ptr = &data->onode_info_ptr;
        ph_args_data->vnode_id = &data->vnode_id;
        ph_args_data->state_change_callback = NodesManager::on_state_change_callback;

        ph_args_data->state_change_args =
            new struct StaticPostHooks::virtual_node_args::state_change_args(); // destory FIXME:
        ph_args_data->state_change_args->hook_args = new StaticPostHooks::virtual_node_args::state_change_hook_args();

        ph_args_data->state_change_args->callback_args = nullptr; // created through the callbacks

        ph_args_data->state_change_args->hook_args->onode_info_ptr = ph_args_data->onode_info_ptr;
        ph_args_data->state_change_args->hook_args->vnode_id = &data->vnode_id;
        ph_args_data->state_change_args->hook_args->sync_data = sync_data;

        uint32_t param_ids_sub[] = {SPA_PARAM_Format};
        pw_node_subscribe_params((struct pw_node *)node, param_ids_sub,
                                 sizeof(param_ids_sub) / sizeof(param_ids_sub[0]));

        process_and_ph_args_data *args = new process_and_ph_args_data();
        args->ph_args_data = ph_args_data;
        args->process_data = data;
        pw_proxy_add_object_listener(node, listener, &node_events, args);

        pw_node_enum_params((struct pw_node *)node, 0, SPA_PARAM_Format, 0, UINT32_MAX, nullptr);
    }

    static void on_global_remove(void *data, uint32_t id) { StoreStates::remove_onode_entry(id); }
};