#include "includes/volume_manager.hpp"
#include "includes/nodes_manager.hpp"
#include <build.h>
#include <cstdint>
#include <iostream>
#include <ostream>
#include <pipewire/core.h>
#include <pipewire/node.h>
#include <pipewire/proxy.h>
#include <pipewire/stream.h>
#include <spa/debug/pod.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/param.h>
#include <spa/param/props.h>
#include <spa/pod/compare.h>
#include <string>
#include <unordered_map>

using std::cout;
using std::endl;
using std::string;
using std::to_string;
using std::unordered_map;

unordered_map<uint32_t, NodesManager::node_info *> VolumeStores::onode_infos = {};
unordered_map<uint32_t, VolumeStores::vnode_data *> VolumeStores::onode_to_vnode_data = {};
unordered_map<uint32_t, VolumeStores::sync_params_data *> VolumeStores::onode_to_sync_params_data = {};

template <typename T>
void VolumeStores::remove_entry_with_onode(uint32_t onode_id, unordered_map<uint32_t, T *> &map) {
    auto it = map.find(onode_id);

    if (it != map.end()) {
        delete map.at(it->first);
        it->second = nullptr;
        map.erase(it);
    }
}

void VolumeStores::log(string msg) {
    cout << msg << endl;
}

bool VolumeStores::get_onode_binary_name(uint32_t onode_id, string &name) {
    auto it = onode_infos.find(onode_id);

    if (it == onode_infos.end())
        return false;

    name = it->second->app_process_binary;
    return true;
}

VolumeStores::vnode_data &VolumeStores::FriendAccessor::get_modifiable_vnode_data(uint32_t onode_id) {

    if (onode_to_vnode_data.find(onode_id) == onode_to_vnode_data.end()) {
        onode_to_vnode_data[onode_id] = new vnode_data(0, nullptr, nullptr, nullptr);

        string onode_binary_name = "";
        bool got_name = get_onode_binary_name(onode_id, onode_binary_name);
        log("Created new virtual node for volume mirror with node ID " + to_string(onode_id) +
            (got_name ? " (" + onode_binary_name + ")" : ""));
    }
    return *onode_to_vnode_data[onode_id];
}

NodesManager::node_info &VolumeStores::FriendAccessor::get_modifiable_onode_info(uint32_t onode_id) {

    if (onode_infos.find(onode_id) == onode_infos.end()) {
        onode_infos[onode_id] = new NodesManager::node_info(onode_id);
        log("New electron node ID " + to_string(onode_id) + " detected");
    }

    return *onode_infos[onode_id];
}

VolumeStores::sync_params_data &
VolumeStores::FriendAccessor::start_new_sync_params_data(uint32_t onode_id, pw_stream &vstream, pw_node *onode,
                                                         const spa_audio_info_raw &audio_info) {
    if (onode_to_sync_params_data.find(onode_id) != onode_to_sync_params_data.end()) {
        remove_entry_with_onode(onode_id, onode_to_sync_params_data);
    }

    onode_to_sync_params_data[onode_id] = new sync_params_data(vstream, onode, audio_info);

    return *onode_to_sync_params_data[onode_id];
}

void VolumeStores::FriendAccessor::cleanup_entries_with_onode_id(uint32_t onode_id) {

    string onode_name = "";
    if (get_onode_binary_name(onode_id, onode_name)) {
        log("Cleaning up node ID " + to_string(onode_id) + " (" + onode_name + ")");
    }

    remove_entry_with_onode<NodesManager::node_info>(onode_id, onode_infos);
    remove_entry_with_onode<sync_params_data>(onode_id, onode_to_sync_params_data);
    remove_entry_with_onode<vnode_data>(onode_id, onode_to_vnode_data);
}

void VolumeStores::FriendAccessor::cleanup() {
    for (const auto &[key, value] : onode_infos)
        remove_entry_with_onode<NodesManager::node_info>(key, onode_infos);

    for (const auto &[key, value] : onode_to_sync_params_data)
        remove_entry_with_onode<sync_params_data>(key, onode_to_sync_params_data);

    for (const auto &[key, value] : onode_to_vnode_data)
        remove_entry_with_onode<vnode_data>(key, onode_to_vnode_data);
}

void VolumeManager::on_vstream_param_props(void *data, uint32_t id, const struct spa_pod *param) {

    auto *sync_data = (VolumeStores::sync_params_data *)data;

    if (id != SPA_PARAM_Props)
        return;

    // find if the field that is updated from param_changed's param is inside the param_data, if it is, copy the param's
    // value into the param_data
    spa_pod_prop *new_param;
    SPA_POD_OBJECT_FOREACH((spa_pod_object *)param, new_param) {

        // i have no idea why, but recording vstreams channelmap param are sometimes broken
        // it is fine for playback vstreams however
        // this SHOULD be fine (i hope) as channelmaps are mostly constant
        if (new_param->key == SPA_PROP_channelMap)
            continue;

        spa_pod_prop *param_data =
            (spa_pod_prop *)spa_pod_object_find_prop((spa_pod_object *)sync_data->param_data, nullptr, new_param->key);
        if (param_data)
            memcpy(&param_data->value, &new_param->value, SPA_POD_SIZE(&new_param->value));
    }

    sync_data->ignore_next_onode_event = true;
    pw_node_set_param(sync_data->onode, SPA_PARAM_Props, 0, sync_data->param_data);
}

void VolumeManager::on_onode_param_props(void *data, int seq, uint32_t id, uint32_t index, uint32_t next,
                                         const struct spa_pod *param) {

    if (id != SPA_PARAM_Props)
        return;

    auto *sync_data = (VolumeStores::sync_params_data *)data;

    if (sync_data->ignore_next_onode_event) {
        sync_data->ignore_next_onode_event = false;
        return;
    }

    // two-way sync for mute value
    bool mute;
    spa_pod_parse_object(param, SPA_TYPE_OBJECT_Props, nullptr, SPA_PROP_mute, SPA_POD_Bool(&mute));
    spa_pod_prop *prop;
    SPA_POD_OBJECT_FOREACH((spa_pod_object *)sync_data->param_data, prop) {
        if (prop->key == SPA_PROP_mute)
            SPA_POD_VALUE(spa_pod_bool, &prop->value) = mute;
    }

    sync_data->ignore_next_onode_event = true;
    pw_node_set_param(sync_data->onode, SPA_PARAM_Props, 0, sync_data->param_data);
    pw_stream_set_param(&sync_data->vstream, SPA_PARAM_Props, sync_data->param_data);
}

void *VolumeManager::post_node_process_hook(NodesManager::create_node_args *vnode_args, void *data) {

    auto *args = (VolumeManagerArgs::post_node_process_hook_args *)data;

    NodesManager::create_node_output output;
    vnode_args->override_desc.app_name = vnode_args->onode.app_process_binary;
    vnode_args->override_desc.app_icon_name = vnode_args->onode.app_process_binary;
    vnode_args->override_desc.media_name = string(PROJECT_NAME) + " volume controller";
    NodesManager::replicate_vnode(*vnode_args, output);

    VolumeStores::vnode_data &vnode_data = VolumeStores::FriendAccessor::get_modifiable_vnode_data(args->onode_id);
    vnode_data.context = output.context;
    vnode_data.core = output.core;
    vnode_data.stream = output.stream;

    output.context = nullptr;
    output.core = nullptr;
    output.stream = nullptr;
    vnode_data.id = 0; // default value, will be set by VolumeManager::on_state_change_single_callback

    pw_node *onode = args->onode;
    args->onode = nullptr;

    auto &sync_data = VolumeStores::FriendAccessor::start_new_sync_params_data(args->onode_id, *vnode_data.stream,
                                                                               onode, vnode_args->onode.audio_info);

    delete vnode_args;
    vnode_args = nullptr;

    // starting sync callback for onode
    uint32_t param_ids_sub[] = {SPA_PARAM_Props};

    pw_node_subscribe_params(sync_data.onode, param_ids_sub, sizeof(param_ids_sub) / sizeof(param_ids_sub[0]));

    static const pw_node_events onode_events = {
        .version = PW_VERSION_NODE_EVENTS,
        .param = VolumeManager::on_onode_param_props,
    };

    pw_proxy_add_object_listener((pw_proxy *)sync_data.onode, sync_data.listeners[0], &onode_events,
                                 (void *)&sync_data);

    // starting sync callback for vstream
    static const pw_stream_events vstream_events = {
        .version = PW_VERSION_STREAM_EVENTS,
        .param_changed = VolumeManager::on_vstream_param_props,
    };

    pw_stream_add_listener(&sync_data.vstream, sync_data.listeners[1], &vstream_events, (void *)&sync_data);

    pw_stream_update_params(&sync_data.vstream, nullptr, 0);

    delete args;
    args = nullptr;

    return nullptr;
}

void VolumeManager::process_new_node(pw_registry *reg, pw_loop *loop, uint32_t id, const char *type) {
    pw_node *onode = (pw_node *)pw_registry_bind(reg, id, type, PW_VERSION_NODE, 0);
    auto *hook_args = new VolumeManagerArgs::post_node_process_hook_args(id, onode);

    NodesManager::process_new_node(onode, new NodesManager::nodes_manager_args_data(
                                              *loop, VolumeStores::FriendAccessor::get_modifiable_onode_info(id),
                                              VolumeManager::post_node_process_hook, hook_args));
}

void VolumeManager::on_global_remove(void *data, uint32_t id) {
    VolumeStores::FriendAccessor::cleanup_entries_with_onode_id(id);
}

void VolumeManager::cleanup() {
    VolumeStores::FriendAccessor::cleanup();
}