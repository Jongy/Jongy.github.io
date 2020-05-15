---
layout: post
title:  "Assert Rewriting - A GCC plugin - part 2"
date:   2020-05-15
categories:
---

This post continues from where we stopped in [part 1]({% post_url 2020-04-25-gcc-assert-introspect %}) (to recall - last
thing we've done was a simple modification of a single node in the AST, and we saw how it reflected in the result)

Today we'll build the logic of assert rewriting and also diver deeper into GCC's API. We'll try to improve typical
assert messages like:

<img src="/assets/img/regular-assert.png"/>

to something nicer:

<img src="/assets/img/patched-assert.png"/>

Well, maybe not the entire of it, but the core parts :) The full thing can be found [in the plugin][plugin].

## Asserts

What's the code in `assert`, actually? Is it a macro? A function? Something else?
The answer depends on which C library you use (`assert` comes from `assert.h` which is a part of standard C, distributed
with the C library / libc).
We'll follow `glibc` which is probably what you have installed. Most desktop Linux distributions use it, so most likely
you have it as well.
*See [this SO][which-libc] question for instructions on determining the libc your GCC uses.*

We can see what's underneath `assert` this way:
```bash
$ cat dummy.c
#include <assert.h>
assert(1 == 1)
$ gcc -E dummy.c
```

`gcc -E` doesn't compile (indeed, this code wouldn't compile) - it runs the preprocessor (doing all includes
and macros substitution), and dumps the result to the standard output.

These are the final few lines:
```c
# 2 "dummy.c" 2
((void) sizeof ((
# 2 "dummy.c"
1 == 1
# 2 "dummy.c" 3 4
) ? 1 : 0), __extension__ ({ if (
# 2 "dummy.c"
1 == 1
# 2 "dummy.c" 3 4
) ; else __assert_fail (
# 2 "dummy.c"
"1 == 1"
# 2 "dummy.c" 3 4
, "dummy.c", 2, __extension__ __PRETTY_FUNCTION__); }))
```

The `#` marks in the output are hints the preprocessor gives to the compiler - they can be ignored.
`__extension__` can also be ignored, it's [used to suppress certain warnings][gcc-extensions-keyword].
Removing them, we remain with:
```c
((void) sizeof ((1 == 1) ? 1 : 0), ({ if (1 == 1) ; else __assert_fail ("1 == 1", "dummy.c", 2, __PRETTY_FUNCTION__); }))
```

The `sizeof` is the first operand of the comma operator, so its result is really unused (it's also `void`ed...).
[A comment][glibc-assert-h] in glibc's source explains it's there to trigger warnings possibly masked due to the previous use of
`__extension__`.

The second operand is the `assert` condition itself: if the condition passes, do nothing; else, call `__assert_fail`,
give it some info (the stringified expression, file name, line number and function name) and it'll do the dirty work.

To patch asserts, we first need to identify them in the AST. Let's see how this code we just saw is represented.

### Asserts in the AST

Write this `dummy.c`:

```c
#include <assert.h>

void test(int n) {
    assert(n == 42);
}
```

Remember our `basic.c` plugin from part 1? We'll change it to run `debug_tree(DECL_SAVED_TREE(t))`,
and compile `dummy.c` with it. *Plugin code is [here][very-basic]*.

*In this post I'll show shortened versions of AST prints, we already understand their structure so I can omit irrelevant
parts.*

```
 <bind_expr ...
    ...
    body <statement_list ...
        ....
        stmt <bind_expr ...
            body <cond_expr ...
                arg:0 <eq_expr ...
                    arg:0 <parm_decl ... n>
                    arg:1 <integer_cst ... constant 42>
                arg:1 <nop_expr ... type <void_type ...
                    arg:0 <integer_cst ... constant 0>
                arg:2 <call_expr ...
```

First we see a `BIND_EXPR` inside a `STATEMENT_LIST`, inside another `BIND_EXPR`!
The outer bind is the function block. It contains a `STATEMENT_LIST`, which is a grouping
of multiple, sequential statements. Inside the list there's another `BIND_EXPR` - this is the block created by
the `assert` (see the `({ })` wrapping in the generated code?).

Now, the body of that last bind is the core logic. It is a `COND_EXPR`, which represents an `if`, or a ternary operator.
It has 3 operands: the condition, an expression to be evaluated if the test passes (`if { ... }` clause) and
an expression to to be evaluated if the test fails (`else { ... }` clause).

* The condition is is an `EQ_EXPR`, which tests for equality between its 2 operands: our `PARM_DECL` of `n`, and the
constant `42`.
* The `if { ... }` clause is a `NOP_EXPR` whose operand is the constant `0`. `NOP_EXPR`s represent conversions which
don't require code generation (e.g `int *p; (char*)p;`). This `NOP_EXPR` has type `void_type`, so it's a `void` cast;
that is, we have `(void)0`. This is the common "empty statement" GCC uses, for when it must put *some* statement
(see `build_empty_stmt` in GCC, which generates it).
* The `else { ... }` clause is a `CALL_EXPR` which represents a function call: the call to `__assert_fail`.

Take a deeper look on the `CALL_EXPR` itself:
```
...
  <call_expr ...
      fn <addr_expr ...
          ... arg:0 <function_decl ... __assert_fail>
      arg:0 ...
      arg:1 ...
      arg:2 ...
      arg:3 ...
...
```

It has the called function in `fn`, and 4 arguments. The called function is an `ADDR_EXPR`: these nodes
represents the address of an object. The object whose address is used here is the `FUNCTION_DECL` of
`__assert_fail`.

I think that's enough to write code that "macthes" on AST nodes which are `assert`s - that is, the main `COND_EXPR`
implementing them. The code (which I also use in the plugin) is as follows:

```c
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
```

## Traversal of function's AST

We saw that the AST may contain nested `BIND_EXPR`s and `STATEMENT_LIST`s. This calls for some recursive traversal,
displayed in this pseudocode:
```
traverse(expr)
    if expr is BIND_EXPR
        expr = bind body of expr

    if expr is STATEMENT_LIST
        for each statement in list
            traverse(statement)
    else
        if is_assert_fail_cond_expr(expr)
            patch assert <-- here the magic happens

traverse(DECL_SAVED_TREE(function))
```

We'll use the implementation of it in our next example, but I won't elaborate on it. If you're interested you can see it the
[plugin code][iterate_function_body].

## Assert Rewriting

Let's start with something simple: generate a nicer "assert failed" message for the `assert(n == 42)`
we had earlier. Instead of the usual ```Assertion `n == 42' failed```, we'll aim for `%d == 42`,
where `%d` is the runtime value of the "bad" number.

We can retain the `COND_EXPR` condition intact, and just replace the call to `__assert_fail` with our print.
Basically, for the given assert, we just need to generate the equivalent of `printf("%d == %d", n, 42)`. Code will
be generated based on input, without hard-coding values, so if the assert changes to `n == 43`, our plugin will
generate `printf("%d == %d", n, 43)`.

Recall we had the `EQ_EXPR`, being the assert condition.

```
  <eq_expr ..
      arg:0 <parm_decl .. n>
      arg:1 <integer_cst .. constant 42>
```

If we can construct a `printf` call and use these 2 as arguments, that might work.

How can we construct calls? `build_call_expr`! Very obvious name.
*Actually, it wasn't that obvious to me originally... and I found `build_function_call` instead,
which caused me huge problems we'll discuss later.*

Anyway, `build_call_expr` accepts 2 + N arguments: the function to be called (e.g `FUNCTION_DECL` or a function
pointer); N, the number of arguments, and then the arguments themselves.

How can we get the `FUNCTION_DECL` of `printf`? In a quick look for functions named "lookup", "search", "identifier"
etc in `tree.h` I couldn't find anything related.
We might not be acquainted with GCC's codebase, but we know the compiler's behavior a bit... When does GCC search for
identifiers in the compilation scope? Right before a search fails and it emits `implicit declaration of function`!

Search "implicit declaration of function" in GCC's code (you can narrow down the search directories to
`gcc/{c,c-family}`, where the C frontend code is). All non-comment results are in `c-decl.c`. Actually, all of them
are in `implicit_decl_warning`. This function is called in 2 sites from `implicitly_declare`, which itself is called
from `build_external_ref` in `c-typeck.c`.

`build_external_ref` gets a `tree id` which is an identifier tree - these are common, identifier trees
are e.g the `DECL_NAME` of declarations. It calls `lookup_name(id)` then runs some more Objective-C-related stuff which
we don't care about, and finally checks if `decl` is a `NULL_TREE` - if so, it calls `implicitly_declare`.

So we'll use `get_identifier` to get the identifier node, then use `lookup_name` (which resolves the symbol
according to C scoping rules). Together, `lookup_name(get_identifier("printf"))`.

Last thing we need is to generate the arguments. First argument is a string constant, which we can build
with `build_string_literal`. Then, we extract the other arguments from the `assert` condition. Our new `patch_assert`
function has the `COND_EXPR`. Get the condition with `COND_EXPR_COND` then extract the 2 sides of `EQ_EXPR` with
`TREE_OPERAND`.

Wrapping them all together, we get:
```c
tree left = TREE_OPERAND(COND_EXPR_COND(cond_expr), 0);
tree right = TREE_OPERAND(COND_EXPR_COND(cond_expr), 1);
tree fmt = build_string_literal(sizeof("%d == %d\n"), "%d == %d\n");

tree call = build_call_expr(lookup_name(get_identifier("printf")), 3, fmt, left, right);

// embed it in the expression - replace __assert_fail() call with it
COND_EXPR_ELSE(cond_expr) = call;
```

The full code as of this point can be seen [here][basic_rewrite].

Create a dummy `main()` to invoke our `test` function with the assert.
```c
void test(int n);

int main(void) {
    test(5);
    return 0;
}
```

Add `#include <stdio.h>` in `dummy.c` - our code now depends on `printf`'s declaration.
```c
#include <assert.h>
#include <stdio.h>

void test(int n) {
    assert(n == 42);
}
```

Compile and run:
```bash
$ g++ -g -I`gcc -print-file-name=plugin`/include -fpic -shared -o basic.so basic_rewrite.c
$ gcc -fplugin=./basic.so dummy.c main.c -o main
$ ./main
5 == 42
```

Cool! It worked! And if we change the 5 or the 42, we'll see the values changing accordingly.

### Complex expressions

Current prints can be made nicer, but the bigger problem is - we can handle only a single type of expressions.
What if function calls are involved? Logical operators like `&&` and `||`? Binary expressions like `+` and `*`?

Obviously we'd like to follow them and print their operands / sub-expressions as well; It's a recursive structure
so our code should be recursive as well.

Here's the logic I thought of:
* Recursively follow both operands of logical & binary operators using depth-first search.
* When going one level deeper, wrap operand with parentheses.
* Leaf expressions are collected in a list, eventually passed as arguments to `printf`.

The code for this part, including explaining comments: ([full code][complex_rewrite])
```c
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
```

A few things about the code, before we run it.

First and most important, C++?? Quite surprising. At least, I was surprised...
Although it's a well-known fact that GCC is written in C++, since the API until now has been quite C-ish, this fact has
slipped my mind.
The first time I *realized again* C++ is mixed here was when I searched for call sites of `build_string_literal`. Its
definition in `tree.c` takes 3 arguments, but all calls to it were passing only 2. Then I looked in its
declaration, and saw it has a default parameter `tree = char_type_node`. AFAIK, C has no default arguments :(

GCC's codebase is C++ but many areas of the code are indeed very C-like (making no use of classes and making heavy use of macros).
However, some areas do make use of C++ to its full extents, including heavy use of templates. This sums it up well:

<img src="/assets/img/gcc-internals.jpeg" title="Thanks @sapir" width="400"/>

About the `UNKNOWN_LOCATION`: it is a special constant of type `location_t`. `location_t`s are used widely around GCC
code. These represent the source code location of expressions, declarations etc - many tree types have a location field,
which can be accessed with `EXPR_LOCATION` / `SET_EXPR_LOCATION`.
These help GCC direct its compilation errors/warnings at the right spot in your source code, and are probably used for
other things as well (e.g generating debug info). You have the `location_t` of a node in `debug_tree` output, it is right before
the closing `>`, for example `dummy.c:4:5 start: dummy.c:4:5 finish: dummy.c:4:5`.
`UNKNOWN_LOCATION` can substitute any location, but it prevents GCC from relating diagnostics messages
with our source code. *In the plugin code, I take the location of the core `COND_EXPR` once, then use it everywhere.*

Okay, let's try to run (reminder - full code is [here][complex_rewrite]).

Change `dummy.c` to:
```c
#include <assert.h>
#include <stdio.h>

void test(int n) {
    assert(n % 2 == 0 && n - 100 == 150);
}
```

Compile everything, and run...
```bash
$ ./main
((1) == (0)) && ((5) == (250))
```

Awesome!

By the way, notice anything weird above? The right expression appears to be `n == 250`, not `n - 100 == 150` as we
have written. Well, even though our plugin runs quite early (before most optimizations take place), some processing was
already done on the AST, and expressions have changed.

### More complexity

Any problems with current logic? We'll explore now.

#### Double evaluation

Let's start with this: what if instead of `n`, we embed a function call expression?

```c
#include <assert.h>
#include <stdio.h>

int func(int n) {
    printf("func called!\n");
    return n + 7;
}

void test(int n) {
    assert(func(n) % 2 == 0 && n - 100 == 150);
}
```

Compile and run:
```bash
$ ./main
func called!
func called!
((0) == (0)) && ((5) == (250))
```

Twice? But we called it once only! Problem is, we took the expressions from `assert` and placed them in our `printf`.
With variables that's probably okay, but with function calls (i.e `CALL_EXPR`s) - each appearance of them will lead
to a function call. *We can't just "clone" expressions that have side effects!*

In the previous post I presented a simple example of "rewriting" an assert in Python. The condition contained a
function call, so we stashed the result in a temporary variable which was used multiple times, instead of calling
the function again.

So... I was about to develop something similar here. Before diving into it, I spent some time reading the various
documents on expression trees, and was lucky to find the savior: `SAVE_EXPR`. This node does exactly what I needed! A
`SAVE_EXPR` can wrap another expression (which might have side effects), then the `SAVE_EXPR` itself can be used
instead, and it will provide the same value. No matter how many times you clone the same `SAVE_EXPR`, the underlying expression
is evaluated at most one time!

Instead of creating variables for all temporary values, all I needed to do was to wrap all expressions with
`SAVE_EXPR`. I won't go into details on how it's done here, but can see it [in the plugin][wrap_in_save_expr].

Lesson here: get acquainted with the various types of trees, they might come in handy.

#### Short-circuts

What'll happen with the following code:

```c
#include <assert.h>
#include <stdio.h>

void test(int *p) {
    assert(p != NULL && *p == 5);
}
```

```c
#include <stddef.h>

void test(int *p);

int main(void) {
    test(NULL);
    return 0;
}
```

Well, the assert should fail - `p != NULL` is false. But if we run it...
```bash
$ ./main
[1]    602485 segmentation fault (core dumped)  ./main
```

Da hell. What's going on here?

The problem lies within our *static* processing of the expression. We create a `printf` containing all operands of
a complex expression; but in reality, due to short-circuiting, parts of the expression are not evaluated at all:
* With `&&`: `5 == 4 && i_am_not_evaluated()`
* With `||`: `5 == 5 || i_am_not_evaluated()`.

Following these basic rules of `&&` and `||`, it gets more complex with larger expressions:
* `((4 == 3 || evaluated()) && 1 == 0) && not_evaluated())`

Well, obviously the *runtime* result of expressions must be taken into account when creating the assert message...

## Runtime generation

At this point I made a stop and went to see how `py.test` does its rewriting (after all, this whole idea came from
it). The core logic of rewriting is in [this file][pytest-assert-rewrite.py]; it's very long, so I decided it
will be easier to look in the output it generates instead :) Lucky for us `py.test` assert rewriting outputs Python (byte)code
and not assembly.

Take this simple test file:
```python
def func(n):
    return n + 3


def test_a():
    assert func(4) == 5 and func(5) == 8
```

`pyc` files are written for test files; these include the modifications made by assert rewriting, so we just need to
decompile the file generated after one test run.

Grab `uncompyle6`, a nice Python decompiler. I recommend using Python 3.6/3.7 because `uncompyle6` failed for me on
3.8.

Easiest done with Docker:
```bash
$ docker run --rm -it python:3.6 /bin/bash
docker$ pip install pytest uncompyle6
docker$ python -m pytest test.py  # test.py is the simple test file from above
docker$ uncompyle6 __pycache__/test.cpython-36-pytest-*.pyc
```

It will output a *very* long piece of Python code, compared to how simple and short our assert originally was. It also
makes use of too many obscure character sequences, and Jekyll (the framework rendering this site) wouldn't agree to render
it, so I put it [here][decompiled_test] instead.

First thought I had in mind when I saw this was "Too bad Python doesn't have `SAVE_EXPR`... :)". It would really
alleviate the use of all temporary variables there.

Anyway, the logic is roughly: (omitting all irrelevant extra/temporary variables, etc)
```python
res1 = func(4) == 5
res_both = res1
if res_both:
    res2 = func(5) == 8
    res_both = res2
if not res_both:
    # assert failed!
    # res1 holds the result of the left expr
    # res2 holds the result of the right, only if res1 is True!
    ...
```

So `py.test` rewrote the entire condition, while conforming to short-circuiting rules: it won't exeucte the right-hand
part of the `and` expression if the left-hand part failed. It also collects information during execution, to be used
if the condition fails eventually.

We can try doing what `py.test` does: replace the condition completely, and run it part by part. But this means that if our
logic is broken, we can break the assert condition and its result might come wrong, which is bad. Instead, I decided to add new
code only in the "failed" branch of the condition, so even if my code introduces some errors, they won't affect the code
in the "happy flow" (i.e, when everything works).
The new code will walk the condition tree in runtime and create the assert message. Since all sub expressions are
wrapped in `SAVE_EXPR`, "re-walking" the condition won't re-evaluate anything.

The generated logic for `assert(x == 1 && y == 2);` will look roughly like this C code:
```c
if (x == 1 && y == 2) {
    // all good
} else {
    if (x == 1) {
        // note the "(...) &&"" part, telling us this is the right-hand part of &&.
        printf("(...) && (%d == 2)", y);
    } else {
        printf("%d == 1", x);
    }
}
```

And if we add more complexity? for `assert((x == 1 && y == 2) || z == 3)` it will be:
```c
if ((x == 1 && y == 2) || z == 3) {
    // all good
} else {
    printf("(");
    if (x == 1) {
        printf("(...) && (%d == 2)", y);
    } else {
        printf("%d == 1", x);
    }
    printf(") || (%d == 3)", z);
}
```

The real logic happens to be a bit more complex :P Mostly because generation has to be recursive (because expressions are
recursive). Following are the relevant parts, commented.
The core function is `make_conditional_expr_repr` which recursively calls itself on the expression tree branches, creating statements
that print the "assert fail" message for each part. The statements are conditional - based on the *runtime* value of expressions,
they will print appropriate messages.

*I omitted many parts for brevity (i.e, no `SAVE_EXPR`s handling here).*
```c
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
```

Full code for this part is [here][runtime_rewrite].

Let's try it with the previously crashing snippet. Compile and run it again:
```bash
$ ./main
0 != 0
```

Good. It prints only the left part of `&&`. Now change `test(NULL)` to `int n = 4; test(&n)` and run again:
```bash
$ ./main
(...) && (4 == 5)
```

As expected. And now with `||`, change the `assert` to `assert(p == NULL || *p == 5)` and run:
```bash
$ ./main
(75904660 == 0) || (4 == 5)
```

Awesome :)

Alright, that's enough for this post. There are a few more core features/concepts in the plugin, but this post is really long enough
already, kudos for you the readers who have made it here.

I hope you found this interesting and useful, if you intend to write plugins!

Also, the plugin itself has been useful to me, and hopefully other developers will make use of it too.

<img src="/assets/img/making-the-compiler-do-it.jpeg"/>

## A bit on debugging

Kidding, I'm not done yet!!

I'll finish this post with 2 debugging examples I'd encountered during the development of this plugin. Besides having
somewhat funny conclusion (well, funny in my eyes :), they're also good teaching for those who want to try working
with GCC's internals.

### Runtime compiler errors

When writing code, you encounter compilation errors; plugins are no exception. You'll also get runtime errors.
But in compiler-related code, runtime errors have graver effects than usually expected: your code may seem to run fine, but if it has
any erroneous behavior it might extend beyond the "run time" of your plguin - errors can be introduced into the (legit) code your
plugin helps building!

I encountered some errors of this type during development. The most annoying one was as follows: compiling a small `assert`
test program worked fine (that is, the assert rewriting code was alright); but when I compiled a bigger project with it, failing
`assert`s would SIGSEGV in the rewritten `assert` logic.

How do we investigate this? You don't have the crashing code... :) *Because it's generated by the plugin, duh. (Actually, I
discovered later that you **can** get it - we'll see how soon.)*

A few words on the plugin, for context: due to some design decision I'd taken, it doesn't generate `printf` calls directly,
but instead generates code that collects the parts into a buffer (using an advancing `sprintf`, like
`pos += sprintf(buf + pos, ...)`) then prints the buffer when done.

First thing I did was to take a stack trace of the runtime crash with GDB. It was in `_IO_default_xsputn`, called in the chain
from `sprintf`. I followed the `glibc` code and crossed it with the faulting instruction, to realize that `sprintf`
tried to write to a bad pointer.

Since it wasn't deterministic (as I said, it only reproduced on some programs), I felt it has something to do with invalid use
of memory (e.g using uninitialized memory). At this point I just read the generated assembly carefully and found the following:
while the `pos += sprintf(buf + pos, ...)` looks alright, the memory location `pos` itself is never initialized to `0`!
I guess it worked on certain compilations - where, by chance/structure, the relevant stack position where `pos` is was
already zero-initialized.

Why did it happen? In the plugin code I used `DECL_INITIAL` which lets you set the initial value for a `DECL`. I took this idea
from some examples I found in the C frontend. Apparently I've been using it wrong, and it wasn't initializing anything...

Finally I [fixed it][decl_initial_fix] by using a `MODIFY_EXPR` to initialize the variable.

When I told my friend @sapir about it "bug", he replied with this:

<img src="/assets/img/runtime-compiler-errors.jpeg" width="400"/>

Great summary. Also, if someone out there manages to get this `DECL_INITIAL` working as expected, I'd be happy to hear...

By the way, a useful API that could help me here (had I known it that time) is `print_generic_expr_to_str`: this GCC function
recreates back C code from GENERIC AST! Very cool. It can help you spot problems in your generated/modified AST, because it's probably
easier to read C code compared to longish `debug_tree`s.

However, now that I'm writing this post, I wanted to try `print_generic_expr_to_str` and see if it could spot the problem for me.
But sadly, the C code it generated for the buggy code *did* include the initialization of `pos` (it was written as
`signed long D.3000 = 0;`, although the `= 0` wasn't existing in the generated assembly)
So you should keep your doubt and check the assembly if things go bad :)

### internal compiler error

Throughout plugin development, you're gonna get "internal compiler error"s quite a lot. These are GCC's way to tell you
something got messed up.
Sometimes their cause is clear: it might be a failing `gcc_assert`, reaching to `gcc_unreachable` and some other types of runtime
checks.

And other times they're like this one:
```
dummy.c:6:1: internal compiler error: Segmentation fault
```

In this case they are a nice name to "segmentation fault", or in other words: your code led to some
memory corruption / invalid access. Now, it might be in your code, and it might be somewhere deep inside GCC's code
(maybe your use of GCC's API is incorrect). GCC produces a nice traceback that includes frames of *plugin* code, but it doesn't
include frames of GCC functions.

One of the crashes I had was in a call to `build_function_call` (the last frame in the stacktrace GCC gave me was
the line in my plugin calling this function).

```
plugin_test.c:26:1: internal compiler error: Segmentation fault
   26 | }
      | ^
0x7faeb3f7a3d4 make_repr_sprintf
    /../gcc_assert_introspect/plugin.c:347
0x7faeb3f7ad57 make_conditional_expr_repr
    /../gcc_assert_introspect/plugin.c:496
...
```

*(As of commit [0633d83b])*

So the crash is somewhere in GCC... How can we debug these? What happened, and what was the full GCC stack trace?

Once again, GDB to the rescue. Now, simply running `gdb gcc ...` won't do because the `gcc` command is actually a "driver program"
that invokes other commands to do the real work of compiling/linking. The C compiler is `cc1`, and `gcc` forks then executes it.
I used [this example][debugging-gcc-hooks] which tells GDB to follow the child process after GCC forks.

Ran with it and got a full stacktrace! Using `backtrace`, top of the stack was:
```
0x00000000008d12a7 in argument_parser::check_argument_type
```

GCC was didn't have source line information (well, I built it this GCC I've been using, so blame on me):
```bash
$ addr2line -e ~/opt/gcc-9.1.0/bin/gcc 0x00000000008d12a7
??:0
```
Bummer :(

Onwards to the disassembly, then... The faulting code is:
```
   0x00000000008d1296 <+422>:   jle    0x60f878 <_ZN15argument_parser19check_argument_typeEPK16format_char_infoRK15length_modifierRP9tree_nodeRPKcbRmS8_iSA_SA_jc.part.0.cold+526>
   0x00000000008d129c <+428>:   mov    0x18(%rsp),%rcx
   0x00000000008d12a1 <+433>:   sub    $0x1,%eax
   0x00000000008d12a4 <+436>:   mov    (%rcx),%rdx
=> 0x00000000008d12a7 <+439>:   cmp    0x4(%rdx),%eax
   0x00000000008d12aa <+442>:   jae    0x60f88c <_ZN15argument_parser19check_argument_typeEPK16format_char_infoRK15length_modifierRP9tree_nodeRPKcbRmS8_iSA_SA_jc.part.0.cold+546>
```
`rdx` is dereferenced, but is was `0`.

Names are mangled and the function went modifications during LTO (that's the `.part.0.cold` suffix). So it took me a while to
find the matching C++ code - line numbers would really help. *If you build your GCC for development, make sure it has debug
info!*

That's the faulting code (from `check_format_types()` in `c-format.c`):
```c
if (EXPR_HAS_LOCATION (cur_param))
  param_loc = EXPR_LOCATION (cur_param);
else if (arglocs)
  {
    /* arg_num is 1-based.  */
    gcc_assert (types->arg_num > 0);
    param_loc = (*arglocs)[types->arg_num - 1];  // <----- CRASH IS HERE
  }
```

`argloc` is a `vec` (from `gcc/vec.h`). The faulting opcode, `cmp 0x4(%rdx), %eax`, is the bounds check before accessing
the element at a given index (the check exists in builds with runtime checks).
Thus `rdx` is `*arglocs`, which is `NULL`, while `arglocs` is not `NULL`. What?

Let's walk the calls - from `build_function_call` to this point. So `build_function_call` calls `c_build_function_call_vec`
with `vNULL` for `arg_loc` (`vNULL` is the "null" value for `vec`s). We get to `build_function_call_vec` which passes it to
`check_function_arguments` as `&arg_loc`... And this value reaches our faulting code. Of course `arglocs` is not `NULL`! It's the
address of a parameter - it can never be `NULL`. But `*arglocs` is `vNULL` and it's invalid to dereference it.

I believed this behavior is rather weird; and decided to send [a question][gcc-mail] to the GCC mailing list, explaining the
situation and asking if I did anything wrong, or this is a real problem I found here. Didn't get any reply, so I filed
[an issue][gcc-not-bug].

The issue got a reply pretty fast. One of the GCC developers said it's not a bug because I wasn't supposed to use
`build_function_call` in the first place - it's only used by the Objective-C/C++ frontends...

[My fix][gcc-not-bug-fix] was to create a wrapper for `c_build_function_call_vec` using non-`vNULL` `arg_locs`.
*Didn't use `build_call_expr`, see [here][why-not-build-call-expr] for an explanation.*

Moral of the story? Try to use stuff from `tree.h` only. GCC has no explicit distinction between "plugin API", "C frontend API"
and other pieces of code. You can trust stuff from `tree.h` to be valid in virtually all cases; if you use any APIs declared in
other files - make sure to they are also used somewhere in the same frontend you're working with: if writing something that works
with the C frontend (like the plugin described in this article), don't use functions that aren't used anywhere in that frontend,
otherwise it's not unlikely you'll bump into similar cases.

It was fun debugging, though :)

## GCC Plugin Development Resources - part 2

We'll finish, once again, with a collection of some references and tips for plugin developers.

* Compile yourself a GCC version with some debug options enabled. Specifically I can vouche for `--enable-checking` of
GCC's `configure`: This enables many types of "tree check" macros which give proper messages before doing some invalid
operation for a given type of node. Helps detecting when you misuse APIs.
e.g it can change an `internal compiler error: Segmentation fault` to something nicer
like `internal compiler error: tree check: accessed operand 6 of ne_expr with 2 operands in ...`.
I used [this script][build-gcc] to build, just make sure to add `--enable-checking` to the invocation of `configure`.
* There's `gcc:<version>` docker image in DockerHub, you can use it to easily test your plugin with different
versions of GCC; `docker run --rm -it gcc:7 /bin/bash`. (Sadly, these were not built with debugging options ^^)
* `print_generic_expr_to_str` generates back C code from a given `GENERIC` expression tree.
Useful during development - you can print it along with `debug_tree` to see how certain expressions look.
* [Debugging GCC][debugging-gcc-guide] with some examples; [Debugging hooks on cc1][debugging-gcc-hooks] in GDB.

[plugin]: https://github.com/Jongy/gcc_assert_introspect
[which-libc]: https://unix.stackexchange.com/questions/120380/what-c-library-version-does-my-system-use
[gcc-extensions-keyword]: https://gcc.gnu.org/onlinedocs/gcc/Alternate-Keywords.html
[glibc-assert-h]: https://sourceware.org/git/?p=glibc.git;a=blob;f=assert/assert.h;h=266ee92e3fb5292b3813cd6607df60b3880dbf5c;hb=HEAD#l99
[very-basic]: ../../../snippets/gcc-assert-introspect-2/very_basic.c
[iterate_function_body]: https://github.com/Jongy/gcc_assert_introspect/blob/1cfff7eeaef3caa889b6650dabfd703e71d0f048/plugin.c#L1004
[basic_rewrite]: ../../../snippets/gcc-assert-introspect-2/basic_rewrite.c
[complex_rewrite]: ../../../snippets/gcc-assert-introspect-2/complex_rewrite.c
[wrap_in_save_expr]: https://github.com/Jongy/gcc_assert_introspect/blob/1cfff7eeaef3caa889b6650dabfd703e71d0f048/plugin.c#L187
[pytest-assert-rewrite.py]: https://github.com/pytest-dev/pytest/blob/master/src/_pytest/assertion/rewrite.py
[decompiled_test]: ../../../snippets/gcc-assert-introspect-2/decompiled_test.py.txt
[runtime_rewrite]: ../../../snippets/gcc-assert-introspect-2/runtime_rewrite.c
[gcc-mail]: https://gcc.gnu.org/pipermail/gcc/2020-April/000361.html
[gcc-not-bug]: https://gcc.gnu.org/bugzilla/show_bug.cgi?id=94664
[gcc-not-bug-fix]: https://github.com/Jongy/gcc_assert_introspect/commit/22d028dab0e96eba0892058edcfdc3b7e3d34689
[why-not-build-call-expr]: https://github.com/Jongy/gcc_assert_introspect/commit/7056cf8efeecf59cce9ee58ff547b2c0270e8bef
[build-gcc]: https://github.com/darrenjs/howto/blob/master/build_scripts/build_gcc_9.sh
[debugging-gcc-hooks]: https://wiki.gentoo.org/wiki/Gcc-ICE-reporting-guide#.5Bbonus.5D_Extract_gcc_backtrace
[decl_initial_fix]: https://github.com/Jongy/gcc_assert_introspect/commit/8b96d244bc0f0559b51041fa2ae33cd146a527da
[0633d83b]: https://github.com/Jongy/gcc_assert_introspect/commit/0633d83b924446fb5d9ca36cf405dc7e626551b9
[debugging-gcc-guide]: https://dmalcolm.fedorapeople.org/gcc/newbies-guide/debugging.html
