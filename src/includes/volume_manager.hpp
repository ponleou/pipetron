#pragma once

#include "nodes_manager.hpp"
#include "pipewire/stream.h"
#include <array>
#include <spa/param/audio/format-utils.h>
#include <string>
#include <unordered_map>
#include <vector>

using std::array;
using std::string;
using std::unordered_map;
using std::vector;

/** @defgroup glossary Project Terms
 *
 * - Single callback: One-shot Pipewire event callbacks where it is set to run once, and delete its own listener after a
 * successful run or process. May contain post hook functions which runs after its one-shot process.
 */

/**
 * Contains function argument structs passed as void* data pointers within PipeWire's event callbacks, and event
 * callbacks' hooks.
 *
 * Primarily contains `onode_id` used to access data entry within VolumeStores
 */
class VolumeManagerArgs {
  private:
    friend class VolumeManager;

    /**
     * Arguments for `VolumeManager::post_state_process_hook`
     *
     * @param onode_id Used to access `vnode_data`, and modify `sync_params_data` within VolumeStores
     */
    struct post_state_process_hook_args {
        const uint32_t onode_id;

        post_state_process_hook_args(const uint32_t onode_id) : onode_id(onode_id) {
        }
    };

    /**
     * Arguments for `VolumeManager::on_state_change_single_callback`.
     *
     * @param onode_id                          The original node ID.
     * @param stream_processed_flag             A boolean flag to check whether the single callback has been processed.
     * @param self_listener                     The `spa_hook` listener for `on_state_change_single_callback`,
     * automatically removed within the callback once processed.
     * @param post_state_process_hook         A hook function called after the the single callback is processed.
     * @param post_state_process_hook_args    The arguments passed as data into `post_state_process_hook`.
     */
    struct state_change_single_callback_args {
        const uint32_t onode_id;
        bool stream_processed_flag;

        spa_hook *self_listener;

        void *(*post_state_process_hook)(void *args);
        void *post_state_process_hook_args;

        state_change_single_callback_args(const uint32_t onode_id, void *(*post_state_process_hook)(void *args),
                                          void *post_state_process_hook_args)
            : onode_id(onode_id) {
            this->stream_processed_flag = false;
            this->self_listener = new spa_hook();
            this->post_state_process_hook = post_state_process_hook;
            this->post_state_process_hook_args = post_state_process_hook_args;
        }

        ~state_change_single_callback_args() {
            if (this->self_listener) {
                spa_hook_remove(this->self_listener);
                delete this->self_listener;
                this->self_listener = nullptr;
            }
        }
    };
};

/**
 * Stores essential information used by `VolumeManager` inside three maps keyed by `onode_id`:
 *   - onode_infos:       contains `NodeManager::onode_info`.
 *   - onode_to_vnode_data:    contains `vnode_data`.
 *   - onode_to_sync_params_data: contains `sync_params_data`.
 *
 * Structs are public
 * The maps are modifiable through functions accessible by friends of `VolumeStores::FriendAccessor`
 */
class VolumeStores {
  public:
    /**
     * Contains data required to sync volume data between onode and vnode
     *
     * @param vnode_reg               vnode's registry, required to maintain accessibility of `vnode`
     * @param vnode                   pw_node of vnode, used to read and copy volume data to `param_data`
     * @param onode                   pw_node of onode, used to set volume data from `param_data`
     * @param ignore_next_onode_event Flag to prevent event callback loop when onode's volume data is modified
     * @param param_data              Contains volume data from `vnode`
     * @param listeners               Maintains `onode` and `vnode` 's `param_changed` event listeners for cleanup.
     */
    struct sync_params_data {
        pw_registry *vnode_reg;
        pw_node *vnode;

        pw_node *onode;
        bool ignore_next_onode_event;
        spa_pod *param_data;
        array<spa_hook *, 2> listeners;

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
            this->listeners.fill(nullptr);

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

    /**
     * Contains the data of `vnode` to maintain the node's instance (context, core, stream, id)
     *
     * @param id       `vnode` 's PipeWire node ID.
     * @param context  The pw_context of `vnode`.
     * @param core     The pw_core of `vnode`.
     * @param stream   The pw_stream of `vnode`.
     */
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
    static unordered_map<uint32_t, NodesManager::node_info *> onode_infos;
    static unordered_map<uint32_t, VolumeStores::vnode_data *> onode_to_vnode_data;
    static unordered_map<uint32_t, VolumeStores::sync_params_data *> onode_to_sync_params_data;

    /**
     * Removes and deletes the entry for `onode_id` from the given map, if present.
     *
     * @param onode_id  The `onode_id` entry to delete from the map
     * @param map       The map to remove the entry from
     */
    template <typename T>
    static void remove_entry_with_onode(uint32_t onode_id, unordered_map<uint32_t, T *> &map);

    static void log(string msg);

    /**
     * Looks up the binary name of the application of `onode_id`.
     *
     * @param onode_id  The `onode_id` application to get
     * @param name      A string reference where its binary name will be set to.
     * @return          true if the binary name was found and set to `name`, false otherwise.
     */
    static bool get_onode_binary_name(uint32_t onode_id, string &name);

  public:
    /**
     * Contains functions to modify `VolumeStores` data
     *
     * Only accessible by its friends
     */
    class FriendAccessor {
      private:
        friend class VolumeManager;

        /**
         * Provides the reference to the `onode_to_vnode_data` map entry for `onode_id` key. If the entry does not
         * exist, the entry will be created lazily.
         *
         * @return reference to the `vnode_data` entry for `onode_id` key.
         */
        static vnode_data &get_modifiable_vnode_data(uint32_t onode_id);

        /**
         * Provides the reference to the `onode_infos` map entry for `onode_id` key. If the entry does not exist, the
         * entry will be created lazily.
         *
         * @return reference to the `onode_info` entry for `onode_id` key.
         */
        static NodesManager::node_info &get_modifiable_onode_info(uint32_t onode_id);

        /**
         * Provides the reference to the `onode_to_sync_params_data` map entry for `onode_id` key. If the entry does not
         * exist, the entry will be created lazily.
         *
         * @return reference to the `sync_params_data` entry for `onode_id` key.
         */
        static sync_params_data &get_modifiable_sync_params_data(uint32_t onode_id);

        /**
         * Deletes and removes all data entries within all maps from `VolumeStores` connected with `onode_id`.
         *
         * @param onode_id Key to delete all data entries from all maps
         */
        static void cleanup_entries_with_onode_id(uint32_t onode_id);

        /**
         * Deletes and clears all entries in every map in `VolumeStores`.
         */
        static void cleanup();
    };
};

/**
 * Main manager to control volume syncing for `VolumeDaemon`
 */
class VolumeManager {
  private:
    struct process_and_vnode_args_data;

    /**
     * Hook called after `NodesManager::process_new_node`
     *
     * It calls `NodesManager::replicate_virtual_node` and continues with next steps to set up the virtual node, which
     * includes calling a `state_changed` callback and hook
     *
     * @param vnode_args Passed by `NodesManager::process_new_node` which is specifically used to call
     * `NodesManager::replicate_virtual_node`
     * @param data The pointer to the data from `NodesManager::process_new_node` 's post hook arguments
     */
    static void *post_node_process_hook(NodesManager::create_node_args *vnode_args, void *data);

    /**
     * A one-shot single callback for PipeWire `vnode` 's `state_change` event. Once the state is
     * PW_STREAM_STATE_PAUSED, indicating the stream is ready, and it can record the stream's node ID.
     *
     * After recording the node ID, it removes its own listener, and calls `post_state_process_hook`.
     */
    static void on_state_change_single_callback(void *data, enum pw_stream_state old, enum pw_stream_state state,
                                                const char *error);

    /**
     * A hook function to run after `on_state_change_single_callback`
     *
     * It sets up the one-way volume data sync between `vnode` and `onode`. It creates events for `param_changed` on
     * `vnode` to call `on_vnode_param_props`, and event for `param` on `onode` to call `on_onode_param_props`.
     *
     * Also sets up `sync_param_data` from `VolumeStores`.
     *
     * @param data The pointer to the data for post hook arguments where the post hook was set to run.
     */
    static void *post_state_process_hook(void *data);

    /**
     * Callback for `vnode` `param_changed` event. Copies the new SPA_PARAM_Props pod, containing volume data, into
     * `sync_param_data` and and syncs `onode` to the data. Also sets `ignore_next_onode_event` from `sync_param_data`
     * to skip the next `onode` 's `param` PipeWire event triggered when syncing `onode` volume data
     */
    static void on_vnode_param_props(void *data, uint32_t id, const struct spa_pod *param);

    /**
     * Callback for `onode` `param` event. Re-syncs its volume data to the data stored inside `sync_param_data`. Also
     * sets `ignore_next_onode_event` from `sync_param_data` to skip the next `onode` 's `param` PipeWire event
     * triggered when syncing `onode` volume data
     *
     * If `ignore_next_onode_event` is true, the function exits.
     */
    static void on_onode_param_props(void *data, int seq, uint32_t id, uint32_t index, uint32_t next,
                                     const struct spa_pod *param);

  public:
    /**
     * Entry point when a new PipeWire global node appears. Binds the original node from the registry and initiates
     * processing via NodesManager, which will gather node metadata and create a matching virtual node. Process
     * continues with setting up `vnode` and the syncing process.
     *
     * @param reg   The PipeWire registry the node was discovered in.
     * @param loop  The PipeWire main loop, used for the virtual node's context.
     * @param id    The PipeWire global ID of the new node.
     * @param type  The PipeWire type string of the global (e.g. PipeWire:Interface:Node).
     */
    static void process_new_node(pw_registry *reg, pw_loop *loop, uint32_t id, const char *type);

    /**
     * Registry global-remove callback. Cleans up all stored data from `VolumeStores` associated with the removed node
     * ID.
     */
    static void on_global_remove(void *data, uint32_t id);

    /** Cleans up all entries in VolumeStores. Call on shutdown. */
    static void cleanup();
};
