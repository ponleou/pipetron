#pragma once

#include "pipewire/core.h"
#include "pipewire/node.h"
#include "pipewire/stream.h"
#include <cstdint>
#include <iostream>
#include <spa/param/audio/format-utils.h>
#include <string>
#include <unordered_map>
#include <vector>

using std::string;
using std::unordered_map;
using std::vector;

class PortLinksManager {
  private:
    /**
     * Removes and deletes the entry for `node_id` from the given map, if present.
     *
     * @param node_id  The `onode_id` entry to delete from the map
     * @param map       The map to remove the entry from
     */
    template <typename T>
    static void remove_entry_with_node_id(uint32_t node_id, unordered_map<uint32_t, T *> &map);

    struct link_proxies_data {
        vector<pw_proxy **> link_proxies;
        uint32_t connected_node_id;

        ~link_proxies_data() {
            for (auto &ptr : link_proxies) {
                if (*ptr) {
                    pw_proxy_destroy(*ptr);
                    *ptr = nullptr;
                }
            }
            link_proxies.clear();
        }
    };

    struct link_connect_task_data {
        const uint32_t playback_node;
        const uint32_t capture_node;
        pw_core &core;

        const uint32_t channels;

        link_connect_task_data(const uint32_t playback_node, const uint32_t capture_node, pw_core &core,
                               const uint32_t channels)
            : playback_node(playback_node), capture_node(capture_node), core(core), channels(channels) {
        }
    };

    static vector<link_connect_task_data *> link_connect_tasks_list;

    static void work_link_connect_task();

    static void cleanup_port_infos_with_node_id(uint32_t node_id);
    static void remove_link_proxies_with_node_id(const uint32_t node_id);

    static void store_link_proxy_between_nodes(pw_proxy *link, const uint32_t node_id_one, const uint32_t node_id_two);
    static link_proxies_data &get_modifiable_link_proxies(const uint32_t node_id);

  public:
    class NodeManagerAccessor {
      private:
        friend class PortLinksManager;
        friend class NodesManager;

        struct port_info {
            const uint32_t id;
            string port_direction;
            string audio_channel;
            string node_id;

            port_info(const uint32_t id) : id(id) {
                this->audio_channel = "";
                this->port_direction = "";
                this->node_id = "";
            }

            port_info(const uint32_t id, string port_direction, string audio_channel, string node_id) : id(id) {
                this->audio_channel = audio_channel;
                this->port_direction = port_direction;
                this->node_id = node_id;
            }
        };

        struct port_infos {
            vector<port_info *> ports_list;

            port_infos() {
                this->ports_list = {};
            }
            void insert_port_info(const uint32_t id, string port_direction, string audio_channel, string node_id) {
                this->ports_list.push_back(new port_info(id, port_direction, audio_channel, node_id));
            }

            ~port_infos() {
                for (port_info *port : this->ports_list) {
                    delete port;
                    port = nullptr;
                }
                this->ports_list.clear();
            }
        };

        static const unordered_map<uint32_t, port_infos *> &get_port_infos_map();
        static void enqueue_link_connection(const uint32_t playback_node_id, const uint32_t capture_node_id,
                                            pw_core &core, const uint32_t channels);
    };

    static void enlist_registry_port_event(const uint32_t id, const struct spa_dict *props);
    static void enlist_registry_node_remove_event(const uint32_t id);
    static void cleanup();

  private:
    static unordered_map<uint32_t, NodeManagerAccessor::port_infos *> node_id_to_port_infos;

    /**
     * THIS CAN LITERALLY ONLY STORE ONE CONNECTION PER NODE
     * SO ONE NODE CAN ONLY BE CONNECTED TO ANOTHER NODE AND BE STORED IN HERE
     */
    static unordered_map<uint32_t, link_proxies_data *> node_id_to_link_proxies;

    static NodeManagerAccessor::port_infos &get_modifiable_port_infos_entry(uint32_t node_id);
};

/**
 * Provides common node managament functions for other managers.
 */
class NodesManager {
  private:
    struct state_change_enqueue_connect_capture_to_onode_args {
        const uint32_t onode_id;
        const uint32_t channels;
        pw_core &core;
        pw_stream &stream;
        spa_hook *self_listener;

        state_change_enqueue_connect_capture_to_onode_args(const uint32_t onode_id, const int32_t channels,
                                                           pw_core &core, pw_stream &stream, spa_hook *listener)
            : onode_id(onode_id), channels(channels), core(core), stream(stream) {
            this->self_listener = listener;
        }

        ~state_change_enqueue_connect_capture_to_onode_args() {
            if (this->self_listener) {
                spa_hook_remove(this->self_listener);
                delete this->self_listener;
                this->self_listener = nullptr;
            }
        }
    };

    /**
     * Contains string description of node
     *
     * @param app_process_binary    The binary name of the application owning this node.
     * @param node_description      The node description
     * @param media_class           The media class (e.g. "Stream/Output/Audio").
     * @param media_name            The media name (e.g. the stream title).
     */
    struct node_desc {
        string app_name;
        string app_icon_name;
        string app_process_binary;
        string node_description;
        string media_class;
        string media_name;
    };

  public:
    /**
     * Stores metadata gathered from an original PipeWire node.
     *
     * @param id                    The PipeWire node ID.
     * @param audio_info            The raw audio format info (channels, rate, format) parsed from SPA_PARAM_Format.
     */
    struct node_info : node_desc {
        const uint32_t id;
        spa_audio_info_raw audio_info;

        node_info(uint32_t id) : id(id) {
            this->audio_info = {};
        }
    };
    ;

    /**
     * Input arguments for `NodesManager::replicate_virtual_node`.
     *
     * @param loop          The PipeWire main loop the virtual node's context will run on.
     * @param onode         The original node's metadata used to replicate its properties.
     * @param override_desc Optional node descriptions that will override `onode` 's metadata when replicating
     * properties.
     */
    struct create_node_args {
        pw_loop &loop;
        const node_info &onode;
        node_desc override_desc;

        create_node_args(pw_loop &loop, const NodesManager::node_info &onode) : loop(loop), onode(onode) {
        }
    };
    ;
    /**
     * Output of `NodesManager::replicate_virtual_node`. Contains the PipeWire objects that make up the newly created
     * virtual node.
     *
     * @param vstream   The pw_stream of the virtual node.
     * @param vcontext  The pw_context of the virtual node.
     * @param vcore     The pw_core connection of the virtual node.
     */
    struct create_node_output {
        pw_stream *vstream;
        pw_context *vcontext;
        pw_core *vcore;
    };
    ;

    /**
     * Contains reference to `onode_info` and other params that tracks the progress of asynchronously gathering an
     * original node's metadata. Both `info_flag` and `params_flag` must be true before processing is considered
     * complete. Processing requires going through `info` and `param` PipeWire events callback.
     *
     * @param onode        Reference to the `onode_info` being populated during the processes.
     * @param info_flag    Set to true once the `info` callback has been processed.
     * @param params_flag  Set to true once the `param` callback has been processed.
     */
    struct process_onode_info_args {
        NodesManager::node_info &onode;
        bool info_flag;
        bool params_flag;

        process_onode_info_args(NodesManager::node_info &onode_info) : onode(onode_info) {
            this->info_flag = false;
            this->params_flag = false;
        }
    };
    ;

    /**
     * Main callback data struct passed through `process_new_node` 's PipeWire event pipeline. Once `onode_info` is
     * collected inside `process_args`, `self_listener` is removed and `post_node_process_hook` is called with
     * `vnode_args` and `node_processed_hook_args` arguments.
     *
     * @param process_args              Argument passed through `onode_info` processing.
     * @param vnode_args                Arguments passed to `post_node_process_hook` for virtual node creation. Contains
     * the same const reference to `onode_info` as `process_args`
     * @param self_listener             The `spa_hook` for `info` and `param` PipeWire event listener.
     * @param post_node_process_hook    Hook function called once `process_new_node` is finished. Includes
     * `create_node_args` passed into the arguments for calling `replicate_virtual_node`.
     * @param node_processed_hook_args  Argument data passed to `post_node_process_hook`.
     */
    struct nodes_manager_args_data {
        NodesManager::process_onode_info_args *process_args;
        NodesManager::create_node_args *vnode_args;
        spa_hook *self_listener;
        void *(*post_node_process_hook)(NodesManager::create_node_args *vnode_args, void *data);
        void *node_processed_hook_args;

        nodes_manager_args_data(pw_loop &loop, NodesManager::node_info &onode_info,
                                void *(*node_processed_hook)(NodesManager::create_node_args *vnode_args, void *data),
                                void *data) {
            this->process_args = new NodesManager::process_onode_info_args(onode_info);
            this->vnode_args = new NodesManager::create_node_args(loop, onode_info);
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
    /**
     * Runs after both `info_flag` and `params_flag` are set, which removes `self_listener`, calls
     * `post_node_process_hook` with `vnode_args` and `node_processed_hook_args`.
     *
     * @param args  The argument data passed from the `process_new_node` function call
     */
    static void maybe_run_post_process(nodes_manager_args_data *args);

    /**
     * Called from the `info` event callback function. Extracts `app_process_binary`, `media_class`, and `media_name`
     * from the PipeWire node info properties into `onode_info` and sets `info_flag` to true once successful.
     *
     * @param data  Pointer to `process_onode_info_args`.
     */
    static void on_node_info_process_onode_info(void *data, const pw_node_info *info);

    /**
     * PipeWire node `info` event callback. Calls `on_node_info_process_onode_info` and calls `maybe_run_post_process`.
     *
     * @param data  Pointer to `nodes_manager_args_data`.
     */
    static void on_node_info_process_callback(void *data, const pw_node_info *info);

    /**
     * Called from the `param` event callback function. Processes `audio_info` from the PipeWire node info properties
     * into `onode_info` and sets `param_flag` to true once successful.
     *
     * @param data  Pointer to `process_onode_info_args`.
     */
    static void on_node_param_process_onode_info(void *data, int seq, uint32_t id, uint32_t index, uint32_t next,
                                                 const spa_pod *param);

    /**
     * PipeWire node `param` event callback. Calls `on_node_param_process_onode_info` and calls
     * `maybe_run_post_process`.
     *
     * @param data  Pointer to `nodes_manager_args_data`.
     */
    static void on_node_param_process_callback(void *data, int seq, uint32_t id, uint32_t index, uint32_t next,
                                               const spa_pod *param);

    static void on_state_change_enqueue_connect_capture_to_onode_single_callback(void *data, enum pw_stream_state old,
                                                                                 enum pw_stream_state state,
                                                                                 const char *error);

  public:
    /**
     * Entry point for processing a new PipeWire node, which creates `info` and `param` event listener on `node` for
     * calling `on_node_info_process_callback` and `on_node_param_process_callback` callbacks. This processing cycle
     * ends once `info_flag` and `param_flag` is true, which it will call `maybe_run_post_process` to finish the job
     *
     * @param node  The PipeWire node proxy to process.
     * @param args  The callback data containing progress tracking, vnode args, and the post-process hook.
     */
    static void process_new_node(pw_node *node, nodes_manager_args_data *args);

    /**
     * Creates a virtual PipeWire node that replicates the original node's properties (app name, media class, icon,
     * binary) and audio format. Should be called inside `post_node_process_hook`, started by `process_new_node`.
     *
     * @param args      Input args provided inside `post_node_process_hook`.
     * @param output    Output struct populated with the created stream, context, and core.
     */
    static void replicate_vnode(const create_node_args &args, create_node_output &output);

    static void connect_capture_to_onode(const create_node_args &args, create_node_output &output);
};
