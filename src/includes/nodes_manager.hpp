#include "pipewire/context.h"
#include "pipewire/core.h"
#include "pipewire/keys.h"
#include "pipewire/node.h"
#include "pipewire/properties.h"
#include "pipewire/proxy.h"
#include "pipewire/stream.h"
#include "spa/param/param.h"
#include "spa/pod/builder.h"
#include "spa/utils/dict.h"
#include "spa/utils/hook.h"
#include <any>
#include <cstdint>
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
TODO: move sync data to stores, actually store it there

TODO: destructors
*/

namespace {
class Stores {
  public:
    struct sync_params_data {
        struct pw_node *vnode;
        struct pw_registry *vnode_reg;
        struct pw_node *onode;
        bool ignore_next_onode_event;
        const spa_pod *param_data;
        vector<spa_hook *> listeners;

        sync_params_data() {
            this->param_data = nullptr;
            this->ignore_next_onode_event = false;
        }

        ~sync_params_data() {
            for (spa_hook *listener : this->listeners) {
                if (listener) {
                    spa_hook_remove(listener);
                    delete listener;
                    listener = nullptr;
                }
            }
            this->listeners.clear();

            if (this->vnode) {
                pw_proxy_destroy((pw_proxy *)this->vnode);
                this->vnode = nullptr;
            }

            if (this->onode) {
                pw_proxy_destroy((pw_proxy *)this->onode);
                this->onode = nullptr;
            }

            if (this->vnode_reg) {
                pw_proxy_destroy((pw_proxy *)this->vnode_reg);
                this->vnode_reg = nullptr;
            }
        }
    };

    struct onode_info {
        const uint32_t id;
        string app_process_binary;
        string media_class;
        string media_name;
        spa_audio_info_raw audio_info;

        onode_info(uint32_t id) : id(id) { this->audio_info = {}; }
    };

  private:
    struct virtual_node_data {
        uint32_t id;
        pw_context *context;
        pw_core *core;
        pw_stream *stream;

        virtual_node_data(uint32_t id, pw_context *context, pw_core *core, pw_stream *stream) {
            this->id = id;
            this->context = context;
            this->core = core;
            this->stream = stream;
        }

        ~virtual_node_data() {
            if (this->stream) {
                pw_stream_destroy(this->stream);
                this->stream = nullptr;
            }

            if (this->core) {
                pw_core_disconnect(this->core);
                this->core = nullptr;
            }

            if (this->context) {
                pw_context_destroy(this->context);
                this->context = nullptr;
            }
        }
    };

    inline static unordered_map<uint32_t, Stores::onode_info *> onode_infos = {};
    inline static unordered_map<uint32_t, Stores::virtual_node_data *> onode_to_vnode = {};
    inline static unordered_map<uint32_t, Stores::sync_params_data *> onode_to_sync_data = {};

    template <typename T> static void remove_onode_entry(uint32_t onode_id, unordered_map<uint32_t, T *> &map) {
        auto it = map.find(onode_id);

        if (it != map.end()) {
            delete map.at(it->first);
            it->second = nullptr;
            map.erase(it);
        }
    }

  public:
    static const virtual_node_data &get_vnode(uint32_t onode_id) { return *onode_to_vnode.at(onode_id); }

    static const void set_vnode(uint32_t onode_id, uint32_t vnode_id, pw_context *context, pw_core *core,
                                pw_stream *stream) {

        onode_to_vnode.emplace(onode_id, new virtual_node_data(vnode_id, context, core, stream));
    }

    static void remove_vnode_entry(uint32_t onode_id) {
        remove_onode_entry<virtual_node_data>(onode_id, onode_to_vnode);
    }

    static const onode_info &get_onode_info(uint32_t onode_id) { return *onode_infos.at(onode_id); }

    static onode_info &modify_onode_info_entry(uint32_t onode_id) {

        if (onode_infos.find(onode_id) == onode_infos.end()) {
            onode_infos[onode_id] = new onode_info(onode_id);
        }

        return *onode_infos[onode_id];
    }

    static void remove_onode_info_entry(uint32_t onode_id) { remove_onode_entry<onode_info>(onode_id, onode_infos); }

    static sync_params_data &modify_sync_data_entry(uint32_t onode_id) {
        if (onode_to_sync_data.find(onode_id) == onode_to_sync_data.end()) {
            onode_to_sync_data[onode_id] = new sync_params_data();
        }

        return *onode_to_sync_data[onode_id];
    }

    static void remove_sync_data_entry(uint32_t onode_id) {
        remove_onode_entry<sync_params_data>(onode_id, onode_to_sync_data);
    }

    static void cleanup() {
        for (const auto &[key, value] : onode_infos)
            remove_onode_entry(key, onode_infos);

        for (const auto &[key, value] : onode_to_vnode)
            remove_onode_entry(key, onode_to_vnode);

        for (const auto &[key, value] : onode_to_sync_data)
            remove_onode_entry(key, onode_to_sync_data);
    }
};

class ArgStructs {
  public:
    /**
    topology: virtual_node_args > state_change_args > state_change_callback_args + state_change_hook_args
    */
    struct virtual_node_args {

        // structs
        struct state_change_args {

            // structs
            struct state_change_hook_args {

                // properties
                const uint32_t &onode_id;
                const uint32_t &vnode_id;

                state_change_hook_args(const uint32_t &onode_id, const uint32_t &vnode_id)
                    : onode_id(onode_id), vnode_id(vnode_id) {}
            };

            /**
            temporary data to set vnode's data upon stream state paused (when virtual stream finishes setting up
            async) self destructed by on_stream_state_changed_process_vnode_id()
            */
            struct state_change_callback_args {
                // properties
                uint32_t &vnode_id;
                struct pw_context &context;
                struct pw_core &core;
                struct pw_stream &stream;
                bool stream_processed_flag;

                const uint32_t &onode_id;
                spa_hook *self_listener;

                state_change_callback_args(const uint32_t &onode_id, uint32_t &vnode_id, spa_hook *listener,
                                           pw_context &vcontext, pw_core &vcore, pw_stream &vstream)
                    : vnode_id(vnode_id), context(vcontext), core(vcore), stream(vstream), onode_id(onode_id) {
                    this->stream_processed_flag = false;
                    this->self_listener = listener;
                }

                ~state_change_callback_args() {
                    if (this->self_listener) {
                        spa_hook_remove(this->self_listener);
                        delete this->self_listener;
                        this->self_listener = nullptr;
                    }
                }
            };

            // properties
            state_change_callback_args *callback_args;
            state_change_hook_args *hook_args;

            state_change_args(state_change_hook_args *hook_args) {
                this->hook_args = hook_args;
                this->callback_args = nullptr;
            }

            ~state_change_args() {
                if (this->hook_args) {
                    delete this->hook_args;
                    this->hook_args = nullptr;
                }

                if (this->callback_args) {
                    delete this->callback_args;
                    this->callback_args = nullptr;
                }
            }
        };

        // properties
        struct pw_loop &loop;
        const Stores::onode_info &onode;
        uint32_t vnode_id;
        void (*state_change_callback)(void *, enum pw_stream_state, enum pw_stream_state, const char *);
        state_change_args *state_change_args;

        virtual_node_args(struct pw_loop &loop, const Stores::onode_info &onode,
                          void (*state_change_callback)(void *, enum pw_stream_state, enum pw_stream_state,
                                                        const char *))
            : loop(loop), onode(onode), state_change_callback(state_change_callback) {
            this->vnode_id = 0;

            this->state_change_args =
                new struct state_change_args(new state_change_args::state_change_hook_args(onode.id, this->vnode_id));
        }

        ~virtual_node_args() {
            if (this->state_change_args) {
                delete this->state_change_args;
                this->state_change_args = nullptr;
            }
        }
    };

    struct process_onode_info_args {
        Stores::onode_info &onode;
        bool info_flag;
        bool params_flag;

        spa_hook *self_listener;

        process_onode_info_args(Stores::onode_info &onode_info, spa_hook *listener) : onode(onode_info) {
            this->self_listener = listener;
            this->info_flag = false;
            this->params_flag = false;
        }

        ~process_onode_info_args() {
            if (this->self_listener) {
                spa_hook_remove(this->self_listener);
                delete this->self_listener;
                this->self_listener = nullptr;
            }
        }
    };
};

class EventListeners {
  public:
    // for syncing params between vnode and onode

    static void on_node_info_process_onode_info(void *data, const struct pw_node_info *info) {
        auto *process_data = (ArgStructs::process_onode_info_args *)data;

        const char *app_process_binary = spa_dict_lookup(info->props, PW_KEY_APP_PROCESS_BINARY);
        const char *media_class = spa_dict_lookup(info->props, PW_KEY_MEDIA_CLASS);
        const char *media_name = spa_dict_lookup(info->props, PW_KEY_MEDIA_NAME);

        process_data->onode.app_process_binary = app_process_binary ? string(app_process_binary) : "";
        process_data->onode.media_class = media_class ? string(media_class) : "";
        process_data->onode.media_name = media_name ? string(media_name) : "";

        process_data->info_flag = true;
    }

    static void on_node_param_process_onode_info(void *data, int seq, uint32_t id, uint32_t index, uint32_t next,
                                                 const struct spa_pod *param) {
        auto *process_data = (ArgStructs::process_onode_info_args *)data;

        if (id != SPA_PARAM_Format)
            return;

        auto *onode_info = &process_data->onode;
        spa_format_audio_raw_parse(param, &onode_info->audio_info);
        process_data->params_flag = true;
    }

    static void on_stream_state_changed_process_vnode_id(void *data, enum pw_stream_state old,
                                                         enum pw_stream_state state, const char *error) {

        // process when its finished
        if (state != PW_STREAM_STATE_PAUSED)
            return;

        ArgStructs::virtual_node_args::state_change_args::state_change_callback_args *state_data =
            (ArgStructs::virtual_node_args::state_change_args::state_change_callback_args *)data;

        if (state_data->stream_processed_flag)
            return;

        state_data->vnode_id = pw_stream_get_node_id(&state_data->stream);

        Stores::set_vnode(state_data->onode_id, state_data->vnode_id, &state_data->context, &state_data->core,
                          &state_data->stream);

        state_data->stream_processed_flag = true;

        spa_hook_remove(state_data->self_listener);
        delete state_data->self_listener;
        state_data->self_listener = nullptr;
    }

    static void on_vnode_param_props(void *data, uint32_t id, const struct spa_pod *param) {

        if (id != SPA_PARAM_Props)
            return;

        auto *sync_data = (struct Stores::sync_params_data *)data;

        sync_data->param_data = param;
        sync_data->ignore_next_onode_event = true;
        pw_node_set_param(sync_data->onode, SPA_PARAM_Props, 0, sync_data->param_data);
    }

    static void on_onode_param_props(void *data, int seq, uint32_t id, uint32_t index, uint32_t next,
                                     const struct spa_pod *param) {

        if (id != SPA_PARAM_Props)
            return;

        auto *sync_data = (Stores::sync_params_data *)data;

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
    static void create_virtual_node(struct ArgStructs::virtual_node_args &args) {

        struct pw_properties *context_props =
            pw_properties_new(PW_KEY_APP_NAME, args.onode.app_process_binary.c_str(), nullptr);
        struct pw_context *virtual_context = pw_context_new(&args.loop, context_props, 0);
        struct pw_core *virtual_core = pw_context_connect(virtual_context, nullptr, 0);

        struct pw_properties *stream_props = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_APP_NAME, args.onode.app_process_binary.c_str(), PW_KEY_MEDIA_CLASS,
            args.onode.media_class.c_str(), PW_KEY_APP_ICON_NAME, args.onode.app_process_binary.c_str(),
            PW_KEY_APP_PROCESS_BINARY, args.onode.app_process_binary.c_str(), nullptr);
        struct pw_stream *virtual_stream = pw_stream_new(virtual_core, args.onode.media_name.c_str(), stream_props);

        uint8_t buffer[1024];
        struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        const struct spa_pod *params[1];
        params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &args.onode.audio_info);

        static const struct pw_stream_events stream_events = {
            .version = PW_VERSION_STREAM_EVENTS,
            .state_changed = args.state_change_callback,
        };

        args.state_change_args->callback_args =
            new ArgStructs::virtual_node_args::state_change_args::state_change_callback_args(
                args.onode.id, args.vnode_id, new spa_hook(), *virtual_context, *virtual_core, *virtual_stream);

        pw_stream_add_listener(virtual_stream, args.state_change_args->callback_args->self_listener, &stream_events,
                               (void *)&args);

        pw_stream_connect(virtual_stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                          (enum pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), params, 1);
    }

    static void
    post_virtual_stream_process(const ArgStructs::virtual_node_args::state_change_args::state_change_hook_args &args) {

        Stores::sync_params_data &data_sync = Stores::modify_sync_data_entry(args.onode_id);

        data_sync.vnode_reg = pw_core_get_registry(Stores::get_vnode(args.onode_id).core, PW_VERSION_REGISTRY, 0);

        data_sync.vnode = (struct pw_node *)pw_registry_bind(data_sync.vnode_reg, args.vnode_id, PW_TYPE_INTERFACE_Node,
                                                             PW_VERSION_NODE, 0);

        struct spa_hook *vnode_listener = new spa_hook();
        struct spa_hook *onode_listener = new spa_hook();

        data_sync.listeners.push_back(vnode_listener);
        data_sync.listeners.push_back(onode_listener);

        uint32_t param_ids_sub[] = {SPA_PARAM_Props};

        pw_node_subscribe_params(Stores::modify_sync_data_entry(args.onode_id).vnode, param_ids_sub,
                                 sizeof(param_ids_sub) / sizeof(param_ids_sub[0]));
        pw_node_subscribe_params(Stores::modify_sync_data_entry(args.onode_id).onode, param_ids_sub,
                                 sizeof(param_ids_sub) / sizeof(param_ids_sub[0]));

        struct pw_stream *vstream = Stores::get_vnode(args.onode_id).stream;

        static const struct pw_stream_events vnode_events = {
            .version = PW_VERSION_STREAM_EVENTS,
            .param_changed = EventListeners::on_vnode_param_props,
        };

        static const struct pw_node_events onode_events = {
            .version = PW_VERSION_NODE_EVENTS,
            .param = EventListeners::on_onode_param_props,
        };

        // pw_proxy_add_object_listener((struct pw_proxy *)data.vnode, vnode_listener, &vnode_events, (void *)&data);
        pw_stream_add_listener(vstream, vnode_listener, &vnode_events,
                               (void *)&Stores::modify_sync_data_entry(args.onode_id));
        pw_proxy_add_object_listener((struct pw_proxy *)Stores::modify_sync_data_entry(args.onode_id).onode,
                                     onode_listener, &onode_events,
                                     (void *)&Stores::modify_sync_data_entry(args.onode_id));

        pw_node_enum_params(Stores::modify_sync_data_entry(args.onode_id).vnode, 0, SPA_PARAM_Props, 0, UINT32_MAX,
                            nullptr);
        pw_stream_update_params(vstream, nullptr, 0);
    }
};

} // namespace

class NodesManager {

  private:
    struct process_and_vnode_args_data {
        ArgStructs::process_onode_info_args *process_args;
        ArgStructs::virtual_node_args *vnode_args;

        ~process_and_vnode_args_data() {
            if (this->process_args) {
                delete this->process_args;
                this->process_args = nullptr;
            }

            if (this->vnode_args) {
                delete this->vnode_args;
                this->vnode_args = nullptr;
            }
        }
    };

    static void maybe_run_post_process(process_and_vnode_args_data *args) {

        if (!(args->process_args->info_flag && args->process_args->params_flag))
            return;

        spa_hook_remove(args->process_args->self_listener);
        delete args->process_args->self_listener;
        args->process_args->self_listener = nullptr;

        ArgStructs::virtual_node_args *vnode_args = args->vnode_args;
        args->vnode_args = nullptr;

        StaticPostHooks::create_virtual_node(*vnode_args);

        delete args;
        args = nullptr;
    }

    static void on_node_info_process_hook(void *data, const struct pw_node_info *info) {
        process_and_vnode_args_data *args = (process_and_vnode_args_data *)data;

        if (args->process_args->info_flag)
            return;

        EventListeners::on_node_info_process_onode_info((void *)args->process_args, info);
        NodesManager::maybe_run_post_process(args);
    }

    static void on_node_param_process_hook(void *data, int seq, uint32_t id, uint32_t index, uint32_t next,
                                           const struct spa_pod *param) {

        process_and_vnode_args_data *args = (process_and_vnode_args_data *)data;

        if (args->process_args->params_flag)
            return;

        EventListeners::on_node_param_process_onode_info((void *)args->process_args, seq, id, index, next, param);
        NodesManager::maybe_run_post_process(args);
    }

    static void on_state_change_callback_hook(void *data, enum pw_stream_state old, enum pw_stream_state state,
                                              const char *error) {
        struct ArgStructs::virtual_node_args *args = (struct ArgStructs::virtual_node_args *)data;

        EventListeners::on_stream_state_changed_process_vnode_id((void *)args->state_change_args->callback_args, old,
                                                                 state, error);

        if (!args->state_change_args->callback_args->stream_processed_flag)
            return;

        StaticPostHooks::post_virtual_stream_process(*args->state_change_args->hook_args);

        delete args;
        args = nullptr;
    }

  public:
    static void process_new_node(pw_registry *reg, pw_loop *loop, uint32_t id, const char *type) {

        Stores::modify_sync_data_entry(id).onode =
            (struct pw_node *)pw_registry_bind(reg, id, type, PW_VERSION_NODE, 0);

        static const struct pw_node_events node_events = {
            .version = PW_VERSION_NODE_EVENTS,
            .info = NodesManager::on_node_info_process_hook,
            .param = NodesManager::on_node_param_process_hook,
        };

        ArgStructs::process_onode_info_args *process_args = new ArgStructs::process_onode_info_args(
            Stores::modify_onode_info_entry(id), new spa_hook()); // destroy later FIXME:

        ArgStructs::virtual_node_args *vnode_args = new ArgStructs::virtual_node_args(
            *loop, Stores::modify_onode_info_entry(id), NodesManager::on_state_change_callback_hook); // destroy FIXME:

        uint32_t param_ids_sub[] = {SPA_PARAM_Format};
        pw_node_subscribe_params(Stores::modify_sync_data_entry(id).onode, param_ids_sub,
                                 sizeof(param_ids_sub) / sizeof(param_ids_sub[0]));

        process_and_vnode_args_data *args = new process_and_vnode_args_data();
        args->vnode_args = vnode_args;
        args->process_args = process_args;
        pw_proxy_add_object_listener((struct pw_proxy *)Stores::modify_sync_data_entry(id).onode,
                                     process_args->self_listener, &node_events, args);

        pw_node_enum_params(Stores::modify_sync_data_entry(id).onode, 0, SPA_PARAM_Format, 0, UINT32_MAX, nullptr);
    }

    static void on_global_remove(void *data, uint32_t id) {
        Stores::remove_vnode_entry(id);
        Stores::remove_sync_data_entry(id);
        Stores::remove_onode_info_entry(id);
    }

    static void cleanup() { Stores::cleanup(); }
};
