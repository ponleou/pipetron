#include "includes/nodes_manager.hpp"

#include "pipewire/context.h"
#include "pipewire/core.h"
#include "pipewire/keys.h"
#include "pipewire/node.h"
#include "pipewire/properties.h"
#include "pipewire/proxy.h"
#include "pipewire/stream.h"
#include "spa/param/param.h"
#include "spa/pod/builder.h"
#include "spa/utils/dict.h"
#include "spa/utils/hook.h"
#include <build.h>
#include <cstdint>
#include <iostream>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/compare.h>
#include <string>

using std::string;

void NodesManager::maybe_run_post_process(nodes_manager_args_data *args) {

    if (!(args->process_args->info_flag && args->process_args->params_flag))
        return;

    spa_hook_remove(args->self_listener);
    delete args->self_listener;
    args->self_listener = nullptr;

    void *hook_args = args->node_processed_hook_args;
    NodesManager::replicate_vnode_args *vnode_args = args->vnode_args;
    args->node_processed_hook_args = nullptr;
    args->vnode_args = nullptr;
    args->post_node_process_hook(vnode_args, hook_args);

    delete args;
    args = nullptr;
}

void NodesManager::on_node_info_process_onode_info(void *data, const struct pw_node_info *info) {

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
                                                    const struct spa_pod *param) {

    auto *process_data = (NodesManager::process_onode_info_args *)data;

    if (id != SPA_PARAM_Format)
        return;

    auto *onode_info = &process_data->onode;
    spa_format_audio_raw_parse(param, &onode_info->audio_info);
    process_data->params_flag = true;
}

void NodesManager::on_node_info_process_callback(void *data, const struct pw_node_info *info) {
    nodes_manager_args_data *args = (nodes_manager_args_data *)data;

    if (args->process_args->info_flag)
        return;

    NodesManager::on_node_info_process_onode_info((void *)args->process_args, info);
    NodesManager::maybe_run_post_process(args);
}

void NodesManager::on_node_param_process_callback(void *data, int seq, uint32_t id, uint32_t index, uint32_t next,
                                                  const struct spa_pod *param) {

    nodes_manager_args_data *args = (nodes_manager_args_data *)data;

    if (args->process_args->params_flag)
        return;

    NodesManager::on_node_param_process_onode_info((void *)args->process_args, seq, id, index, next, param);
    NodesManager::maybe_run_post_process(args);
}

void NodesManager::process_new_node(pw_node *node, nodes_manager_args_data *args) {

    static const struct pw_node_events node_events = {
        .version = PW_VERSION_NODE_EVENTS,
        .info = NodesManager::on_node_info_process_callback,
        .param = NodesManager::on_node_param_process_callback,
    };

    uint32_t param_ids_sub[] = {SPA_PARAM_Format};
    pw_node_subscribe_params(node, param_ids_sub, sizeof(param_ids_sub) / sizeof(param_ids_sub[0]));

    pw_proxy_add_object_listener((struct pw_proxy *)node, args->self_listener, &node_events, args);

    pw_node_enum_params(node, 0, SPA_PARAM_Format, 0, UINT32_MAX, nullptr);
}

void NodesManager::replicate_vnode(const NodesManager::replicate_vnode_args &args,
                                   NodesManager::replicate_vnode_output &output) {

    struct pw_properties *context_props =
        pw_properties_new(PW_KEY_APP_NAME, args.onode.app_process_binary.c_str(), nullptr);
    struct pw_context *virtual_context = pw_context_new(&args.loop, context_props, 0);
    struct pw_core *virtual_core = pw_context_connect(virtual_context, nullptr, 0);

    struct pw_properties *stream_props = pw_properties_new(
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

    struct pw_stream *virtual_stream = pw_stream_new(
        virtual_core,
        (args.override_desc.media_name != "" ? args.override_desc.media_name : args.onode.media_name).c_str(),
        stream_props);

    uint8_t buffer[1024];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod *params[1];
    params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &args.onode.audio_info);

    output.vcontext = virtual_context;
    output.vcore = virtual_core;
    output.vstream = virtual_stream;

    pw_stream_connect(virtual_stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                      (enum pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), params, 1);
}

// void NodesManager::replicate_vnode_node_desc(const NodesManager::replicate_vnode_args &args,
//                                              NodesManager::replicate_vnode_output &output) {

//     struct pw_properties *context_props = pw_properties_new(
//         PW_KEY_APP_NAME, (args.onode.node_description + " (" + string(PROJECT_NAME) + ")").c_str(), nullptr);
//     struct pw_context *virtual_context = pw_context_new(&args.loop, context_props, 0);
//     struct pw_core *virtual_core = pw_context_connect(virtual_context, nullptr, 0);

//     struct pw_properties *stream_props = pw_properties_new(
//         PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_APP_NAME,
//         (args.onode.node_description + " (" + string(PROJECT_NAME) + ")").c_str(), PW_KEY_MEDIA_CLASS,
//         args.onode.media_class.c_str(), PW_KEY_APP_ICON_NAME,
//         (args.onode.node_description + " (" + string(PROJECT_NAME) + ")").c_str(), PW_KEY_APP_PROCESS_BINARY,
//         (args.onode.node_description + " (" + string(PROJECT_NAME) + ")").c_str(), PW_KEY_NODE_DESCRIPTION,
//         (args.onode.node_description + " (" + string(PROJECT_NAME) + ")").c_str(), nullptr);
//     struct pw_stream *virtual_stream =
//         pw_stream_new(virtual_core, ("Replicated " + args.onode.media_name).c_str(), stream_props);

//     uint8_t buffer[1024];
//     struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
//     const struct spa_pod *params[1];
//     params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &args.onode.audio_info);

//     output.vcontext = virtual_context;
//     output.vcore = virtual_core;
//     output.vstream = virtual_stream;

//     pw_stream_connect(virtual_stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
//                       (enum pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), params, 1);
// }