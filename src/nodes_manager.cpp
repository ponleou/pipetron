#include "includes/nodes_manager.hpp"
#include <build.h>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <pipewire/context.h>
#include <pipewire/core.h>
#include <pipewire/keys.h>
#include <pipewire/link.h>
#include <pipewire/node.h>
#include <pipewire/port.h>
#include <pipewire/properties.h>
#include <pipewire/proxy.h>
#include <pipewire/stream.h>
#include <set>
#include <spa/param/audio/format-utils.h>
#include <spa/param/param.h>
#include <spa/pod/compare.h>
#include <spa/pod/builder.h>
#include <spa/utils/dict.h>
#include <spa/utils/hook.h>
#include <string>
#include <sys/types.h>

using std::cout;
using std::endl;
using std::make_shared;
using std::pair;
using std::set;
using std::string;
using std::to_string;

// ===== PORT LINKS MANAGER =====

unordered_map<uint32_t, PortLinksManager::port_infos *> PortLinksManager::node_id_to_port_infos = {};

void PortLinksManager::log(string msg) {
    cout << msg << endl;
}

template <typename T>
void PortLinksManager::remove_entry_with_node_id(uint32_t node_id, unordered_map<uint32_t, T *> &map) {
    auto it = map.find(node_id);

    if (it != map.end()) {
        delete it->second;
        it->second = nullptr;
        map.erase(it);
    }
}

template <typename T>
T &PortLinksManager::get_modifiable_entry(uint32_t key, unordered_map<uint32_t, T *> &map, function<T *()> factory,
                                          string log_new_entry) {
    if (map.find(key) == map.end()) {
        map[key] = factory();

        if (log_new_entry != "")
            log(log_new_entry);
    }

    return *map[key];
}

const unordered_map<uint32_t, PortLinksManager::port_infos *> &PortLinksManager::get_port_infos_map() {
    return node_id_to_port_infos;
}

PortLinksManager::port_infos &PortLinksManager::get_modifiable_port_infos_entry(uint32_t node_id) {

    return get_modifiable_entry(node_id, node_id_to_port_infos,
                                function<port_infos *()>([]() { return new port_infos(); }));
}

void PortLinksManager::cleanup_link_infos_with_node_id(uint32_t node_id) {
    remove_entry_with_node_id(node_id, node_id_to_link_infos);
}

void PortLinksManager::cleanup_port_infos_with_node_id(uint32_t node_id) {
    remove_entry_with_node_id(node_id, node_id_to_port_infos);
}

unordered_map<uint32_t, PortLinksManager::link_infos *> PortLinksManager::node_id_to_link_infos = {};
vector<PortLinksManager::link_connect_task_data *> PortLinksManager::link_connect_tasks_list = {};
unordered_map<uint32_t, PortLinksManager::created_link_proxies_data *>
    PortLinksManager::node_id_to_created_link_proxies = {};

PortLinksManager::link_infos &PortLinksManager::get_modifiable_link_infos_entry(uint32_t node_id) {
    return get_modifiable_entry(node_id, node_id_to_link_infos,
                                function<link_infos *()>([]() { return new link_infos(); }));
}

void PortLinksManager::disconnect_links_from_node_with_link_info(const uint32_t node_id, pw_registry *reg) {
    log("Removing all links connected to node ID " + to_string(node_id));

    const link_infos &links = get_modifiable_link_infos_entry(node_id);

    for (auto &link : links.links_list) {

        // this line for some reason is disconnecting clients from pulseaudio server
        // for pavucontrol-qt, the app hangs
        // for pavucontrol, it causes the app to reconnect to pulseaudio server
        // for pwvucontrol, it is perfectly fine

        pw_registry_destroy(reg, link->id);
    }

    cleanup_link_infos_with_node_id(node_id);
}

void PortLinksManager::store_created_link_proxy_between_nodes(pw_proxy *link, const uint32_t node_id_one,
                                                              const uint32_t node_id_two) {
    log("Created link between node ID " + to_string(node_id_one) + " and " + to_string(node_id_two));

    shared_ptr<pw_proxy *> shared_link = make_shared<pw_proxy *>(link);

    auto &entry_one = get_modifiable_link_proxies(node_id_one);
    entry_one.link_proxies.push_back(new created_link_proxy(shared_link, node_id_two));

    auto &entry_two = get_modifiable_link_proxies(node_id_two);
    entry_two.link_proxies.push_back(new created_link_proxy(shared_link, node_id_one));
}

void PortLinksManager::NodeManagerAccessor::enqueue_link_connection_task(const uint32_t playback_node_id,
                                                                         const uint32_t capture_node_id, pw_core &core,
                                                                         const uint32_t channels) {
    log("Enqueued link creation between node ID " + to_string(playback_node_id) + " (playback) and " +
        to_string(capture_node_id) + " (capture)");

    link_connect_tasks_list.push_back(new link_connect_task_data(playback_node_id, capture_node_id, core, channels));
}

void PortLinksManager::NodeManagerAccessor::copy_playback_link_direction(
    pw_stream &modify_node, const uint32_t modify_node_id, pw_core &modify_node_core,
    const uint32_t modify_node_channels, const uint32_t node_id_to_copy, pw_registry *reg) {
    disconnect_links_from_node_with_link_info(modify_node_id, reg);

    set<uint32_t> capture_node_ids = {};
    const link_infos &links = get_modifiable_link_infos_entry(node_id_to_copy);

    for (auto &link : links.links_list) {
        if (!link->is_output_direction)
            continue;

        capture_node_ids.insert(link->connected_to_node_id);
    }

    for (auto capture_node_id : capture_node_ids) {
        enqueue_link_connection_task(modify_node_id, capture_node_id, modify_node_core, modify_node_channels);
    }
}

void PortLinksManager::work_link_connect_task(pw_registry *reg) {
    const auto &port_infos_map = get_port_infos_map();

    for (size_t i = 0; i < link_connect_tasks_list.size(); i++) {
        link_connect_task_data *task = link_connect_tasks_list[i];
        const uint32_t playback_node_id = task->playback_node;
        const uint32_t capture_node_id = task->capture_node;

        if (port_infos_map.find(capture_node_id) == port_infos_map.end())
            continue;

        if (port_infos_map.find(playback_node_id) == port_infos_map.end())
            continue;

        const vector<port_info *> &playback_ports = port_infos_map.at(playback_node_id)->ports_list;
        const vector<port_info *> &capture_ports = port_infos_map.at(capture_node_id)->ports_list;

        vector<pair<uint32_t, uint32_t>> port_pairs = {}; // playback first, then capture id

        for (const port_info *playback_port : playback_ports) {
            if (playback_port->port_direction != "out")
                continue;

            for (const port_info *capture_port : capture_ports) {
                if (capture_port->port_direction != "in")
                    continue;

                if (capture_port->audio_channel == playback_port->audio_channel) {
                    port_pairs.push_back({playback_port->id, capture_port->id});
                }
            }
        }

        if (port_pairs.size() >= task->channels) {
            // disconnect all links from the playback (electron node)
            disconnect_links_from_node_with_link_info(playback_node_id, reg);

            for (auto pair : port_pairs) {
                string output_port_id = to_string(pair.first);
                string input_port_id = to_string(pair.second);

                pw_properties *props = pw_properties_new(PW_KEY_LINK_OUTPUT_PORT, output_port_id.c_str(),
                                                         PW_KEY_LINK_INPUT_PORT, input_port_id.c_str(), nullptr);

                void *link = pw_core_create_object(&task->core, "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK,
                                                   &props->dict, 0);

                if (link != nullptr) {
                    store_created_link_proxy_between_nodes((pw_proxy *)link, playback_node_id, capture_node_id);
                }

                pw_properties_free(props);
                props = nullptr;
            }

            delete task;
            task = nullptr;
            link_connect_tasks_list.erase(link_connect_tasks_list.begin() + i);
        }
    }
}

PortLinksManager::created_link_proxies_data &PortLinksManager::get_modifiable_link_proxies(const uint32_t node_id) {
    if (node_id_to_created_link_proxies.find(node_id) == node_id_to_created_link_proxies.end()) {
        node_id_to_created_link_proxies[node_id] = new created_link_proxies_data();
    }
    return *node_id_to_created_link_proxies[node_id];
}

void PortLinksManager::remove_created_link_proxies_with_node_id(const uint32_t node_id) {
    if (node_id_to_created_link_proxies.find(node_id) == node_id_to_created_link_proxies.end())
        return;

    // we have to search for entries with these keys and delete link_proxies that is connected with node_id
    set<uint32_t> connected_node_ids = {};

    const created_link_proxies_data &link_proxies_data = get_modifiable_link_proxies(node_id);
    for (auto &link_proxy : link_proxies_data.link_proxies) {
        connected_node_ids.insert(link_proxy->connected_node_id);
    }

    remove_entry_with_node_id(node_id, node_id_to_created_link_proxies);

    // removing specific `created_link_proxy` object that are connected to the node_id we are removing from other node
    // maps
    for (uint32_t id : connected_node_ids) {
        created_link_proxies_data &link_proxies_data = get_modifiable_link_proxies(id);

        for (size_t i = 0; i < link_proxies_data.link_proxies.size(); i++) {
            auto &link_proxy = link_proxies_data.link_proxies[i];

            if (link_proxy->connected_node_id != node_id)
                continue;

            delete link_proxy;
            link_proxies_data.link_proxies.erase(link_proxies_data.link_proxies.begin() + i);
            i--;
        }
    }
}

void PortLinksManager::enlist_registry_link_event(const uint32_t id, const struct spa_dict *props) {
    const char *input_node = spa_dict_lookup(props, PW_KEY_LINK_INPUT_NODE);
    const char *output_node = spa_dict_lookup(props, PW_KEY_LINK_OUTPUT_NODE);

    if (input_node && output_node) {
        link_infos &input_links = get_modifiable_link_infos_entry(atoi(input_node));
        link_infos &output_links = get_modifiable_link_infos_entry(atoi(output_node));

        input_links.insert_link_info(id, atoi(output_node), false);
        output_links.insert_link_info(id, atoi(input_node), true);
    }
}

void PortLinksManager::enlist_registry_port_event(const uint32_t id, const struct spa_dict *props, pw_registry *reg) {
    const char *node_id = spa_dict_lookup(props, PW_KEY_NODE_ID);
    const char *audio_channel = spa_dict_lookup(props, PW_KEY_AUDIO_CHANNEL);
    const char *port_direction = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION);

    if (node_id) {
        port_infos &ports = get_modifiable_port_infos_entry(atoi(node_id));
        ports.insert_port_info(id, port_direction ? string(port_direction) : "",
                               audio_channel ? string(audio_channel) : "", node_id ? string(node_id) : "");
    }

    work_link_connect_task(reg);
}

void PortLinksManager::enlist_registry_remove_event(const uint32_t id) {
    cleanup_port_infos_with_node_id(id);
    cleanup_link_infos_with_node_id(id);
    remove_created_link_proxies_with_node_id(id);
}

void PortLinksManager::cleanup() {
    for (size_t i = 0; i < link_connect_tasks_list.size(); i++) {
        auto *task = link_connect_tasks_list[i];
        if (!task)
            continue;

        delete task;
        task = nullptr;
    }
    link_connect_tasks_list.clear();

    for (const auto &[key, value] : node_id_to_link_infos)
        cleanup_link_infos_with_node_id(key);

    for (const auto &[key, value] : node_id_to_port_infos)
        cleanup_port_infos_with_node_id(key);

    for (const auto &[key, value] : node_id_to_created_link_proxies)
        remove_created_link_proxies_with_node_id(key);
}

// ===== END OF PORT LINKS MANAGER =====
// =====================================
// =========== NODE MANAGER ============

void NodesManager::maybe_run_post_process(nodes_manager_args_data *args) {

    if (!(args->process_args->info_flag && args->process_args->params_flag))
        return;

    spa_hook_remove(args->self_listener);
    delete args->self_listener;
    args->self_listener = nullptr;

    void *hook_args = args->node_processed_hook_args;
    NodesManager::create_node_args *vnode_args = args->vnode_args;
    args->node_processed_hook_args = nullptr;
    args->vnode_args = nullptr;
    args->post_node_process_hook(vnode_args, hook_args);

    delete args;
    args = nullptr;
}

void NodesManager::on_node_info_process_onode_info(void *data, const pw_node_info *info) {

    auto *process_data = (NodesManager::process_onode_info_args *)data;

    const char *app_name = spa_dict_lookup(info->props, PW_KEY_APP_NAME);
    const char *app_icon_name = spa_dict_lookup(info->props, PW_KEY_APP_ICON_NAME);
    const char *app_process_binary = spa_dict_lookup(info->props, PW_KEY_APP_PROCESS_BINARY);
    const char *media_class = spa_dict_lookup(info->props, PW_KEY_MEDIA_CLASS);
    const char *media_name = spa_dict_lookup(info->props, PW_KEY_MEDIA_NAME);
    const char *node_desc = spa_dict_lookup(info->props, PW_KEY_NODE_DESCRIPTION);

    process_data->onode.app_name = app_name ? string(app_name) : "";
    process_data->onode.app_icon_name = app_icon_name ? string(app_icon_name) : "";
    process_data->onode.app_process_binary = app_process_binary ? string(app_process_binary) : "";
    process_data->onode.media_class = media_class ? string(media_class) : "";
    process_data->onode.media_name = media_name ? string(media_name) : "";
    process_data->onode.node_description = node_desc ? string(node_desc) : "";

    process_data->info_flag = true;
}

void NodesManager::on_node_param_process_onode_info(void *data, int seq, uint32_t id, uint32_t index, uint32_t next,
                                                    const spa_pod *param) {

    auto *process_data = (NodesManager::process_onode_info_args *)data;

    if (id != SPA_PARAM_Format)
        return;

    auto *onode_info = &process_data->onode;
    spa_format_audio_raw_parse(param, &onode_info->audio_info);
    process_data->params_flag = true;
}

void NodesManager::on_node_info_process_callback(void *data, const pw_node_info *info) {
    nodes_manager_args_data *args = (nodes_manager_args_data *)data;

    if (args->process_args->info_flag)
        return;

    NodesManager::on_node_info_process_onode_info((void *)args->process_args, info);
    NodesManager::maybe_run_post_process(args);
}

void NodesManager::on_node_param_process_callback(void *data, int seq, uint32_t id, uint32_t index, uint32_t next,
                                                  const spa_pod *param) {

    nodes_manager_args_data *args = (nodes_manager_args_data *)data;

    if (args->process_args->params_flag)
        return;

    NodesManager::on_node_param_process_onode_info((void *)args->process_args, seq, id, index, next, param);
    NodesManager::maybe_run_post_process(args);
}

// all this does is get ID (after the stream is paused/ready) and enqueue a link connection
void NodesManager::on_state_change_enqueue_connect_capture_to_onode_single_callback(void *data,
                                                                                    enum pw_stream_state old,
                                                                                    enum pw_stream_state state,
                                                                                    const char *error) {

    auto *args = (state_change_enqueue_connect_capture_to_onode_args *)data;

    if (state == PW_STREAM_STATE_UNCONNECTED || state == PW_STREAM_STATE_ERROR) {
        delete args;
        args = nullptr;
        return;
    }

    if (state != PW_STREAM_STATE_PAUSED)
        return;

    const uint32_t playback_node = args->onode_id;
    const uint32_t capture_node = pw_stream_get_node_id(&args->stream);

    PortLinksManager::NodeManagerAccessor::enqueue_link_connection_task(playback_node, capture_node, args->core,
                                                                        args->channels);

    spa_hook_remove(args->self_listener);
    delete args->self_listener;
    args->self_listener = nullptr;

    delete args;
    args = nullptr;
}

void NodesManager::process_new_node(pw_node *node, nodes_manager_args_data *args) {

    static const pw_node_events node_events = {
        .version = PW_VERSION_NODE_EVENTS,
        .info = NodesManager::on_node_info_process_callback,
        .param = NodesManager::on_node_param_process_callback,
    };

    uint32_t param_ids_sub[] = {SPA_PARAM_Format};
    pw_node_subscribe_params(node, param_ids_sub, sizeof(param_ids_sub) / sizeof(param_ids_sub[0]));

    pw_proxy_add_object_listener((pw_proxy *)node, args->self_listener, &node_events, args);

    pw_node_enum_params(node, 0, SPA_PARAM_Format, 0, UINT32_MAX, nullptr);
}

void NodesManager::replicate_vnode(const NodesManager::create_node_args &args,
                                   NodesManager::create_node_output &output) {

    pw_properties *context_props = pw_properties_new(PW_KEY_APP_NAME, args.onode.app_process_binary.c_str(), nullptr);
    pw_context *virtual_context = pw_context_new(&args.loop, context_props, 0);
    pw_core *virtual_core = pw_context_connect(virtual_context, nullptr, 0);

    pw_properties *stream_props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        //
        PW_KEY_APP_NAME,
        (args.override_desc.app_name != "" ? args.override_desc.app_name : args.onode.app_name).c_str(),
        //
        PW_KEY_MEDIA_CLASS,
        (args.override_desc.media_class != "" ? args.override_desc.media_class : args.onode.media_class).c_str(),
        //
        PW_KEY_APP_ICON_NAME,
        (args.override_desc.app_icon_name != "" ? args.override_desc.app_icon_name : args.onode.app_icon_name).c_str(),
        //
        PW_KEY_APP_PROCESS_BINARY,
        (args.override_desc.app_process_binary != "" ? args.override_desc.app_process_binary
                                                     : args.onode.app_process_binary)
            .c_str(),
        //
        PW_KEY_NODE_DESCRIPTION,
        (args.override_desc.node_description != "" ? args.override_desc.node_description : args.onode.node_description)
            .c_str(),
        //
        nullptr);

    if (args.node_group != "") {
        pw_properties_set(stream_props, PW_KEY_NODE_GROUP, args.node_group.c_str());
    }

    pw_stream *virtual_stream = pw_stream_new(
        virtual_core,
        (args.override_desc.media_name != "" ? args.override_desc.media_name : args.onode.media_name).c_str(),
        stream_props);

    // this pod_builder cant be dynamic like in volume/audio manager in syncing params
    // i dont know why
    uint8_t buffer[1024];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

    const spa_pod *params[1];
    params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &args.onode.audio_info);

output.context = virtual_context;
    output.core = virtual_core;
    output.stream = virtual_stream;

    if (args.onode.media_class.find("Output") != string::npos || args.onode.media_class.find("Sink") != string::npos)
        pw_stream_connect(virtual_stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                          (enum pw_stream_flags)(PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS), params, 1);

    else if (args.onode.media_class.find("Input") != string::npos ||
             args.onode.media_class.find("Source") != string::npos)
        pw_stream_connect(virtual_stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                          (enum pw_stream_flags)(PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS), params, 1);
    else
        pw_stream_connect(virtual_stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                          (enum pw_stream_flags)(PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS), params, 1);
}

void NodesManager::connect_capture_to_onode(const create_node_args &onode_args, create_node_output &output) {
    pw_context *context = pw_context_new(&onode_args.loop, nullptr, 0);
    pw_core *core = pw_context_connect(context, nullptr, 0);

    pw_properties *stream_props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio",
                                                    //
                                                    nullptr);

    if (onode_args.node_group != "") {
        pw_properties_set(stream_props, PW_KEY_NODE_GROUP, onode_args.node_group.c_str());
    }

    pw_stream *stream = pw_stream_new(
        core,
        (onode_args.override_desc.media_name != "" ? onode_args.override_desc.media_name : onode_args.onode.media_name)
            .c_str(),
        stream_props);

    // this pod_builder cant be dynamic like in volume/audio manager in syncing params
    // i dont know why
    uint8_t buffer[1024];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

    const spa_pod *params[1];
    params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &onode_args.onode.audio_info);


    output.stream = stream;
    output.context = context;
    output.core = core;

    pw_stream_connect(stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                      (enum pw_stream_flags)(PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS), params, 1);

    auto *callback_args = new state_change_enqueue_connect_capture_to_onode_args(
        onode_args.onode.id, onode_args.onode.audio_info.channels, *core, *stream, new spa_hook());

    static const pw_stream_events stream_events = {
        .version = PW_VERSION_STREAM_EVENTS,
        .state_changed = on_state_change_enqueue_connect_capture_to_onode_single_callback,
    };

    pw_stream_add_listener(stream, callback_args->self_listener, &stream_events, callback_args);
}

void NodesManager::copy_playback_link_direction(pw_stream &modify_node, const uint32_t modify_node_id,
                                                pw_core &modify_node_core, const uint32_t modify_node_channels,
                                                const uint32_t node_id_to_copy, pw_registry *reg) {
    PortLinksManager::NodeManagerAccessor::copy_playback_link_direction(modify_node, modify_node_id, modify_node_core,
                                                                        modify_node_channels, node_id_to_copy, reg);
}