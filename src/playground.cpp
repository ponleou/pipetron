#include "pipewire/client.h"
#include "pipewire/context.h"
#include "pipewire/core.h"
#include "pipewire/main-loop.h"
#include "pipewire/node.h"
#include "pipewire/pipewire.h"
#include "pipewire/proxy.h"
#include <iostream>
#include <map>
#include <ostream>

using std::cout;
using std::endl;
using std::string;

struct node_data {
    struct pw_proxy *proxy;
    struct spa_hook listener;
};

std::map<uint32_t, node_data> nodes;

void raiseError(bool condition, string message, int status = 1) {
    if (condition) {
        cout << "Error: " << message << endl;
        exit(status);
    }
}

struct roundtrip_data {
    int pending;
    struct pw_main_loop *loop;
};

static void on_core_done(void *data, uint32_t id, int seq) {
    auto *d = (struct roundtrip_data *)data;

    if (id == PW_ID_CORE && seq == d->pending) {
        cout << "done seq: " << seq << endl;
        // pw_main_loop_quit(d->loop);
    }
}

static void roundtrip(struct pw_core *core, struct pw_main_loop *loop) {
    static const struct pw_core_events core_events = {
        .version = PW_VERSION_CORE_EVENTS,
        .done = on_core_done,
    };

    struct roundtrip_data d = {.pending = pw_core_sync(core, PW_ID_CORE, 0), .loop = loop};
    struct spa_hook core_listener;
    int err;

    pw_core_add_listener(core, &core_listener, &core_events, &d);

    if ((err = pw_main_loop_run(loop)) < 0)
        printf("main_loop_run error:%d!\n", err);

    spa_hook_remove(&core_listener);
}

static void node_info(void *data, const struct pw_client_info *info) {
    if (info->props) {
        const struct spa_dict_item *item;
        spa_dict_for_each(item, info->props) { printf("%s = %s\n", item->key, item->value); }
    }
}

static const struct pw_node_events node_events = {
    PW_VERSION_NODE_EVENTS,
    .info = node_info,
};

// Replace registry_event_global with:
static void registry_event_global(void *data, uint32_t id, uint32_t permissions, const char *type, uint32_t version,
                                  const struct spa_dict *props) {

    if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
        struct pw_registry *reg = (struct pw_registry *)data;
        struct pw_proxy *proxy = (struct pw_proxy *)pw_registry_bind(reg, id, type, PW_VERSION_NODE, 0);
        struct spa_hook *listener = new spa_hook();
        spa_zero(*listener);
        pw_proxy_add_object_listener(proxy, listener, &node_events, nullptr);
    }
}

int main() {
    struct pw_main_loop *loop;
    struct pw_context *context;
    struct pw_core *core;
    struct pw_registry *registry;
    struct spa_hook registry_listener;

    // registry_events objects connected to the handler registry_event_global
    static const struct pw_registry_events registry_events = {
        .version = PW_VERSION_REGISTRY_EVENTS,
        .global = registry_event_global,
    };

    pw_init(nullptr, nullptr);

    // getting context to connect to pipewire daemon
    loop = pw_main_loop_new(nullptr);
    context = pw_context_new(pw_main_loop_get_loop(loop), nullptr, 0);

    // connect context to daemon
    core = pw_context_connect(context, nullptr, 0);
    raiseError(core == nullptr, "failed to connect to pipewire daemon");

    // setup for listener
    registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
    spa_zero(registry_listener);

    // listener with required setup and events handler
    pw_registry_add_listener(registry, &registry_listener, &registry_events, registry);

    roundtrip(core, loop);

    pw_proxy_destroy((struct pw_proxy *)registry);
    pw_core_disconnect(core);
    pw_context_destroy(context);
    pw_main_loop_destroy(loop);

    return 0;
}