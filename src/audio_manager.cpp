#include "includes/audio_manager.hpp"
#include "includes/nodes_manager.hpp"
#include "pipewire/core.h"
#include "pipewire/stream.h"
#include "spa/utils/dict.h"
#include "spa/utils/hook.h"
#include <build.h>
#include <cstdint>
#include <iostream>
#include <stdlib.h>

using std::cout;
using std::endl;
using std::to_string;

// ========= AUDIO STORES =========
unordered_map<uint32_t, NodesManager::node_info *> AudioStores::omic_infos = {};
unordered_map<uint32_t, NodesManager::node_info *> AudioStores::elec_node_infos = {};
unordered_map<uint32_t, AudioStores::vnode_data *> AudioStores::elec_node_to_vnode_data = {};
unordered_map<uint32_t, AudioStores::vnode_data *> AudioStores::elec_node_to_capture_node_data = {};
unordered_map<uint32_t, AudioStores::transfer_audio_data *> AudioStores::elec_node_to_transfer_audio_data = {};

template <typename T>
void AudioStores::remove_entry_with_node_id(uint32_t node_id, unordered_map<uint32_t, T *> &map) {
    auto it = map.find(node_id);

    if (it != map.end()) {
        delete it->second;
        it->second = nullptr;
        map.erase(it);
    }
}
void AudioStores::log(string msg) {
    cout << msg << endl;
}

bool AudioStores::get_elec_node_binary_name(uint32_t node_id, string &name) {
    auto it = elec_node_infos.find(node_id);

    if (it == elec_node_infos.end())
        return false;

    name = it->second->app_process_binary;
    return true;
}

AudioStores::vnode_data &AudioStores::FriendAccessor::get_modifiable_vnode_data(uint32_t elec_node_id) {
    if (elec_node_to_vnode_data.find(elec_node_id) == elec_node_to_vnode_data.end()) {
        elec_node_to_vnode_data[elec_node_id] = new vnode_data(0, nullptr, nullptr, nullptr);
        // TODO: log, follow set_vnode
    }
    return *elec_node_to_vnode_data[elec_node_id];
}

AudioStores::vnode_data &AudioStores::FriendAccessor::get_modifiable_capture_node_data(uint32_t elec_node_id) {
    if (elec_node_to_capture_node_data.find(elec_node_id) == elec_node_to_capture_node_data.end()) {
        elec_node_to_capture_node_data[elec_node_id] = new vnode_data(0, nullptr, nullptr, nullptr);
        // TODO: log, follow set_vnode
    }
    return *elec_node_to_capture_node_data[elec_node_id];
}

NodesManager::node_info &AudioStores::FriendAccessor::get_modifiable_elec_node_info(uint32_t elec_node_id) {

    if (elec_node_infos.find(elec_node_id) == elec_node_infos.end()) {
        elec_node_infos[elec_node_id] = new NodesManager::node_info(elec_node_id);
        log("New pipewire node ID " + to_string(elec_node_id) + " detected");
    }

    return *elec_node_infos[elec_node_id];
}

AudioStores::transfer_audio_data &AudioStores::FriendAccessor::start_new_transfer_audio_data(uint32_t elec_node_id,
                                                                                             pw_stream &capture,
                                                                                             pw_stream &vnode) {

    if (elec_node_to_transfer_audio_data.find(elec_node_id) != elec_node_to_transfer_audio_data.end()) {
        remove_entry_with_node_id(elec_node_id, elec_node_to_transfer_audio_data);
    }

    elec_node_to_transfer_audio_data[elec_node_id] = new transfer_audio_data(capture, vnode);
    // TODO: log, follow set_vnode

    return *elec_node_to_transfer_audio_data[elec_node_id];
}

NodesManager::node_info &AudioStores::FriendAccessor::get_modifiable_omic_info(uint32_t elec_node_id) {

    if (omic_infos.find(elec_node_id) == omic_infos.end()) {
        omic_infos[elec_node_id] = new NodesManager::node_info(elec_node_id);
        log("New pipewire mic node ID " + to_string(elec_node_id) + " detected");
    }

    return *omic_infos[elec_node_id];
}

void AudioStores::FriendAccessor::cleanup_stores_with_elec_node_id(uint32_t elec_node_id) {

    string onode_name = "";
    if (get_elec_node_binary_name(elec_node_id, onode_name)) {
        log("Cleaning up node ID " + to_string(elec_node_id) + " (" + onode_name + ")");
    }

    remove_entry_with_node_id(elec_node_id, elec_node_infos);
    remove_entry_with_node_id(elec_node_id, elec_node_to_transfer_audio_data);
    remove_entry_with_node_id(elec_node_id, elec_node_to_vnode_data);
    remove_entry_with_node_id(elec_node_id, elec_node_to_capture_node_data);
}

void AudioStores::FriendAccessor::cleanup() {
    for (const auto &[key, value] : omic_infos)
        remove_entry_with_node_id(key, omic_infos);

    for (const auto &[key, value] : elec_node_infos)
        remove_entry_with_node_id(key, elec_node_infos);

    for (const auto &[key, value] : elec_node_to_transfer_audio_data)
        remove_entry_with_node_id(key, elec_node_to_transfer_audio_data);

    for (const auto &[key, value] : elec_node_to_vnode_data)
        remove_entry_with_node_id(key, elec_node_to_vnode_data);

    for (const auto &[key, value] : elec_node_to_capture_node_data)
        remove_entry_with_node_id(key, elec_node_to_capture_node_data);
}

// ========= END OF AUDIO STORES =========
// =======================================
// ============ AUDIO MANAGER ============

// TODO: mic
void *AudioManager::post_mic_process_hook(NodesManager::create_node_args *vnode_args, void *data) {

    auto *onode_id = (uint32_t *)data;

    NodesManager::create_node_output output;
    vnode_args->override_desc.node_description = vnode_args->onode.node_description + " (" + string(PROJECT_NAME) + ")";

    NodesManager::replicate_vnode(*vnode_args, output);

    delete vnode_args;
    vnode_args = nullptr;

    AudioStores::vnode_data &vnode_data = AudioStores::FriendAccessor::get_modifiable_vnode_data(*onode_id);
    vnode_data.context = output.context;
    vnode_data.core = output.core;
    vnode_data.stream = output.stream;

    output.context = nullptr;
    output.core = nullptr;
    output.stream = nullptr;
    vnode_data.id = 0; // default value, will be set by AudioManager::on_state_change_single_callback

    delete onode_id;
    onode_id = nullptr;

    return nullptr;
}

void AudioManager::on_process_capture_store_data_callback(void *data) {
    auto *args = (AudioStores::transfer_audio_data *)data;

    pw_buffer *capture_buf = pw_stream_dequeue_buffer(&args->capture_node);

    if (capture_buf != nullptr) {
        args->store_buffer_to_queue(capture_buf);
        pw_stream_queue_buffer(&args->capture_node, capture_buf);
    }
}

void AudioManager::on_process_vnode_play_data_callback(void *data) {
    auto *args = (AudioStores::transfer_audio_data *)data;

    pw_buffer *vnode_buf = pw_stream_dequeue_buffer(&args->vnode);

    if (vnode_buf != nullptr) {
        args->load_buffer_from_queue(vnode_buf);
        pw_stream_queue_buffer(&args->vnode, vnode_buf);
    }
}

void AudioManager::on_state_changed_vnode_copy_link_direction_single_callback(void *data, enum pw_stream_state old,
                                                                              enum pw_stream_state state,
                                                                              const char *error) {
    if (state != PW_STREAM_STATE_PAUSED)
        return;

    auto *args = (AudioManagerArgs::on_state_changed_vnode_copy_link_direction_args *)data;

    NodesManager::copy_playback_link_direction(args->vnode_stream, pw_stream_get_node_id(&args->vnode_stream),
                                               args->vnode_core, args->vnode_channels, args->onode_id, &args->reg);

    if (args->self_listener) {
        spa_hook_remove(args->self_listener);
        delete args->self_listener;
        args->self_listener = nullptr;
    }

    delete args;
    args = nullptr;
}

void *AudioManager::post_elec_node_process_hook(NodesManager::create_node_args *vnode_args, void *data) {

    auto *args = (AudioManagerArgs::post_elec_node_process_hook_args *)data;

    // create replicate vnode (the one with correct name and icon)
    NodesManager::create_node_output replicate_vnode_output;
    vnode_args->override_desc.app_name = vnode_args->onode.app_process_binary;
    vnode_args->override_desc.app_icon_name = vnode_args->onode.app_process_binary;
    vnode_args->override_desc.media_name = string(PROJECT_NAME) + " " + vnode_args->onode.media_name;
    NodesManager::replicate_vnode(*vnode_args, replicate_vnode_output);

    AudioStores::vnode_data &vnode_data = AudioStores::FriendAccessor::get_modifiable_vnode_data(args->onode_id);
    vnode_data.context = replicate_vnode_output.context;
    vnode_data.core = replicate_vnode_output.core;
    vnode_data.stream = replicate_vnode_output.stream;
    vnode_data.id = 0;

    replicate_vnode_output.context = nullptr;
    replicate_vnode_output.core = nullptr;
    replicate_vnode_output.stream = nullptr;
    vnode_args->override_desc.app_name = "";
    vnode_args->override_desc.app_icon_name = "";
    vnode_args->override_desc.media_name = "";

    // after creating vnode, change the vnode links to copy onode
    static const pw_stream_events vnode_copy_link_event{
        .version = PW_VERSION_STREAM_EVENTS,
        .state_changed = on_state_changed_vnode_copy_link_direction_single_callback,
    };

    auto *state_changed_args = new AudioManagerArgs::on_state_changed_vnode_copy_link_direction_args(
        args->onode_id, args->reg, *vnode_data.core, *vnode_data.stream, vnode_args->onode.audio_info.channels);

    pw_stream_add_listener(vnode_data.stream, state_changed_args->self_listener, &vnode_copy_link_event,
                           state_changed_args);

    // create a capture with links to the target node
    NodesManager::create_node_output create_capture_output;
    vnode_args->override_desc.app_icon_name = vnode_args->onode.app_process_binary;
    vnode_args->override_desc.media_name = "Capture for " + vnode_args->onode.app_process_binary;
    NodesManager::connect_capture_to_onode(*vnode_args, create_capture_output);

    AudioStores::vnode_data &capture_node_data =
        AudioStores::FriendAccessor::get_modifiable_capture_node_data(args->onode_id);
    capture_node_data.context = create_capture_output.context;
    capture_node_data.core = create_capture_output.core;
    capture_node_data.stream = create_capture_output.stream;
    capture_node_data.id = 0;

    vnode_args->override_desc.app_icon_name = "";
    vnode_args->override_desc.media_name = "";
    create_capture_output.context = nullptr;
    create_capture_output.core = nullptr;
    create_capture_output.stream = nullptr;

    delete vnode_args;
    vnode_args = nullptr;

    // create listeners to run the transfer audio
    static const pw_stream_events capture_events = {
        .version = PW_VERSION_STREAM_EVENTS,
        .process = on_process_capture_store_data_callback,
    };

    auto &transfer_audio_data = AudioStores::FriendAccessor::start_new_transfer_audio_data(
        args->onode_id, *capture_node_data.stream, *vnode_data.stream);

    pw_stream_add_listener(capture_node_data.stream, transfer_audio_data.listeners[0], &capture_events,
                           &transfer_audio_data);

    static const pw_stream_events vnode_events = {
        .version = PW_VERSION_STREAM_EVENTS,
        .process = on_process_vnode_play_data_callback,
    };

    pw_stream_add_listener(vnode_data.stream, transfer_audio_data.listeners[1], &vnode_events, &transfer_audio_data);

    delete args;
    args = nullptr;

    return nullptr;
}

// TODO: mic
void AudioManager::process_mic_node(pw_registry *reg, pw_loop *loop, uint32_t id, const char *type) {

    pw_node *mic_node = (pw_node *)pw_registry_bind(reg, id, type, PW_VERSION_NODE, 0);
    // FIXME: delete this bind after

    uint32_t *onode_id = new uint32_t(id);

    NodesManager::process_new_node(mic_node, new NodesManager::nodes_manager_args_data(
                                                 *loop, AudioStores::FriendAccessor::get_modifiable_omic_info(id),
                                                 AudioManager::post_mic_process_hook, onode_id));
}

void AudioManager::process_playback_elec_node(pw_registry *reg, pw_loop *loop, uint32_t id, const char *type) {

    pw_node *elec_node = (pw_node *)pw_registry_bind(reg, id, type, PW_VERSION_NODE, 0);
    // FIXME: delete this bind after

    auto *hook_args = new AudioManagerArgs::post_elec_node_process_hook_args(id, *reg);

    NodesManager::process_new_node(elec_node, new NodesManager::nodes_manager_args_data(
                                                  *loop, AudioStores::FriendAccessor::get_modifiable_elec_node_info(id),
                                                  AudioManager::post_elec_node_process_hook, (void *)hook_args));
}

void AudioManager::enlist_registry_port_event(const uint32_t id, const struct spa_dict *props, pw_registry *reg) {
    PortLinksManager::enlist_registry_port_event(id, props, reg);
}

void AudioManager::enlist_registry_link_event(const uint32_t id, const struct spa_dict *props) {
    PortLinksManager::enlist_registry_link_event(id, props);
}

void AudioManager::enlist_registry_remove_event(uint32_t id) {
    AudioStores::FriendAccessor::cleanup_stores_with_elec_node_id(id);
    PortLinksManager::enlist_registry_remove_event(id);
}

void AudioManager::cleanup() {
    AudioStores::FriendAccessor::cleanup();
    PortLinksManager::cleanup();
}