#include "includes/volume_manager.hpp"
#include "includes/nodes_manager.hpp"
#include "pipewire/core.h"
#include "pipewire/node.h"
#include "pipewire/proxy.h"
#include "pipewire/stream.h"
#include "spa/param/param.h"
#include "spa/pod/builder.h"
#include "spa/utils/hook.h"
#include <build.h>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <ostream>
#include <spa/param/audio/format-utils.h>
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

VolumeStores::sync_params_data &VolumeStores::FriendAccessor::get_modifiable_sync_params_data(uint32_t onode_id) {
    if (onode_to_sync_params_data.find(onode_id) == onode_to_sync_params_data.end()) {
        onode_to_sync_params_data[onode_id] = new sync_params_data();
    }

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

void VolumeManager::on_vnode_param_props(void *data, uint32_t id, const struct spa_pod *param) {

    if (id != SPA_PARAM_Props)
        return;

    auto *sync_data = (VolumeStores::sync_params_data *)data;

    if (sync_data->param_data) {
        free(sync_data->param_data);
        sync_data->param_data = nullptr;
    }
    sync_data->param_data = spa_pod_copy(param);

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

    sync_data->ignore_next_onode_event = true;
    pw_node_set_param(sync_data->onode, SPA_PARAM_Props, 0, sync_data->param_data);
}

void *VolumeManager::post_state_process_hook(void *data) {
    auto *args = (VolumeManagerArgs::post_state_process_hook_args *)data;

    VolumeStores::sync_params_data &data_sync =
        VolumeStores::FriendAccessor::get_modifiable_sync_params_data(args->onode_id);
    const auto &vnode = VolumeStores::FriendAccessor::get_modifiable_vnode_data(args->onode_id);
    data_sync.vnode_reg = pw_core_get_registry(vnode.core, PW_VERSION_REGISTRY, 0);
    data_sync.vnode =
        (struct pw_node *)pw_registry_bind(data_sync.vnode_reg, vnode.id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0);

    spa_hook *vnode_listener = new spa_hook();
    spa_hook *onode_listener = new spa_hook();

    data_sync.listeners[0] = vnode_listener;
    data_sync.listeners[1] = onode_listener;

    uint32_t param_ids_sub[] = {SPA_PARAM_Props};

    pw_node_subscribe_params(VolumeStores::FriendAccessor::get_modifiable_sync_params_data(args->onode_id).vnode,
                             param_ids_sub, sizeof(param_ids_sub) / sizeof(param_ids_sub[0]));
    pw_node_subscribe_params(VolumeStores::FriendAccessor::get_modifiable_sync_params_data(args->onode_id).onode,
                             param_ids_sub, sizeof(param_ids_sub) / sizeof(param_ids_sub[0]));

    pw_stream *vstream = vnode.stream;

    static const pw_stream_events vnode_events = {
        .version = PW_VERSION_STREAM_EVENTS,
        .param_changed = VolumeManager::on_vnode_param_props,
    };

    static const pw_node_events onode_events = {
        .version = PW_VERSION_NODE_EVENTS,
        .param = VolumeManager::on_onode_param_props,
    };

    pw_stream_add_listener(vstream, vnode_listener, &vnode_events,
                           (void *)&VolumeStores::FriendAccessor::get_modifiable_sync_params_data(args->onode_id));
    pw_proxy_add_object_listener(
        (pw_proxy *)VolumeStores::FriendAccessor::get_modifiable_sync_params_data(args->onode_id).onode, onode_listener,
        &onode_events, (void *)&VolumeStores::FriendAccessor::get_modifiable_sync_params_data(args->onode_id));

    pw_node_enum_params(VolumeStores::FriendAccessor::get_modifiable_sync_params_data(args->onode_id).vnode, 0,
                        SPA_PARAM_Props, 0, UINT32_MAX, nullptr);
    pw_stream_update_params(vstream, nullptr, 0);

    delete args;
    args = nullptr;

    return nullptr;
}

void VolumeManager::on_state_change_single_callback(void *data, enum pw_stream_state old, enum pw_stream_state state,
                                                    const char *error) {
    VolumeManagerArgs::state_change_single_callback_args *args =
        (VolumeManagerArgs::state_change_single_callback_args *)data;

    if (state == PW_STREAM_STATE_UNCONNECTED || state == PW_STREAM_STATE_ERROR) {
        delete args;
        args = nullptr;
        return;
    }

    if (state == PW_STREAM_STATE_PAUSED && !args->stream_processed_flag) {
        auto &vnode_data = VolumeStores::FriendAccessor::get_modifiable_vnode_data(args->onode_id);
        vnode_data.id = pw_stream_get_node_id(vnode_data.stream);

        args->stream_processed_flag = true;

        spa_hook_remove(args->self_listener);
        delete args->self_listener;
        args->self_listener = nullptr;
    }

    if (args->stream_processed_flag) {
        // running the hook
        void *hook_args = args->post_state_process_hook_args;
        args->post_state_process_hook_args = nullptr;
        args->post_state_process_hook(hook_args);

        delete args;
        args = nullptr;
    }
}

void *VolumeManager::post_node_process_hook(NodesManager::create_node_args *vnode_args, void *data) {

    auto *onode_id = (uint32_t *)data;

    NodesManager::create_node_output output;
    vnode_args->override_desc.app_name = vnode_args->onode.app_process_binary;
    vnode_args->override_desc.app_icon_name = vnode_args->onode.app_process_binary;
    vnode_args->override_desc.media_name = string(PROJECT_NAME) + " volume controller";
    NodesManager::replicate_vnode(*vnode_args, output);
    delete vnode_args;
    vnode_args = nullptr;

    VolumeStores::vnode_data &vnode_data = VolumeStores::FriendAccessor::get_modifiable_vnode_data(*onode_id);
    vnode_data.context = output.context;
    vnode_data.core = output.core;
    vnode_data.stream = output.stream;

    output.context = nullptr;
    output.core = nullptr;
    output.stream = nullptr;
    vnode_data.id = 0; // default value, will be set by VolumeManager::on_state_change_single_callback

    static const pw_stream_events stream_events = {
        .version = PW_VERSION_STREAM_EVENTS,
        .state_changed = VolumeManager::on_state_change_single_callback,
    };

    VolumeManagerArgs::state_change_single_callback_args *callback_args =
        new VolumeManagerArgs::state_change_single_callback_args(
            *onode_id, VolumeManager::post_state_process_hook,
            (void *)new VolumeManagerArgs::post_state_process_hook_args(*onode_id));

    pw_stream_add_listener(vnode_data.stream, callback_args->self_listener, &stream_events, (void *)callback_args);

    delete onode_id;
    onode_id = nullptr;

    return nullptr;
}

void VolumeManager::process_new_node(pw_registry *reg, pw_loop *loop, uint32_t id, const char *type) {

    VolumeStores::FriendAccessor::get_modifiable_sync_params_data(id).onode =
        (pw_node *)pw_registry_bind(reg, id, type, PW_VERSION_NODE, 0);

    uint32_t *onode_id = new uint32_t(id);

    NodesManager::process_new_node(
        VolumeStores::FriendAccessor::get_modifiable_sync_params_data(id).onode,
        new NodesManager::nodes_manager_args_data(*loop, VolumeStores::FriendAccessor::get_modifiable_onode_info(id),
                                                  VolumeManager::post_node_process_hook, onode_id));
}

void VolumeManager::on_global_remove(void *data, uint32_t id) {
    VolumeStores::FriendAccessor::cleanup_entries_with_onode_id(id);
}

void VolumeManager::cleanup() {
    VolumeStores::FriendAccessor::cleanup();
}