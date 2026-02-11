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

/**
 * Provides common node managament functions for other managers.
 */
class NodesManager {
  private:
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
    struct onode_info : node_desc {
        const uint32_t id;
        spa_audio_info_raw audio_info;

        onode_info(uint32_t id) : id(id) {
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
    struct replicate_vnode_args {
        pw_loop &loop;
        const onode_info &onode;
        node_desc override_desc;

        replicate_vnode_args(pw_loop &loop, const NodesManager::onode_info &onode) : loop(loop), onode(onode) {
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
    struct replicate_vnode_output {
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
        NodesManager::onode_info &onode;
        bool info_flag;
        bool params_flag;

        process_onode_info_args(NodesManager::onode_info &onode_info) : onode(onode_info) {
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
     * `replicate_vnode_args` passed into the arguments for calling `replicate_virtual_node`.
     * @param node_processed_hook_args  Argument data passed to `post_node_process_hook`.
     */
    struct nodes_manager_args_data {
        NodesManager::process_onode_info_args *process_args;
        NodesManager::replicate_vnode_args *vnode_args;
        spa_hook *self_listener;
        void *(*post_node_process_hook)(NodesManager::replicate_vnode_args *vnode_args, void *data);
        void *node_processed_hook_args;

        nodes_manager_args_data(pw_loop &loop, NodesManager::onode_info &onode_info,
                                void *(*node_processed_hook)(NodesManager::replicate_vnode_args *vnode_args,
                                                             void *data),
                                void *data) {
            this->process_args = new NodesManager::process_onode_info_args(onode_info);
            this->vnode_args = new NodesManager::replicate_vnode_args(loop, onode_info);
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
    static void on_node_info_process_onode_info(void *data, const struct pw_node_info *info);

    /**
     * PipeWire node `info` event callback. Calls `on_node_info_process_onode_info` and calls `maybe_run_post_process`.
     *
     * @param data  Pointer to `nodes_manager_args_data`.
     */
    static void on_node_info_process_callback(void *data, const struct pw_node_info *info);

    /**
     * Called from the `param` event callback function. Processes `audio_info` from the PipeWire node info properties
     * into `onode_info` and sets `param_flag` to true once successful.
     *
     * @param data  Pointer to `process_onode_info_args`.
     */
    static void on_node_param_process_onode_info(void *data, int seq, uint32_t id, uint32_t index, uint32_t next,
                                                 const struct spa_pod *param);

    /**
     * PipeWire node `param` event callback. Calls `on_node_param_process_onode_info` and calls
     * `maybe_run_post_process`.
     *
     * @param data  Pointer to `nodes_manager_args_data`.
     */
    static void on_node_param_process_callback(void *data, int seq, uint32_t id, uint32_t index, uint32_t next,
                                               const struct spa_pod *param);

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
    static void replicate_vnode(const replicate_vnode_args &args, replicate_vnode_output &output);

    // static void replicate_vnode_node_desc(const replicate_vnode_args &args, replicate_vnode_output &output);
};
