#pragma once

#include "nodes_manager.hpp"
#include "pipewire/stream.h"
#include <spa/param/audio/format-utils.h>
#include <string>
#include <unordered_map>
#include <vector>

using std::string;
using std::unordered_map;
using std::vector;

class VolumeManagerArgs {
  private:
    friend class VolumeManager;
    struct post_state_process_hook_args {
        const uint32_t onode_id;

        post_state_process_hook_args(const uint32_t onode_id) : onode_id(onode_id) {
        }
    };

    struct state_change_callback_args {
        const uint32_t onode_id;
        spa_hook *self_listener;

        bool stream_processed_flag;

        void *(*state_processed_hook)(void *args);
        void *state_processed_hook_args;

        state_change_callback_args(const uint32_t onode_id, void *(*state_processed_hook)(void *args),
                                   void *state_processed_hook_args)
            : onode_id(onode_id) {
            this->stream_processed_flag = false;
            this->self_listener = new spa_hook();
            this->state_processed_hook = state_processed_hook;
            this->state_processed_hook_args = state_processed_hook_args;
        }

        ~state_change_callback_args() {
            if (this->self_listener) {
                spa_hook_remove(this->self_listener);
                delete this->self_listener;
                this->self_listener = nullptr;
            }
        }
    };
};

class VolumeStores {
  public:
    struct sync_params_data {
        pw_node *vnode;
        pw_registry *vnode_reg;
        pw_node *onode;
        bool ignore_next_onode_event;
        spa_pod *param_data;
        vector<spa_hook *> listeners;

        sync_params_data() {
            this->vnode = nullptr;
            this->onode = nullptr;
            this->vnode_reg = nullptr;
            this->param_data = nullptr;
            this->ignore_next_onode_event = true;
            this->listeners = {};
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

            if (this->param_data) {
                free(this->param_data);
                this->param_data = nullptr;
            }
        }
    };

    struct vnode_data {
        uint32_t id;
        pw_context *context;
        pw_core *core;
        pw_stream *stream;

        vnode_data(uint32_t id, pw_context *context, pw_core *core, pw_stream *stream) {
            this->id = id;
            this->context = context;
            this->core = core;
            this->stream = stream;
        }

        ~vnode_data() {
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

  private:
    static unordered_map<uint32_t, NodesManager::onode_info *> onode_infos;
    static unordered_map<uint32_t, VolumeStores::vnode_data *> onode_to_vnode;
    static unordered_map<uint32_t, VolumeStores::sync_params_data *> onode_to_sync_data;

    template <typename T>
    static void remove_entry_with_onode(uint32_t onode_id, unordered_map<uint32_t, T *> &map);
    static void log(string msg);
    static bool get_onode_binary_name(uint32_t onode_id, string &name);

  public:
    class FriendAccessor {
      private:
        friend class VolumeManager;

        static vnode_data &get_modifiable_vnode_data(uint32_t onode_id);
        static NodesManager::onode_info &get_modifiable_onode_info(uint32_t onode_id);
        static sync_params_data &get_modifiable_sync_params_data(uint32_t onode_id);
        static void cleanup_entries_with_onode_id(uint32_t onode_id);
        static void cleanup();
    };
};

class VolumeManager {
  private:
    struct process_and_vnode_args_data;

    static void *post_node_process_hook(NodesManager::create_vnode_args *vnode_args, void *data);

    static void on_state_change_single_callback(void *data, enum pw_stream_state old, enum pw_stream_state state,
                                                const char *error);
    static void *post_state_process_hook(void *data);

    static void on_vnode_param_props(void *data, uint32_t id, const struct spa_pod *param);
    static void on_onode_param_props(void *data, int seq, uint32_t id, uint32_t index, uint32_t next,
                                     const struct spa_pod *param);

  public:
    static void process_new_node(pw_registry *reg, pw_loop *loop, uint32_t id, const char *type);
    static void on_global_remove(void *data, uint32_t id);
    static void cleanup();
};
