#include <stdio.h>

#include <gcc-plugin.h>
#include <tree.h>
#include <print-tree.h>
#include <plugin-version.h>

int plugin_is_GPL_compatible; // must be defined & exported for the plugin to be loaded

static void pre_genericize_callback(void *event_data, void *user_data) {
    tree t = (tree)event_data;

    debug_tree(DECL_SAVED_TREE(t));
}

int plugin_init(struct plugin_name_args *plugin_info, struct plugin_gcc_version *version) {
    printf("Loaded! compiled for GCC %s\n", gcc_version.basever);
    register_callback(plugin_info->base_name, PLUGIN_PRE_GENERICIZE, pre_genericize_callback, NULL);

    return 0;
}
