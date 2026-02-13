#include "includes/nodes_manager.hpp"

#include "pipewire/context.h"
#include "pipewire/core.h"
#include "pipewire/keys.h"
#include "pipewire/link.h"
#include "pipewire/node.h"
#include "pipewire/port.h"
#include "pipewire/properties.h"
#include "pipewire/proxy.h"
#include "pipewire/stream.h"
#include "spa/param/param.h"
#include "spa/pod/builder.h"
#include "spa/utils/dict.h"
#include "spa/utils/hook.h"
#include <build.h>
#include <cstddef>
#include <cstdint>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/compare.h>
#include <string>
#include <sys/types.h>

using std::string;

// ===== PORT LINKS MANAGER =====

unordered_map<uint32_t, PortLinksManager::NodeManagerAccessor::port_infos *> PortLinksManager::node_id_to_port_infos =
    {};

const unordered_map<uint32_t, PortLinksManager::NodeManagerAccessor::port_infos *> &
PortLinksManager::NodeManagerAccessor::get_port_infos_map() {
    return node_id_to_port_infos;
}

PortLinksManager::NodeManagerAccessor::port_infos &PortLinksManager::get_modifiable_port_infos_entry(uint32_t node_id) {
    if (node_id_to_port_infos.find(node_id) == node_id_to_port_infos.end()) {
        node_id_to_port_infos[node_id] = new NodeManagerAccessor::port_infos();
        // TODO: log, follow set_vnode
    }
    return *node_id_to_port_infos[node_id];
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

void PortLinksManager::cleanup_link_infos_with_node_id(uint32_t node_id) {
    remove_entry_with_node_id(node_id, node_id_to_link_infos);
}

void PortLinksManager::cleanup_port_infos_with_node_id(uint32_t node_id) {
    remove_entry_with_node_id(node_id, node_id_to_port_infos);
}

unordered_map<uint32_t, PortLinksManager::link_infos *> PortLinksManager::node_id_to_link_infos = {};
vector<PortLinksManager::link_connect_task_data *> PortLinksManager::link_connect_tasks_list = {};
unordered_map<uint32_t, PortLinksManager::link_proxies_data *> PortLinksManager::node_id_to_create_link_proxies = {};

PortLinksManager::link_infos &PortLinksManager::get_modifiable_link_infos_entry(uint32_t node_id) {
    if (node_id_to_link_infos.find(node_id) == node_id_to_link_infos.end()) {
        node_id_to_link_infos[node_id] = new link_infos();
        // TODO: log, follow set_vnode
    }
    return *node_id_to_link_infos[node_id];
}

void PortLinksManager::disconnect_links_from_node(const uint32_t node_id, pw_registry *reg) {
    const link_infos &links = get_modifiable_link_infos_entry(node_id);

    for (auto &link : links.links_list) {
        pw_registry_destroy(reg, link->id);
    }

    remove_entry_with_node_id(node_id, node_id_to_link_infos);
}

void PortLinksManager::store_created_link_proxy_between_nodes(pw_proxy *link, const uint32_t node_id_one,
                                                              const uint32_t node_id_two) {
    // TODO: log
    auto &entry_one = get_modifiable_link_proxies(node_id_one);
    auto &entry_two = get_modifiable_link_proxies(node_id_two);

    entry_one.connected_node_id = node_id_two;
    entry_two.connected_node_id = node_id_one;

    auto link_ptr = new pw_proxy *(link);
    entry_one.link_proxies.push_back(link_ptr);
    entry_two.link_proxies.push_back(link_ptr);
}

void PortLinksManager::NodeManagerAccessor::enqueue_link_connection(const uint32_t playback_node_id,
                                                                    const uint32_t capture_node_id, pw_core &core,
                                                                    const uint32_t channels) {
    link_connect_tasks_list.push_back(new link_connect_task_data(playback_node_id, capture_node_id, core, channels));
}

void PortLinksManager::work_link_connect_task(pw_registry *reg) {
    const auto &port_infos_map = NodeManagerAccessor::get_port_infos_map();

    for (size_t i = 0; i < link_connect_tasks_list.size(); i++) {
        uint32_t link_connected = 0;

        link_connect_task_data *task = link_connect_tasks_list[i];
        const uint32_t playback_node_id = task->playback_node;
        const uint32_t capture_node_id = task->capture_node;

        if (port_infos_map.find(capture_node_id) == port_infos_map.end())
            continue;

        if (port_infos_map.find(playback_node_id) == port_infos_map.end())
            continue;

        // disconnect all links from the playback (electron node)
        disconnect_links_from_node(playback_node_id, reg);

        // since we are going to be adding links between these two, make sure we remove any current links between the
        // two
        remove_created_link_proxies_with_node_id(playback_node_id);
        remove_created_link_proxies_with_node_id(capture_node_id);

        const vector<NodeManagerAccessor::port_info *> &playback_ports =
            port_infos_map.at(playback_node_id)->ports_list;
        const vector<NodeManagerAccessor::port_info *> &capture_ports = port_infos_map.at(capture_node_id)->ports_list;

        for (const NodeManagerAccessor::port_info *playback_port : playback_ports) {
            if (playback_port->port_direction != "out")
                continue;

            for (const NodeManagerAccessor::port_info *capture_port : capture_ports) {
                if (capture_port->port_direction != "in")
                    continue;

                if (capture_port->audio_channel == playback_port->audio_channel) {

                    char output_port_id[32], input_port_id[32];
                    snprintf(output_port_id, sizeof(output_port_id), "%u", playback_port->id);
                    snprintf(input_port_id, sizeof(input_port_id), "%u", capture_port->id);

                    pw_properties *props = pw_properties_new(PW_KEY_LINK_OUTPUT_PORT, output_port_id,
                                                             PW_KEY_LINK_INPUT_PORT, input_port_id, nullptr);

                    void *link = pw_core_create_object(&task->core, "link-factory", PW_TYPE_INTERFACE_Link,
                                                       PW_VERSION_LINK, &props->dict, 0);

                    if (link != nullptr) {
                        store_created_link_proxy_between_nodes((pw_proxy *)link, playback_node_id, capture_node_id);
                        link_connected++;
                    }
                }
            }
        }

        if (link_connected >= task->channels) {
            delete task;
            task = nullptr;
            link_connect_tasks_list.erase(link_connect_tasks_list.begin() + i);
        }
    }
}

PortLinksManager::link_proxies_data &PortLinksManager::get_modifiable_link_proxies(const uint32_t node_id) {
    if (node_id_to_create_link_proxies.find(node_id) == node_id_to_create_link_proxies.end()) {
        node_id_to_create_link_proxies[node_id] = new link_proxies_data();
    }
    return *node_id_to_create_link_proxies[node_id];
}

void PortLinksManager::remove_created_link_proxies_with_node_id(const uint32_t node_id) {
    if (node_id_to_create_link_proxies.find(node_id) == node_id_to_create_link_proxies.end())
        return;

    const uint32_t connected_node_id = node_id_to_create_link_proxies.at(node_id)->connected_node_id;

    PortLinksManager::remove_entry_with_node_id(node_id, node_id_to_create_link_proxies);
    if (node_id_to_create_link_proxies.find(connected_node_id) != node_id_to_create_link_proxies.end()) {
        PortLinksManager::remove_entry_with_node_id(connected_node_id, node_id_to_create_link_proxies);
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
        NodeManagerAccessor::port_infos &ports = get_modifiable_port_infos_entry(atoi(node_id));
        ports.insert_port_info(id, port_direction ? string(port_direction) : "",
                               audio_channel ? string(audio_channel) : "", node_id ? string(node_id) : "");
    }

    work_link_connect_task(reg);
}

void PortLinksManager::enlist_registry_node_remove_event(const uint32_t id) {
    remove_created_link_proxies_with_node_id(id);
    cleanup_port_infos_with_node_id(id);
    cleanup_link_infos_with_node_id(id);
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

    for (const auto &[key, value] : node_id_to_port_infos)
        remove_entry_with_node_id(key, node_id_to_port_infos);

    for (const auto &[key, value] : node_id_to_create_link_proxies)
        remove_entry_with_node_id(key, node_id_to_create_link_proxies);

    for (const auto &[key, value] : node_id_to_link_infos)
        remove_entry_with_node_id(key, node_id_to_link_infos);
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

    if (state != PW_STREAM_STATE_PAUSED)
        return;

    auto *args = (state_change_enqueue_connect_capture_to_onode_args *)data;

    const uint32_t playback_node = args->onode_id;
    const uint32_t capture_node = pw_stream_get_node_id(&args->stream);

    PortLinksManager::NodeManagerAccessor::enqueue_link_connection(playback_node, capture_node, args->core,
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
        nullptr);

    pw_stream *virtual_stream = pw_stream_new(
        virtual_core,
        (args.override_desc.media_name != "" ? args.override_desc.media_name : args.onode.media_name).c_str(),
        stream_props);

    uint8_t buffer[1024];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const spa_pod *params[1];
    params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &args.onode.audio_info);

    output.vcontext = virtual_context;
    output.vcore = virtual_core;
    output.vstream = virtual_stream;

    if (args.onode.media_class.find("Output") != string::npos || args.onode.media_class.find("Sink") != string::npos)
        pw_stream_connect(virtual_stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                          (enum pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), params, 1);

    else if (args.onode.media_class.find("Input") != string::npos ||
             args.onode.media_class.find("Source") != string::npos)
        pw_stream_connect(virtual_stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                          (enum pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), params, 1);
    else
        pw_stream_connect(virtual_stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                          (enum pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), params, 1);
}

void NodesManager::connect_capture_to_onode(const create_node_args &onode_args, create_node_output &output) {
    pw_context *context = pw_context_new(&onode_args.loop, nullptr, 0);
    pw_core *core = pw_context_connect(context, nullptr, 0);

    char target[32];
    snprintf(target, sizeof(target), "%u", onode_args.onode.id);

    pw_properties *stream_props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        //
        PW_KEY_MEDIA_NAME,
        (onode_args.override_desc.media_name != "" ? onode_args.override_desc.media_name : onode_args.onode.media_name)
            .c_str(),
        nullptr);

    pw_stream *stream = pw_stream_new(core, PROJECT_NAME, stream_props);

    uint8_t buffer[1024];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const spa_pod *params[1];
    params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &onode_args.onode.audio_info);

    output.vstream = stream;
    output.vcontext = context;
    output.vcore = core;

    pw_stream_connect(stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                      (enum pw_stream_flags)(PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS), params, 1);

    auto *callback_args = new state_change_enqueue_connect_capture_to_onode_args(
        onode_args.onode.id, onode_args.onode.audio_info.channels, *core, *stream, new spa_hook());

    static const pw_stream_events stream_events = {
        .version = PW_VERSION_STREAM_EVENTS,
        .state_changed = NodesManager::on_state_change_enqueue_connect_capture_to_onode_single_callback,
    };

    pw_stream_add_listener(stream, callback_args->self_listener, &stream_events, callback_args);
}