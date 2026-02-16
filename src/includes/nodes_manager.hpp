#pragma once

#include "pipewire/core.h"
#include "pipewire/node.h"
#include "pipewire/stream.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <spa/param/audio/format-utils.h>
#include <string>
#include <unordered_map>
#include <vector>

using std::function;
using std::shared_ptr;
using std::string;
using std::unordered_map;
using std::vector;

/**
 * Manages methods and stores related to ports and links. Stores includes:
 *  - node_id_to_link_infos:            map with `node_id` key to `link_infos`
 *  - node_id_to_port_infos:            map with `node_id` key to `port_infos`
 *  - node_id_to_created_link_proxies:  map with `node_id` key to the link proxy pointers created by PortLinkManager.
 *                                      Two `node_id` entries, which the links connect with, will contain the same link
 *                                      proxies pointers.
 *  - link_connect_tasks_list:          list of task information which are enqueued for PortLinkManager to create links
 *                                      between two specified nodes when ready
 */
class PortLinksManager {
  private:
    static void log(string msg);

    /**
     * Removes and deletes the entry for `node_id` from the given map, if present.
     *
     * @param node_id  The `onode_id` entry to delete from the map
     * @param map       The map to remove the entry from
     */
    template <typename T>
    static void remove_entry_with_node_id(uint32_t node_id, unordered_map<uint32_t, T *> &map);

    template <typename T>
    static T &get_modifiable_entry(uint32_t key, unordered_map<uint32_t, T *> &map, function<T *()> factory,
                                   string log_new_entry = "");

    /**
     * Link information collected from global registry event on PW_TYPE_INTERFACE_Link are stored in this struct. Meant
     * to be stored in a map to the key of one node ID, while the other node ID is stored in `connected_to_node_id`.
     *
     * @param id                    ID of the link
     * @param connected_to_node_id  the node ID that the link connects to with the entry's key node ID
     * @param is_output_direction   the direction of the entry's key node ID, false if its an input node, true if its an
     * output node
     */
    struct link_info {
        const uint32_t id;
        const uint32_t connected_to_node_id;
        const bool is_output_direction;

        link_info(const uint32_t id, const uint32_t connected_to_node_id, const bool is_output_direction)
            : id(id), connected_to_node_id(connected_to_node_id), is_output_direction(is_output_direction) {
        }
    };

    /**
     * Used inside the map `node_id_to_link_infos` to store all links connected to the entry's key node ID as
     * `link_info`
     */
    struct link_infos {
        vector<link_info *> links_list;

        link_infos() {
            this->links_list = {};
        }

        void insert_link_info(const uint32_t id, const uint32_t connected_to_node_id, const bool is_output_direction) {
            this->links_list.push_back(new link_info(id, connected_to_node_id, is_output_direction));
        }

        ~link_infos() {
            for (link_info *link : this->links_list) {
                delete link;
                link = nullptr;
            }
            this->links_list.clear();
        }
    };

    /**
     * Struct to contain one instance of a created link proxy between two nodes. Meant to be mapped to the ID of one
     * node, while the other node ID is stored in `connected_node_id`. These instances reference the link created
     * manually by the program.
     *
     * The link_proxy_ptr is a pointer in a `shared_ptr`, allowing reference by multiple node. This is used to allow
     * multiple `created_link_proxy` instances to reference the same `link_proxy_ptr` because a link connects between
     * two nodes. Hence, two entries in the map may reference the same proxy pointer.
     *
     * @param link_proxy_ptr    a shared pointer containing the pw_proxy pointer for the link
     * @param connected_node_id contains the node ID that the link proxies is connecting with the entry's key node ID
     */
    struct created_link_proxy {
        shared_ptr<pw_proxy *> link_proxy_ptr;
        uint32_t connected_node_id;

        created_link_proxy(shared_ptr<pw_proxy *> link_proxy_ptr, uint32_t connected_node_id) {
            this->link_proxy_ptr = link_proxy_ptr;
            this->connected_node_id = connected_node_id;
        }

        ~created_link_proxy() {
            if (*this->link_proxy_ptr) {
                pw_proxy_destroy(*this->link_proxy_ptr);
                *this->link_proxy_ptr = nullptr;
            }
        }
    };

    /**
     * Struct used to store multiple `created_link_proxy` instances in a single vector. Meant to be mapped to the ID of
     * one node, allowing the node to contain multiple `created_link_proxy` instances.
     */
    struct created_link_proxies_data {
        vector<created_link_proxy *> link_proxies;

        ~created_link_proxies_data() {
            for (auto *link_proxy : this->link_proxies) {
                if (link_proxy) {
                    delete link_proxy;
                    link_proxy = nullptr;
                }
            }
            this->link_proxies.clear();
        }
    };

    /**
     * Information required for a task definition to create links. On recieving task with this struct, a link will be
     * created between the `playback_node` and `capture_node`. Tasks are stored in `link_connect_tasks_list`
     *
     * @param playback_node the ID of the playback node to create link from
     * @param capture_node  the ID of the capture node to create link to
     * @param core          pw_core of either node, required to create link proxies
     * @param channels      number of channels for each node (should be the same for both playback and capture node),
     * aka the number of expected links (one for each channel)
     */
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

    /**
     * Port information collected from global registry event on PW_TYPE_INTERFACE_Port are stored in this struct.
     *
     * @param id                ID of the port
     * @param port_direction    direction of the port, input or output port
     * @param audio_channel     the channel of the port (e.g. FL, FR, etc.)
     * @param node_id           the node ID that the port belongs to
     */
    struct port_info {
        const uint32_t id;
        string port_direction;
        string audio_channel;
        string node_id;

        port_info(const uint32_t id, string port_direction, string audio_channel, string node_id) : id(id) {
            this->audio_channel = audio_channel;
            this->port_direction = port_direction;
            this->node_id = node_id;
        }
    };

    /**
     * Used inside the map `node_id_to_port_infos` to store all ports belonging to the node_id key
     */
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

    static unordered_map<uint32_t, link_infos *> node_id_to_link_infos;
    static unordered_map<uint32_t, created_link_proxies_data *> node_id_to_created_link_proxies;
    static vector<link_connect_task_data *> link_connect_tasks_list;
    static unordered_map<uint32_t, port_infos *> node_id_to_port_infos;

    /**
     * Provides the reference to the `node_id_to_link_infos` map entry for `node_id` key. If the entry does
     * not exist, the entry will be created lazily.
     *
     * @return reference to the `link_infos` entry for `node_id` key.
     */
    static link_infos &get_modifiable_link_infos_entry(uint32_t node_id);

    /**
     * Remove the `node_id_to_link_infos` map's entry with `node_id` key
     */
    static void cleanup_link_infos_with_node_id(uint32_t node_id);

    /**
     * Disconnect all links connected to `node_id` using stored link info, and also cleans up the `link_infos` entry for
     * `node_id` by calling `cleanup_link_infos_with_node_id`
     */
    static void disconnect_links_from_node_with_link_info(const uint32_t node_id, pw_registry *reg);

    /**
     * Provides the reference to the `node_id_to_created_link_proxies` map entry for `node_id` key. If the entry does
     * not exist, the entry will be created lazily.
     *
     * @return reference to the `created_link_proxies_data` entry for `node_id` key.
     */
    static created_link_proxies_data &get_modifiable_link_proxies(const uint32_t node_id);

    /**
     * Creates two entries for `node_id_to_created_link_proxies` for keys of each node_id_one and node_id_two, where its
     * `connected_node_id` value is its other pair node_id, and both entries contain the same pointer to the links
     * connected between the two nodes.
     */
    static void store_created_link_proxy_between_nodes(pw_proxy *link, const uint32_t node_id_one,
                                                       const uint32_t node_id_two);

    /**
     * Deletes the created link between the pair nodes connected to `node_id` and also cleans up the data in
     * `node_id_to_created_link_proxies` for the `node_id` entry and its pair connection entry.
     */
    static void remove_created_link_proxies_with_node_id(const uint32_t node_id);

    /**
     * Function called on global registry event for PW_TYPE_INTERFACE_Port which does work enqueued in
     * `link_connect_tasks_list`. The work done includes removing all links from the task's playback node, and creating
     * links to all matching channels between the playback and capture nodes specified in `link_connect_task_data`.
     *
     * WARNING: this function does not handle checks to automatically remove tasks from list if the enqueued node pairs
     * can never be linked. The task will forever remain in the task list until the link is completed successfully. If
     * the link cannot be created, the function will continue to fail on every retry.
     *
     * @param reg used to disconnect all links from the playback node by calling
     * `disconnect_links_from_node_with_link_info`
     */
    static void work_link_connect_task(pw_registry *reg);

    /**
     * Provides the reference to the `node_id_to_port_infos` map entry for `node_id` key. If the entry does
     * not exist, the entry will be created lazily.
     *
     * @return reference to the `port_infos` entry for `node_id` key.
     */
    static port_infos &get_modifiable_port_infos_entry(uint32_t node_id);

    /**
     * Getter for the `node_id_to_port_infos` map. Read-only.
     */
    static const unordered_map<uint32_t, port_infos *> &get_port_infos_map();

    /**
     * Remove the `node_id_to_port_infos` map's entry with `node_id` key
     */
    static void cleanup_port_infos_with_node_id(uint32_t node_id);

  public:
    class NodeManagerAccessor {
      private:
        friend class PortLinksManager;
        friend class NodesManager;

        /**
         * Create `link_connect_task_data` to `link_connect_tasks_list`. The task will remove all links from playback
         * node, and create links between the playback and capture node on `work_link_connect_task` call.
         *
         * WARNING: enqueuing a link connection task assumes that the two nodes CAN and WILL succeed whenever possible.
         * If task is enqueued with two nodes that are not compatible (non-matching channels or direction), the task
         * will forever remain in the task list, and will fail on every retry. This can siginifcantly slow down program.
         *
         * @param playback_node_id node ID of the playback node
         * @param capture_node_id node ID of the capture node
         * @param core pw_core reference of either playback or capture node, required to create the link
         * @param channels the number of channels of either playback or capture node, which should be the same (aka, the
         * number of links that will be created between the two nodes)
         */
        static void enqueue_link_connection_task(const uint32_t playback_node_id, const uint32_t capture_node_id,
                                                 pw_core &core, const uint32_t channels);

        /**
         * This function will ensure the modifying node will have links connected to the same nodes as the node to be
         * copied. The function will disconnect all current links in the modifying node, and create links to all nodes
         * that the node to copy is connected to.
         *
         * WARNING: This function only works for playback nodes, that is, the modifying node and the node to be copied
         * must be playback. This function also calls `enqueue_link_connection_task`, meaning it requires that the node
         * to be modified can be connected to the same nodes that the node to be copied are connected to.
         *
         * @param modify_node           pw_stream of the playback node to have its links modified
         * @param modify_node_id        node ID of `modify_node`
         * @param modify_node_core      pw_core of `modify_node`
         * @param modify_node_channels  the number of channels that `modify_node` playback has (aka, number of links
         * created). This should be the same as the node to be copied.
         * @param node_id_to_copy       the node ID of the node that modifying node will follow for its link connections
         * @param reg                   pw_registry pointer, required to disconnect all links from `modify_node`.
         */
        static void copy_playback_link_direction(pw_stream &modify_node, const uint32_t modify_node_id,
                                                 pw_core &modify_node_core, const uint32_t modify_node_channels,
                                                 const uint32_t node_id_to_copy, pw_registry *reg);
    };

    /**
     * Called on global registry event when the type is a `PW_TYPE_INTERFACE_Link` which stores `link_infos` to
     * `node_id_to_link_infos`. Stores two entries for one link node, each for the two connected `node_id` s between the
     * link.
     *
     * @param id    ID of the link
     * @param props props of the link
     */
    static void enlist_registry_link_event(const uint32_t id, const struct spa_dict *props);

    /**
     * Called on global registry event when the type is a `PW_TYPE_INTERFACE_Port` which stores `port_infos` to
     * `node_id_to_link_infos`. Stores an entry mapped to the `node_id` that the port belongs to, and also calls
     * `work_link_connect_task` to complete enqueued link creation tasks.
     *
     * @param id    ID of the link
     * @param props props of the link
     * @param reg   pw_registry pointer required by `work_link_connect_task` to delete links
     */
    static void enlist_registry_port_event(const uint32_t id, const struct spa_dict *props, pw_registry *reg);

    /**
     * Called on registry global remove event. Deletes link proxies created by PortLinkManager, `link_infos`, and
     * `port_infos` from `node_id_to_*` maps in PortLinkManager for the `id` keys.
     *
     * @param id  ID of the removed pipewire item
     */
    static void enlist_registry_remove_event(const uint32_t id);

    /**
     * Called on process kill, which cleans up `node_id_to_link_infos`, `node_id_to_created_link_proxies`,
     * `link_connect_tasks_list`, and `node_id_to_port_infos`.
     */
    static void cleanup();
};

/**
 * Provides common node managament functions for other managers.
 */
class NodesManager {
  private:
    /**
     * Struct used to pass through to the created capture's state change callback, which gets the capture stream's ID
     * when ready, start the process of disconnecting links to the playback onode, and connect a created capture to the
     * playback.
     *
     * @param onode_id      the node_id of the (electron) playback node
     * @param channels      number of channels of the playback node (and the capture node which uses the same setting)
     * @param core          pw_core used to create links
     * @param stream        pw_stream of the created capture node
     * @param self_listener listener for the state_change single callback
     */
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
     * @param app_name              The name of the node.
     * @param app_icon_name         The name of the icon used by the node
     * @param app_process_binary    The binary name of the application owning this node.
     * @param node_description      The node description.
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
     * Store of information for a PipeWire node. Inherits `node_desc`.
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
     * Generic input arguments for methods in NodeManager that creates nodes in replication to the `onode` information,
     * such as `replicate_vnode` and `connect_capture_to_onode`.
     *
     * @param loop          The PipeWire main loop the virtual node's context will run on.
     * @param onode         The original node's metadata used to replicate its properties.
     * @param override_desc Optional node descriptions that will override `node_info` 's metadata when replicating
     * properties.
     */
    struct create_node_args {
        pw_loop &loop;
        const node_info &onode;
        node_desc override_desc;
        string node_group;

        create_node_args(pw_loop &loop, const NodesManager::node_info &onode) : loop(loop), onode(onode) {
            this->node_group = "";
        }
    };

    /**
     * Generic output of for methods in NodeManager that creates nodes, such as `replicate_vnode` and
     * `connect_capture_to_onode`. Contains the PipeWire objects that make up the newly created node.
     *
     * @param stream   The pw_stream of the virtual node.
     * @param context  The pw_context of the virtual node.
     * @param core     The pw_core connection of the virtual node.
     */
    struct create_node_output {
        pw_stream *stream;
        pw_context *context;
        pw_core *core;
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

    /**
     * State_change single callback for the created capture stream called from `connect_capture_to_onode`. Collects the
     * created capture stream's ID when ready, and enqueues a link creation task between the created capture stream and
     * the `onode_id` passed from the data as `state_change_enqueue_connect_capture_to_onode_args`.
     */
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

    /**
     * Creates a capture node to connect with the node referenced in `onode_args`. Replicates onode information from
     * `onode_args` and calls `on_state_change_enqueue_connect_capture_to_onode_single_callback` to enqueue process to
     * connect links to the capture node from the onode.
     *
     * @param args      Input args provided inside `post_node_process_hook`, reference to the playback onode to connect
     * a capture to.
     * @param output    Output struct populated with the created stream, context, and core for the capture node.
     */
    static void connect_capture_to_onode(const create_node_args &onode_args, create_node_output &output);

    /**
     * Interface in NodeManagers that will call PortLinkManager's `copy_playback_link_direction`.
     */
    static void copy_playback_link_direction(pw_stream &modify_node, const uint32_t modify_node_id,
                                             pw_core &modify_node_core, const uint32_t modify_node_channels,
                                             const uint32_t node_id_to_copy, pw_registry *reg);
};
