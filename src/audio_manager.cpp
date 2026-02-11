#include "includes/audio_manager.hpp"
#include <build.h>
#include <iostream>
#include <string>

using std::cout;
using std::endl;
using std::to_string;

unordered_map<uint32_t, NodesManager::onode_info *> AudioStores::omic_infos = {};
unordered_map<uint32_t, NodesManager::onode_info *> AudioStores::onode_infos = {};
unordered_map<uint32_t, AudioStores::vnode_data *> AudioStores::onode_to_vnode_data = {};
unordered_map<uint32_t, AudioStores::sync_params_data *> AudioStores::onode_to_sync_params_data = {};

template <typename T>
void AudioStores::remove_entry_with_onode(uint32_t onode_id, unordered_map<uint32_t, T *> &map) {
    auto it = map.find(onode_id);

    if (it != map.end()) {
        delete map.at(it->first);
        it->second = nullptr;
        map.erase(it);
    }
}

void AudioStores::log(string msg) {
    cout << msg << endl;
}

bool AudioStores::get_onode_binary_name(uint32_t onode_id, string &name) {
    auto it = onode_infos.find(onode_id);

    if (it == onode_infos.end())
        return false;

    name = it->second->app_process_binary;
    return true;
}

AudioStores::vnode_data &AudioStores::FriendAccessor::get_modifiable_vnode_data(uint32_t onode_id) {
    if (onode_to_vnode_data.find(onode_id) == onode_to_vnode_data.end()) {
        onode_to_vnode_data[onode_id] = new vnode_data(0, nullptr, nullptr, nullptr);
        // TODO: log, follow set_vnode
    }
    return *onode_to_vnode_data[onode_id];
}

NodesManager::onode_info &AudioStores::FriendAccessor::get_modifiable_onode_info(uint32_t onode_id) {

    if (onode_infos.find(onode_id) == onode_infos.end()) {
        onode_infos[onode_id] = new NodesManager::onode_info(onode_id);
        log("New pipewire node ID " + to_string(onode_id) + " detected");
    }

    return *onode_infos[onode_id];
}

NodesManager::onode_info &AudioStores::FriendAccessor::get_modifiable_omic_info(uint32_t onode_id) {

    if (omic_infos.find(onode_id) == omic_infos.end()) {
        omic_infos[onode_id] = new NodesManager::onode_info(onode_id);
        log("New pipewire mic node ID " + to_string(onode_id) + " detected");
    }

    return *omic_infos[onode_id];
}

AudioStores::sync_params_data &AudioStores::FriendAccessor::get_modifiable_sync_params_data(uint32_t onode_id) {
    if (onode_to_sync_params_data.find(onode_id) == onode_to_sync_params_data.end()) {
        onode_to_sync_params_data[onode_id] = new sync_params_data();
    }

    return *onode_to_sync_params_data[onode_id];
}

void AudioStores::FriendAccessor::cleanup_elec_node_entries_with_onode_id(uint32_t onode_id) {

    string onode_name = "";
    if (get_onode_binary_name(onode_id, onode_name)) {
        log("Cleaning up node ID " + to_string(onode_id) + " (" + onode_name + ")");
    }

    remove_entry_with_onode<NodesManager::onode_info>(onode_id, onode_infos);
    remove_entry_with_onode<vnode_data>(onode_id, onode_to_vnode_data);
    remove_entry_with_onode<sync_params_data>(onode_id, onode_to_sync_params_data);
}

void AudioStores::FriendAccessor::cleanup() {
    for (const auto &[key, value] : onode_infos)
        remove_entry_with_onode<NodesManager::onode_info>(key, onode_infos);

    for (const auto &[key, value] : onode_to_vnode_data)
        remove_entry_with_onode<vnode_data>(key, onode_to_vnode_data);

    for (const auto &[key, value] : onode_to_sync_params_data)
        remove_entry_with_onode<sync_params_data>(key, onode_to_sync_params_data);
}

void *AudioManager::post_mic_process_hook(NodesManager::replicate_vnode_args *vnode_args, void *data) {

    auto *onode_id = (uint32_t *)data;

    NodesManager::replicate_vnode_output output;
    vnode_args->override_desc.node_description = vnode_args->onode.node_description + " (" + string(PROJECT_NAME) + ")";

    NodesManager::replicate_vnode(*vnode_args, output);

    delete vnode_args;
    vnode_args = nullptr;

    AudioStores::vnode_data &vnode_data = AudioStores::FriendAccessor::get_modifiable_vnode_data(*onode_id);
    vnode_data.context = output.vcontext;
    vnode_data.core = output.vcore;
    vnode_data.stream = output.vstream;

    output.vcontext = nullptr;
    output.vcore = nullptr;
    output.vstream = nullptr;
    vnode_data.id = 0; // default value, will be set by AudioManager::on_state_change_single_callback

    delete onode_id;
    onode_id = nullptr;

    return nullptr;
}

void *AudioManager::post_elec_node_process_hook(NodesManager::replicate_vnode_args *vnode_args, void *data) {

    auto *onode_id = (uint32_t *)data;

    NodesManager::replicate_vnode_output output;
    vnode_args->override_desc.app_name = vnode_args->onode.app_process_binary;
    vnode_args->override_desc.app_icon_name = vnode_args->onode.app_process_binary;
    vnode_args->override_desc.media_name = string(PROJECT_NAME) + " " + vnode_args->onode.media_name;
    NodesManager::replicate_vnode(*vnode_args, output);
    delete vnode_args;
    vnode_args = nullptr;

    AudioStores::vnode_data &vnode_data = AudioStores::FriendAccessor::get_modifiable_vnode_data(*onode_id);
    vnode_data.context = output.vcontext;
    vnode_data.core = output.vcore;
    vnode_data.stream = output.vstream;

    output.vcontext = nullptr;
    output.vcore = nullptr;
    output.vstream = nullptr;
    vnode_data.id = 0; // default value, will be set by VolumeManager::on_state_change_single_callback

    // static const pw_stream_events stream_events = {
    //     .version = PW_VERSION_STREAM_EVENTS,
    //     .state_changed = VolumeManager::on_state_change_single_callback,
    // };

    // VolumeManagerArgs::state_change_single_callback_args *callback_args =
    //     new VolumeManagerArgs::state_change_single_callback_args(
    //         *onode_id, VolumeManager::post_state_process_hook,
    //         (void *)new VolumeManagerArgs::post_state_process_hook_args(*onode_id));

    // pw_stream_add_listener(vnode_data.stream, callback_args->self_listener, &stream_events, (void *)callback_args);

    delete onode_id;
    onode_id = nullptr;

    return nullptr;
}

void AudioManager::process_mic_node(pw_registry *reg, pw_loop *loop, uint32_t id, const char *type) {

    AudioStores::FriendAccessor::get_modifiable_sync_params_data(id).onode =
        (pw_node *)pw_registry_bind(reg, id, type, PW_VERSION_NODE, 0);

    uint32_t *onode_id = new uint32_t(id);

    NodesManager::process_new_node(
        AudioStores::FriendAccessor::get_modifiable_sync_params_data(id).onode,
        new NodesManager::nodes_manager_args_data(*loop, AudioStores::FriendAccessor::get_modifiable_omic_info(id),
                                                  AudioManager::post_mic_process_hook, onode_id));
}

void AudioManager::process_elec_node(pw_registry *reg, pw_loop *loop, uint32_t id, const char *type) {

    AudioStores::FriendAccessor::get_modifiable_sync_params_data(id).onode =
        (pw_node *)pw_registry_bind(reg, id, type, PW_VERSION_NODE, 0);

    uint32_t *onode_id = new uint32_t(id);

    NodesManager::process_new_node(
        AudioStores::FriendAccessor::get_modifiable_sync_params_data(id).onode,
        new NodesManager::nodes_manager_args_data(*loop, AudioStores::FriendAccessor::get_modifiable_onode_info(id),
                                                  AudioManager::post_elec_node_process_hook, onode_id));
}

void AudioManager::on_global_remove(void *data, uint32_t id) {
    // TODO:
    AudioStores::FriendAccessor::cleanup_elec_node_entries_with_onode_id(id);
}

void AudioManager::cleanup() {
    AudioStores::FriendAccessor::cleanup();
}