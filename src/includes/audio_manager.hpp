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
 *   - elec_node_infos:                 contains the electron node info.
 *   - elec_node_to_vnode_data:         contains the pointers and id to the created virtual node mapped to the electron
 * node id
 *   - elec_node_to_capture_node_data:  contains the pointers and id to the create capture node for the mapped electron
 * node id it copied.
 *
 *
 * TODO: mic
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

    static unordered_map<uint32_t, NodesManager::node_info *> elec_node_infos;
    static unordered_map<uint32_t, AudioStores::vnode_data *> elec_node_to_vnode_data;
    static unordered_map<uint32_t, AudioStores::vnode_data *> elec_node_to_capture_node_data;
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
     * Looks up the binary name of the application of `node_id` from the `elec_node_infos` maps.
     *
     * @param node_id   The `node_id` of the electron application to get
     * @param name      A string reference where its binary name will be set to.
     * @return          true if the binary name was found and set to `name`, false otherwise.
     */
    static bool get_elec_node_binary_name(uint32_t node_id, string &name);

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
         * Provides the reference to the `elec_node_to_vnode_data` map entry for `elec_node_id` key. If the entry does
         * not exist, the entry will be created lazily.
         *
         * @return reference to the `vnode_data` entry for `elec_node_id` key.
         */
        static vnode_data &get_modifiable_vnode_data(uint32_t elec_node_id);

        /**
         * Provides the reference to the `elec_node_to_capture_node_data` map entry for `elec_node_id` key. If the entry
         * does not exist, the entry will be created lazily.
         *
         * @return reference to the `vnode_data` entry for `elec_node_id` key.
         */
        static vnode_data &get_modifiable_capture_node_data(uint32_t elec_node_id);

        /**
         * Provides the reference to the `elec_node_infos` map entry for `elec_node_id` key. If the entry does not
         * exist, the entry will be created lazily.
         *
         * @return reference to the `elec_node_info` entry for `elec_node_id` key.
         */
        static NodesManager::node_info &get_modifiable_elec_node_info(uint32_t elec_node_id);

        /**
         * TODO: mic
         * Provides the reference to the `omic_infos` map entry for `onode_id` key. If the entry does not exist, the
         * entry will be created lazily.
         *
         * @return reference to the `onode_info` entry for `onode_id` key.
         */
        static NodesManager::node_info &get_modifiable_omic_info(uint32_t onode_id);

        /**
         * Deletes and removes all data entries within maps from `AudioStores` connected with `elec_node_id`. These
         * includes maps with the prefix `elec_node_to_*` and `elec_node_infos` map.
         *
         * @param onode_id Key to delete all data entries from all maps
         */
        static void cleanup_stores_with_elec_node_id(uint32_t elec_node_id);

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
     * It creates a virtual node to replicate the playback electron node, and a capture node to capture the electron
     * node's audio data. // TODO: after capturing audio what to do?
     *
     * From calling `NodesManager::connect_capture_to_onode`, it creates the capture node, disconnects the playback
     * electron node's links, and make links from the playback electron node to the capture node.
     *
     * @param vnode_args  Passed by `NodesManager::process_new_node` which is specifically used to call
     * other `NodeManager` methods to create nodes with those information
     * @param data        The pointer to the data from `NodesManager::process_new_node` 's post hook arguments
     */
    static void *post_elec_node_process_hook(NodesManager::create_node_args *vnode_args, void *data);

    /**
     * // TODO: mic
     *
     * Hook called after `NodesManager::process_mic_node`
     *
     * It calls `NodesManager::replicate_vnode` and continues with next steps to set up the virtual node, which
     * includes calling a `state_changed` callback and hook
     *
     * @param vnode_args  Passed by `NodesManager::process_new_node` which is specifically used to call
     * `NodesManager::replicate_virtual_node`
     * @param data        The pointer to the data from `NodesManager::process_new_node` 's post hook arguments
     */
    static void *post_mic_process_hook(NodesManager::create_node_args *vnode_args, void *data);

  public:
    /**
     * Interface for AudioDaemon to call on global registry event when a playback electron node is found. It calls
     * `NodesManager::process_new_node` to collect the node's information, and assigned hooks to create virtual and
     * capture nodes, and processes the playback electron node's audio.
     *
     * @param reg   The PipeWire registry the node was discovered in.
     * @param loop  The PipeWire main loop, used for the virtual node's context.
     * @param id    The PipeWire global ID of the new node.
     * @param type  The PipeWire type string of the global (e.g. PipeWire:Interface:Node).
     */
    static void process_playback_elec_node(pw_registry *reg, pw_loop *loop, uint32_t id, const char *type);

    /**
     * TODO: mic
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
     * Interface for AudioDaemon to call on global registry event when the type is a `PW_TYPE_INTERFACE_Link`. It
     * calls `PortLinkManager::enlist_registry_link_event`
     *
     * @param id    ID of the node from the global registry event
     * @param props props of the node from the global registry event
     */
    static void enlist_registry_link_event(const uint32_t id, const struct spa_dict *props);

    /**
     * Interface for AudioDaemon to call on global registry event when the type is a `PW_TYPE_INTERFACE_Port`. It
     * calls `PortLinkManager::enlist_registry_port_event`
     *
     * `PortLinkManager::enlist_registry_port_event` calls its internal work to create enqueued links between nodes on
     * new port events. This uses a `pw_registry*` to complete.
     *
     * @param id    ID of the node from the global registry event
     * @param props props of the node from the global registry event
     * @param reg   required `PortLinkManager::enlist_registry_port_event`
     */
    static void enlist_registry_port_event(const uint32_t id, const struct spa_dict *props, pw_registry *reg);

    /**
     * Registry global-remove callback. Cleans up all stored data from `AudioStores` associated with the removed node
     * ID, and calls `PortLinkManager` 's own `enlist_registry_node_remove_event`
     *
     * @param id  ID of the removed pipewire item
     */
    static void enlist_registry_remove_event(uint32_t id);

    /** Cleans up all entries in AudioStores and PortLinkManager with its `cleanup` method. Call on shutdown. */
    static void cleanup();
};