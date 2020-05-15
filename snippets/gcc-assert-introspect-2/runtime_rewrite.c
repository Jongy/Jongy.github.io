#include <stdio.h>

#include <gcc-plugin.h>
#include <tree.h>
#include <tree-iterator.h>
#include <plugin-version.h>
#include <c-family/c-common.h>
#include <c-tree.h>
#include <stringpool.h>

int plugin_is_GPL_compatible; // must be defined for the plugin to run

// convert the type of 'expr' to text representing its operation, for example "+" for PLUS_EXPR.
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

// builds a COND_EXPR
static inline tree _build_conditional_expr(location_t colon_loc, tree ifexp,
    tree op1, tree op1_original_type, tree op2, tree op2_original_type) {

#if GCCPLUGIN_VERSION >= 8001 // a32c8316ff282ec
    return build_conditional_expr(colon_loc, ifexp, false, op1, op1_original_type,
        colon_loc, op2, op2_original_type, colon_loc);
#else
    return build_conditional_expr(colon_loc, ifexp, false, op1, op1_original_type,
        op2, op2_original_type);
#endif
}

static tree printf_decl;

// builds a printf(format, ...) call with given args
static tree make_printf(const char *format, vec<tree, va_gc> *args) {
    tree fmt = build_string_literal(strlen(format) + 1, format);
    if (args) {
        vec_safe_insert(args, 0, fmt);
        tree call = build_call_expr_loc_vec(UNKNOWN_LOCATION, printf_decl, args);
        vec_free(args);
        return call;
    } else {
        return build_call_expr_loc(UNKNOWN_LOCATION, printf_decl, 1, fmt);
    }
}

static tree make_conditional_expr_repr(tree expr) {
    const enum tree_code code = TREE_CODE(expr);

    // for TRUTH_ANDIF_EXPR/TRUTH_AND_EXPR:
    // * if left fails, we print only left
    // * if right fails, we print (...) && right
    // * if both pass, we print nothing
    if (code == TRUTH_ANDIF_EXPR || code == TRUTH_AND_EXPR) {
        tree stmts = alloc_stmt_list();
        // statements that print the left side
        append_to_statement_list(make_conditional_expr_repr(TREE_OPERAND(expr, 0)), &stmts);

        tree left_stmts = stmts;

        stmts = alloc_stmt_list();
        // statements that print the right side
        append_to_statement_list(make_printf("(...) && (", NULL), &stmts);
        append_to_statement_list(make_conditional_expr_repr(TREE_OPERAND(expr, 1)), &stmts);
        append_to_statement_list(make_printf(")", NULL), &stmts);

        tree right_stmts = stmts;

        // if "left" condition passes, run "right" statements. else run "left" statements.
        tree cond = _build_conditional_expr(UNKNOWN_LOCATION, TREE_OPERAND(expr, 0),
            right_stmts, NULL_TREE,
            left_stmts, NULL_TREE);

        return cond;
    }
    // for TRUTH_ORIF_EXPR/TRUTH_OR_EXPR
    // * if left and right fail, we print both
    // * if any pass, we print nothing
    else if (code == TRUTH_ORIF_EXPR || code == TRUTH_OR_EXPR) {
        tree stmts = alloc_stmt_list();
        append_to_statement_list(make_printf("(", NULL), &stmts);
        append_to_statement_list(make_conditional_expr_repr(TREE_OPERAND(expr, 0)), &stmts);
        append_to_statement_list(make_printf(") || (", NULL), &stmts);
        append_to_statement_list(make_conditional_expr_repr(TREE_OPERAND(expr, 1)), &stmts);
        append_to_statement_list(make_printf(")", NULL), &stmts);

        // if expr passes - print nothing (build_empty_stmt branch).
        // if expr fails - print both
        return _build_conditional_expr(UNKNOWN_LOCATION, expr,
            build_empty_stmt(UNKNOWN_LOCATION), NULL_TREE,
            stmts, NULL_TREE);
    }
    // for others - we always print them - because this code gets called only if the expression it reprs
    // has failed, because the &&/|| code guards it.
    else {
        tree stmts = alloc_stmt_list();

        const char *op = get_expr_op_repr(expr);
        // if it's a binary expression - print both sides.
        if (op != NULL) {
            char format[64];
            (void)snprintf(format, sizeof(format), " %s ", op);

            append_to_statement_list(make_conditional_expr_repr(TREE_OPERAND(expr, 0)), &stmts);
            append_to_statement_list(make_printf(format, NULL), &stmts);
            append_to_statement_list(make_conditional_expr_repr(TREE_OPERAND(expr, 1)), &stmts);
        } else {
            // it's a plain value - print it alone.
            vec<tree, va_gc> *args;
            vec_alloc(args, 2); // 1 for the format
            args->quick_push(expr);

            append_to_statement_list(make_printf("%d", args), &stmts);
        }

        return stmts;
    }
}

static void patch_assert(tree cond_expr) {
    printf_decl = lookup_name(get_identifier("printf"));
    COND_EXPR_ELSE(cond_expr) = make_conditional_expr_repr(COND_EXPR_COND(cond_expr));
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
