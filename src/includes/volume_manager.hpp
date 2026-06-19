#pragma once

#include "nodes_manager.hpp"
#include <array>
#include <pipewire/context.h>
#include <pipewire/proxy.h>
#include <pipewire/stream.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/param/props.h>
#include <spa/pod/dynamic.h>
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
     * Arguments for VolumeManager::post_node_process_hook
     *
     * @param onode_id  ID of the onode (electron node)
     * @param onode     pw_node pointer of onode from registry bind
     */
    struct post_node_process_hook_args {
        const uint32_t onode_id;
        pw_node *onode;

        post_node_process_hook_args(const uint32_t onode_id, pw_node *onode) : onode_id(onode_id) {
            this->onode = onode;
        }

        ~post_node_process_hook_args() {
            if (this->onode) {
                pw_proxy_destroy((pw_proxy *)this->onode);
                this->onode = nullptr;
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
     * @param vstream                 pw_stream of vnode, used to read and copy volume data to `param_data`
     * @param onode                   pw_node of onode, used to set volume data from `param_data` and also copy mute
     * prop to `param_data`
     * @param ignore_next_onode_event Flag to prevent event callback loop when onode's volume data is modified
     * @param param_data              Contains accumulative and updated volume data with 4 fields: mute, volume,
     * channelmap and channelvolume.
     * @param listeners               Maintains `onode` and `vnode` 's `param_changed` event listeners for cleanup.
     */
    struct sync_params_data {
        pw_stream &vstream;
        pw_node *onode;
        bool ignore_next_onode_event;
        spa_pod *param_data;
        array<spa_hook *, 2> listeners;

        sync_params_data(pw_stream &vstream, pw_node *onode, const spa_audio_info_raw &audio_info) : vstream(vstream) {
            this->onode = onode;
            this->ignore_next_onode_event = true;
            this->listeners = {};
            this->listeners[0] = new spa_hook();
            this->listeners[1] = new spa_hook();

            spa_pod_dynamic_builder builder;
            spa_pod_dynamic_builder_init(&builder, nullptr, 0, 128);

            float volumes[audio_info.channels];
            for (uint32_t i = 0; i < audio_info.channels; i++)
                volumes[i] = 1.0f;

            spa_pod *temp = (spa_pod *)spa_pod_builder_add_object(
                &builder.b, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props, SPA_PROP_volume, SPA_POD_Float(1.0f),
                SPA_PROP_channelVolumes, SPA_POD_Array(sizeof(float), SPA_TYPE_Float, audio_info.channels, volumes),
                SPA_PROP_channelMap,
                SPA_POD_Array(sizeof(uint32_t), SPA_TYPE_Id, audio_info.channels, audio_info.position), SPA_PROP_mute,
                SPA_POD_Bool(false));

            this->param_data = spa_pod_copy(temp);
            spa_pod_dynamic_builder_clean(&builder);
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

            if (this->onode) {
                pw_proxy_destroy((pw_proxy *)this->onode);
                this->onode = nullptr;
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
         * Deletes existing `sync_params_data` instance in `onode_id` entry from `onode_to_sync_params_data` map (if
         * any), and creates a new `sync_params_data` instance.
         *
         * @param onode_id      ID of the onode (electron node)
         * @param vstream       pw_stream of the virtual stream replicating onode
         * @param onode         pw_node pointer of onode from registry bind
         * @param audio_info    raw audio info used to construct the appropriate param for the sync (uses its channels
         * and positions)
         *
         * @return reference to the `sync_params_data` entry for `onode_id` key.
         */
        static sync_params_data &start_new_sync_params_data(uint32_t onode_id, pw_stream &vstream, pw_node *onode,
                                                            const spa_audio_info_raw &audio_info);

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
     * Callback for `vnode` `param_changed` event. Copies the new SPA_PARAM_Props pod, containing volume data, into
     * `sync_param_data` and and syncs `onode` to the data. Also sets `ignore_next_onode_event` from `sync_param_data`
     * to skip the next `onode` 's `param` PipeWire event triggered when syncing `onode` volume data
     */
    static void on_vstream_param_props(void *data, uint32_t id, const struct spa_pod *param);

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
