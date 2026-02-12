#pragma once

#include "nodes_manager.hpp"
#include "pipewire/stream.h"
#include <array>
#include <cstdint>
#include <spa/param/audio/format-utils.h>
#include <string>
#include <unordered_map>
#include <vector>

using std::array;
using std::string;
using std::unordered_map;
using std::vector;

/**
 * Stores essential information used by `AudioManager` inside three maps keyed by `onode_id`:
 *   - onode_infos:       contains `NodeManager::onode_info`.
 *   - onode_to_vnode_data:    contains `vnode_data`.
 *   - onode_to_sync_params_data: contains `sync_params_data`.
 *
 * Structs are public
 * The maps are modifiable through functions accessible by friends of `AudioStores::FriendAccessor`
 */
class AudioStores {
  public:
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
    static unordered_map<uint32_t, NodesManager::node_info *> omic_infos;

    static unordered_map<uint32_t, NodesManager::node_info *> onode_infos;
    static unordered_map<uint32_t, AudioStores::vnode_data *> onode_to_vnode_data;
    static unordered_map<uint32_t, AudioStores::vnode_data *> onode_to_capture_node_data;
    /**
     * Removes and deletes the entry for `node_id` from the given map, if present.
     *
     * @param node_id  The `onode_id` entry to delete from the map
     * @param map       The map to remove the entry from
     */
    template <typename T>
    static void remove_entry_with_node_id(uint32_t node_id, unordered_map<uint32_t, T *> &map);

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
     * Contains functions to modify `AudioStores` data
     *
     * Only accessible by its friends
     */
    class FriendAccessor {
      private:
        friend class AudioManager;

        /**
         * Provides the reference to the `onode_to_vnode_data` map entry for `onode_id` key. If the entry does not
         * exist, the entry will be created lazily.
         *
         * @return reference to the `vnode_data` entry for `onode_id` key.
         */
        static vnode_data &get_modifiable_vnode_data(uint32_t onode_id);

        static vnode_data &get_modifiable_capture_node_data(uint32_t onode_id);

        /**
         * Provides the reference to the `onode_infos` map entry for `onode_id` key. If the entry does not exist, the
         * entry will be created lazily.
         *
         * @return reference to the `onode_info` entry for `onode_id` key.
         */
        static NodesManager::node_info &get_modifiable_onode_info(uint32_t onode_id);

        /**
         * Provides the reference to the `omic_infos` map entry for `onode_id` key. If the entry does not exist, the
         * entry will be created lazily.
         *
         * @return reference to the `onode_info` entry for `onode_id` key.
         */
        static NodesManager::node_info &get_modifiable_omic_info(uint32_t onode_id);

        /**
         * Deletes and removes all data entries within all maps from `AudioStores` connected with `onode_id`.
         *
         * @param onode_id Key to delete all data entries from all maps
         */
        static void cleanup_elec_node_entries_with_onode_id(uint32_t onode_id);

        /**
         * Deletes and clears all entries in every map in `AudioStores`.
         */
        static void cleanup();
    };
};

class AudioManager {
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
    static void *post_elec_node_process_hook(NodesManager::create_node_args *vnode_args, void *data);

    /**
     * Hook called after `NodesManager::process_mic_node`
     *
     * It calls `NodesManager::replicate_vnode` and continues with next steps to set up the virtual node, which
     * includes calling a `state_changed` callback and hook
     *
     * @param vnode_args Passed by `NodesManager::process_new_node` which is specifically used to call
     * `NodesManager::replicate_virtual_node`
     * @param data The pointer to the data from `NodesManager::process_new_node` 's post hook arguments
     */
    static void *post_mic_process_hook(NodesManager::create_node_args *vnode_args, void *data);

    static void connect_capture_to_elec_node(const uint32_t elec_node_id, pw_loop &loop);

  public:
    static void enlist_registry_port_event(const uint32_t id, const struct spa_dict *props);

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
    static void process_elec_node(pw_registry *reg, pw_loop *loop, uint32_t id, const char *type);

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
    static void process_mic_node(pw_registry *reg, pw_loop *loop, uint32_t id, const char *type);

    /**
     * Registry global-remove callback. Cleans up all stored data from `AudioStores` associated with the removed node
     * ID.
     */
    static void enlist_registry_node_remove_event(uint32_t id);

    /** Cleans up all entries in AudioStores. Call on shutdown. */
    static void cleanup();
};