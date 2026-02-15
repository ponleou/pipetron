#pragma once

#include "nodes_manager.hpp"
#include "pipewire/core.h"
#include "pipewire/stream.h"
#include <array>
#include <cstdint>
#include <queue>
#include <spa/param/audio/format-utils.h>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using std::array;
using std::move;
using std::queue;
using std::string;
using std::unordered_map;
using std::vector;

class AudioManagerArgs {
  private:
    friend class AudioManager;

    /**
     * Args struct passed to `post_elec_node_process_hook` function.
     *
     * @param onode_id the node ID of the original/electron node
     * @param reg pw_registry reference which will be used to create `on_state_changed_vnode_copy_link_direction_args`
     */
    struct post_elec_node_process_hook_args {
        const uint32_t onode_id;
        pw_registry &reg;

        post_elec_node_process_hook_args(const uint32_t onode_id, pw_registry &reg) : onode_id(onode_id), reg(reg) {
        }
    };

    /**
     * Args struct passed to `on_state_changed_vnode_copy_link_direction_single_callback`. Contains all data required to
     * call NodeManager's `copy_playback_link_direction` after getting the stream's ID in the `state_changed` callback.
     * Contains self_listener for the single callback.
     */
    struct on_state_changed_vnode_copy_link_direction_args {
        const uint32_t onode_id;
        pw_registry &reg;
        pw_core &vnode_core;
        pw_stream &vnode_stream;
        const uint32_t vnode_channels;
        spa_hook *self_listener;

        on_state_changed_vnode_copy_link_direction_args(const uint32_t onode_id, pw_registry &reg, pw_core &vnode_core,
                                                        pw_stream &vnode_stream, const uint32_t vnode_channels)
            : onode_id(onode_id), reg(reg), vnode_core(vnode_core), vnode_stream(vnode_stream),
              vnode_channels(vnode_channels) {
            this->self_listener = new spa_hook();
        }

        ~on_state_changed_vnode_copy_link_direction_args() {
            if (this->self_listener) {
                spa_hook_remove(this->self_listener);
                delete this->self_listener;
                this->self_listener = nullptr;
            }
        }
    };
};

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

    /**
     * Data struct stored in a map to the original node ID that holds `process` listeners for the capture node and the
     * virtual node, which are responsible for copying audio data from the capture node into the virtual node for
     * playback. Has all the essential data to keep `process` callback running and copying audio data in isolation.
     *
     * @param capture_node  the capture node's pw_stream object, used to dequeue buffer and copy audio data into queue
     * @param vnode         the playback virtual node's pw_stream object, used to dequeue buffer and copy audio data
     * from queue
     * @param audio_queue   contains audio data in queue from `capture_node` to be taken by `vnode` playback
     * @param listeners     holds pointers to the two `process` listeners for `capture_node` and `vnode`
     *
     * @param store_buffer_to_queue   called by `capture_node` 's process callback to store buffer data into the queue
     * @param load_buffer_from_queue  called by `vnode` 's process callback to copy data from queue into its buffer
     */
    struct transfer_audio_data {
        pw_stream &capture_node;
        pw_stream &vnode;
        queue<vector<uint8_t>> audio_queue;
        array<spa_hook *, 2> listeners;

        transfer_audio_data(pw_stream &capture_node, pw_stream &vnode) : capture_node(capture_node), vnode(vnode) {
            this->audio_queue = {};
            this->listeners[0] = new spa_hook();
            this->listeners[1] = new spa_hook();
        }

        ~transfer_audio_data() {
            for (spa_hook *listener : this->listeners) {
                if (listener) {
                    spa_hook_remove(listener);
                    delete listener;
                    listener = nullptr;
                }
            }
            this->listeners.fill(nullptr);
        }

        void store_buffer_to_queue(pw_buffer *buffer) {
            if (buffer == nullptr)
                return;

            for (uint32_t i = 0; i < buffer->buffer->n_datas; i++) {
                vector<uint8_t> audio_data(buffer->buffer->datas[i].chunk->size);

                memcpy(audio_data.data(), buffer->buffer->datas[i].data, buffer->buffer->datas[i].chunk->size);
                this->audio_queue.push(move(audio_data));
            }
        }

        void load_buffer_from_queue(pw_buffer *buffer) {
            if (buffer == nullptr)
                return;

            for (uint32_t i = 0; i < buffer->buffer->n_datas; i++) {
                if (!this->audio_queue.empty()) {
                    vector<uint8_t> &audio_data = this->audio_queue.front();

                    memcpy(buffer->buffer->datas[i].data, audio_data.data(), audio_data.size());
                    buffer->buffer->datas[i].chunk->size = audio_data.size();

                    this->audio_queue.pop();
                } else {
                    // added this because there were buzzing sound
                    // i guess queuing an empty buffer causes buzzing
                    memset(buffer->buffer->datas[i].data, 0, buffer->buffer->datas[i].maxsize);
                    buffer->buffer->datas[i].chunk->size = buffer->buffer->datas[i].maxsize;
                }
            }
        }
    };

  private:
    static unordered_map<uint32_t, NodesManager::node_info *> omic_infos;

    static unordered_map<uint32_t, NodesManager::node_info *> elec_node_infos;
    static unordered_map<uint32_t, AudioStores::vnode_data *> elec_node_to_vnode_data;
    static unordered_map<uint32_t, AudioStores::vnode_data *> elec_node_to_capture_node_data;

    static unordered_map<uint32_t, AudioStores::transfer_audio_data *> elec_node_to_transfer_audio_data;
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
         * Provides the reference to the `elec_node_to_vnode_data` map entry for `elec_node_id` key. If the entry
         * does not exist, the entry will be created lazily.
         *
         * @return reference to the `vnode_data` entry for `elec_node_id` key.
         */
        static vnode_data &get_modifiable_vnode_data(uint32_t elec_node_id);

        /**
         * Provides the reference to the `elec_node_to_capture_node_data` map entry for `elec_node_id` key. If the
         * entry does not exist, the entry will be created lazily.
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
         * Deletes existing `transfer_audio_data` instance in `elec_node_id` entry from
         * `elec_node_to_transfer_audio_data` map (if any), and creates a new `transfer_audio_data` instance.
         *
         * @return reference to the new `transfer_audio_data` instance
         */
        static transfer_audio_data &start_new_transfer_audio_data(uint32_t elec_node_id, pw_stream &capture,
                                                                  pw_stream &vnode);

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
     * `vnode` 's single callback for `state_changed` event, which on `PW_STREAM_STATE_PAUSED`, will get the stream's ID
     * and call NodeManager's `copy_playback_link_direction` for `vnode` as modifying node, and the electron (original)
     * node as node to be copied. Will delete its own `self_listener` from `data` after a successful run.
     *
     * @param data `on_state_changed_vnode_copy_link_direction_args` instance data
     */
    static void on_state_changed_vnode_copy_link_direction_single_callback(void *data, enum pw_stream_state old,
                                                                           enum pw_stream_state state,
                                                                           const char *error);

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
     * Callback used by the capture node connected to the original electron node. Called on `process` event, which will
     * copy the capture node's audio buffer into the `transfer_audio_data` instance queue.
     *
     * @param data data casted to `transfer_audio_data` which contains the info required to transfer between the two
     * pairs in isolation
     */
    static void on_process_capture_store_data_callback(void *data);

    /**
     * Callback used by the replicated virtual node for playback. Called on `process` event, which will copy audio data
     * from the `transfer_audio_data` queue into its buffer for playback.
     *
     * @param data data casted to `transfer_audio_data` which contains the info required to transfer between the two
     * pairs in isolation
     */
    static void on_process_vnode_play_data_callback(void *data);

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
     * `PortLinkManager::enlist_registry_port_event` calls its internal work to create enqueued links between nodes
     * on new port events. This uses a `pw_registry*` to complete.
     *
     * @param id    ID of the node from the global registry event
     * @param props props of the node from the global registry event
     * @param reg   required `PortLinkManager::enlist_registry_port_event`
     */
    static void enlist_registry_port_event(const uint32_t id, const struct spa_dict *props, pw_registry *reg);

    /**
     * Registry global-remove callback. Cleans up all stored data from `AudioStores` associated with the removed
     * node ID, and calls `PortLinkManager` 's own `enlist_registry_node_remove_event`
     *
     * @param id  ID of the removed pipewire item
     */
    static void enlist_registry_remove_event(uint32_t id);

    /** Cleans up all entries in AudioStores and PortLinkManager with its `cleanup` method. Call on shutdown. */
    static void cleanup();
};