#include "pipewire/context.h"
#include "pipewire/core.h"
#include "pipewire/main-loop.h"
#include "pipewire/node.h"
#include "pipewire/pipewire.h"
#include "pipewire/proxy.h"
#include "spa/utils/hook.h"
#include <cerrno>
#include <cstring>
#include <iostream>
#include <ostream>
using std::cout;
using std::endl;
using std::string;

void raiseError(bool condition, string message, int status = 1) {
    if (condition) {
        cout << "Error: " << message << endl;
        exit(status);
    }
}

struct registry_event_global_data {
    struct pw_registry *reg;
    struct spa_hook *listener;
};

static void chromium_proxy_event(void *data, const struct pw_node_info *info) {
    if (info->props) {
        const struct spa_dict_item *item;
        spa_dict_for_each(item, info->props) { printf("%s = %s\n", item->key, item->value); }
    }
}

static void reg_event_set_chromium_listeners(void *data, uint32_t id, uint32_t permissions, const char *type,
                                             uint32_t version, const struct spa_dict *props) {

    auto *reg_data = (struct registry_event_global_data *)data;

    if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
        for (uint32_t i = 0; i < props->n_items; i++) {
            if (strcmp(props->items[i].key, "application.name") != 0)
                continue;

            if (strcmp(props->items[i].value, "Chromium") == 0) {

                auto *proxy = (struct pw_proxy *)pw_registry_bind(reg_data->reg, id, type, PW_VERSION_NODE, 0);
                static const struct pw_node_events chromium_node_events = {.version = PW_VERSION_NODE_EVENTS,
                                                                           .info = chromium_proxy_event};

                pw_proxy_add_object_listener(proxy, new spa_hook(), &chromium_node_events, nullptr);
            }

            break;
        }
    }
}

int main() {
    pw_init(nullptr, nullptr);

    // getting context to connect to pipewire daemon
    struct pw_main_loop *loop = pw_main_loop_new(nullptr);
    struct pw_context *context = pw_context_new(pw_main_loop_get_loop(loop), nullptr, 0);

    // connect context to daemon
    struct pw_core *core = pw_context_connect(context, nullptr, 0);
    raiseError(core == nullptr, string("failed to connect to pipewire daemon, ") + strerror(errno), errno);

    struct pw_registry *registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);

    struct spa_hook *listener = new spa_hook();

    static const struct pw_registry_events registry_events = {
        .version = PW_VERSION_REGISTRY_EVENTS,
        .global = reg_event_set_chromium_listeners,
    };

    struct registry_event_global_data reg_data = {registry, listener};
    pw_registry_add_listener(registry, listener, &registry_events, &reg_data);

    pw_main_loop_run(loop);

    pw_proxy_destroy((struct pw_proxy *)registry);
    pw_core_disconnect(core);
    pw_context_destroy(context);
    pw_main_loop_destroy(loop);

    return 0;
}