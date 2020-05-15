#include <stdio.h>

#include <gcc-plugin.h>
#include <tree.h>
#include <tree-iterator.h>
#include <plugin-version.h>
#include <c-family/c-common.h>
#include <stringpool.h>

int plugin_is_GPL_compatible; // must be defined for the plugin to run

static void patch_assert(tree cond_expr) {
    tree left = TREE_OPERAND(COND_EXPR_COND(cond_expr), 0);
    tree right = TREE_OPERAND(COND_EXPR_COND(cond_expr), 1);
    tree fmt = build_string_literal(sizeof("%d == %d\n"), "%d == %d\n");

    tree call = build_call_expr(lookup_name(get_identifier("printf")), 3, fmt, left, right);

    // embed it in the expression - replace __assert_fail() call with it
    COND_EXPR_ELSE(cond_expr) = call;
}

static bool is_assert_fail_cond_expr(tree expr) {
    if (TREE_CODE(expr) != COND_EXPR) {
        return false;
    }

    tree expr_else = COND_EXPR_ELSE(expr);
    return (
        TREE_CODE(COND_EXPR_THEN(expr)) == NOP_EXPR &&
        TREE_CODE(expr_else) == CALL_EXPR &&
        TREE_CODE(CALL_EXPR_FN(expr_else)) == ADDR_EXPR &&
        TREE_CODE(TREE_OPERAND(CALL_EXPR_FN(expr_else), 0)) == FUNCTION_DECL &&
        // IDENTIFIER_POINTER(DECL_NAME(...)) gets the (null-terminated) name string from of declaration.
        0 == strcmp("__assert_fail", IDENTIFIER_POINTER(DECL_NAME(TREE_OPERAND(CALL_EXPR_FN(expr_else), 0))))
    );
}

static void iterate_function_body(tree expr) {
    tree body;

    if (TREE_CODE(expr) == BIND_EXPR) {
        body = BIND_EXPR_BODY(expr);
    } else {
        gcc_assert(TREE_CODE(expr) == STATEMENT_LIST);
        body = expr;
    }

    if (TREE_CODE(body) == STATEMENT_LIST) {
        for (tree_stmt_iterator i = tsi_start(body); !tsi_end_p(i); tsi_next(&i)) {
            tree stmt = tsi_stmt(i);

            if (TREE_CODE(stmt) == BIND_EXPR) {
                iterate_function_body(stmt);
            }
        }
    } else {
        if (is_assert_fail_cond_expr(body)) {
            gcc_assert(TREE_CODE(expr) == BIND_EXPR);
            gcc_assert(TREE_CODE(body) == COND_EXPR);

            patch_assert(body);
        }
    }
}

static void pre_genericize_callback(void *event_data, void *user_data) {
    tree t = (tree)event_data;

    if (TREE_CODE(t) == FUNCTION_DECL) {
        iterate_function_body(DECL_SAVED_TREE(t));
    }
}

int plugin_init(struct plugin_name_args *plugin_info, struct plugin_gcc_version *version) {
    printf("I'm loaded!, compiled for GCC %s\n", gcc_version.basever);
    register_callback(plugin_info->base_name, PLUGIN_PRE_GENERICIZE, pre_genericize_callback, NULL);

    return 0;
}
