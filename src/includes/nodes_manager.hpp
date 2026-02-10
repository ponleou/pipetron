#pragma once

#include "pipewire/node.h"
#include "pipewire/stream.h"
#include <spa/param/audio/format-utils.h>
#include <string>
#include <unordered_map>
#include <vector>

using std::string;
using std::unordered_map;
using std::vector;

class NodesManager {
  public:
    struct onode_info {
        const uint32_t id;
        string app_process_binary;
        string media_class;
        string media_name;
        spa_audio_info_raw audio_info;

        onode_info(uint32_t id) : id(id) {
            this->audio_info = {};
        }
    };
    ;

    struct create_vnode_args {
        struct pw_loop &loop;
        const NodesManager::onode_info &onode;

        create_vnode_args(struct pw_loop &loop, const NodesManager::onode_info &onode) : loop(loop), onode(onode) {
        }
    };
    ;
    struct create_vnode_output {
        pw_stream *vstream;
        pw_context *vcontext;
        pw_core *vcore;
    };
    ;

    struct process_onode_info_args {
        NodesManager::onode_info &onode;
        bool info_flag;
        bool params_flag;

        process_onode_info_args(NodesManager::onode_info &onode_info) : onode(onode_info) {
            this->info_flag = false;
            this->params_flag = false;
        }
    };
    ;

    struct nodes_manager_args_data {
        NodesManager::process_onode_info_args *process_args;
        NodesManager::create_vnode_args *vnode_args;
        spa_hook *self_listener;
        void *(*post_node_process_hook)(NodesManager::create_vnode_args *vnode_args, void *data);
        void *node_processed_hook_args;

        nodes_manager_args_data(pw_loop &loop, NodesManager::onode_info &onode_info,
                                void *(*node_processed_hook)(NodesManager::create_vnode_args *vnode_args, void *data),
                                void *data) {
            this->process_args = new NodesManager::process_onode_info_args(onode_info);
            this->vnode_args = new NodesManager::create_vnode_args(loop, onode_info);
            this->post_node_process_hook = node_processed_hook;
            this->node_processed_hook_args = data;
            this->self_listener = new spa_hook();
        }

        ~nodes_manager_args_data() {
            if (this->process_args) {
                delete this->process_args;
                this->process_args = nullptr;
            }

            if (this->vnode_args) {
                delete this->vnode_args;
                this->vnode_args = nullptr;
            }

            if (this->self_listener) {
                spa_hook_remove(this->self_listener);
                delete this->self_listener;
                this->self_listener = nullptr;
            }

            if (this->post_node_process_hook) {
                this->post_node_process_hook = nullptr;
            }

            if (this->node_processed_hook_args) {
                this->node_processed_hook_args = nullptr;
            }
        }
    };
    ;

  private:
    static void maybe_run_post_process(nodes_manager_args_data *args);

    static void on_node_info_process_onode_info(void *data, const struct pw_node_info *info);
    static void on_node_info_process_callback(void *data, const struct pw_node_info *info);

    static void on_node_param_process_onode_info(void *data, int seq, uint32_t id, uint32_t index, uint32_t next,
                                                 const struct spa_pod *param);
    static void on_node_param_process_callback(void *data, int seq, uint32_t id, uint32_t index, uint32_t next,
                                               const struct spa_pod *param);

  public:
    static void process_new_node(pw_node *node, nodes_manager_args_data *args);
    static void create_virtual_node(create_vnode_args &args, create_vnode_output &output);
};
