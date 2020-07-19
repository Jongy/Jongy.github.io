#include <stdio.h>

#include <gcc-plugin.h>
#include <tree.h>
#include <tree-iterator.h>
#include <plugin-version.h>
#include <c-family/c-common.h>
#include <stringpool.h>

int plugin_is_GPL_compatible; // must be defined for the plugin to run

// convert the type of 'expr' to text representing its operation, for example "+"" for PLUS_EXPR.
// this list is shortened for brevity.
static const char *get_expr_op_repr(tree expr) {
    const char *op;

    switch (TREE_CODE(expr)) {
    case EQ_EXPR: op = "=="; break;
    case NE_EXPR: op = "!="; break;
    case TRUTH_AND_EXPR:
    case TRUTH_ANDIF_EXPR: op = "&&"; break;
    case TRUTH_OR_EXPR:
    case TRUTH_ORIF_EXPR: op = "||"; break;
    case PLUS_EXPR: op = "+"; break;
    case MINUS_EXPR: op = "-"; break;
    case MULT_EXPR: op = "*"; break;
    case TRUNC_DIV_EXPR: op = "/"; break;
    default: op = NULL; break;
    }

    return op;
}

// the core logic: recursively calls itself for operands of binary operators.
// wtf? c++? vec<..> *& ??
static char *create_expression_repr(tree expr, vec<tree, va_gc> *&args) {
    char buf[1024];

    const char *op = get_expr_op_repr(expr);
    // got a binary operator for this expression?
    if (NULL != op) {
        // then it's binary! descend into its 2 operands.
        char *left = create_expression_repr(TREE_OPERAND(expr, 0), args);
        char *right = create_expression_repr(TREE_OPERAND(expr, 1), args);
        // wrap them up together
        snprintf(buf, sizeof(buf), "(%s) %s (%s)", left, op, right);
        free(left);
        free(right);

        return xstrdup(buf);
    }

    // add current expression to arguments list.
    // since we descend the tree left-to-right, the order of arguments for printf will match the
    // order in this list.
    vec_safe_push(args, expr);
    // use %d for everything to keep things simple.
    return xstrdup("%d");
}

static void patch_assert(tree cond_expr) {
    vec<tree, va_gc> *v;
    vec_alloc(v, 0);

    char *fmt = create_expression_repr(COND_EXPR_COND(cond_expr), v);

    // format comes first.
    vec_safe_insert(v, 0, build_string_literal(strlen(fmt) + 1, fmt));
    free(fmt);

    // UNKNOWN_LOCATION? more on that below.
    tree call = build_call_expr_loc_vec(UNKNOWN_LOCATION, lookup_name(get_identifier("printf")), v);
    vec_free(v);

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
        // IDENTIFIER_POINTER(DECL_NAME(...)) gets the (null-terminated) name string from a declaration.
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
