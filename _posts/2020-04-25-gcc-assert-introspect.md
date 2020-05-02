---
layout: post
title:  "Assert Rewriting - A GCC plugin - part 1"
date:   2020-04-25 15:31:04 +0300
categories:
---

This article tells the story of how I got into developing a GCC plugin that rewrites C asserts, in attempt
to mimic `py.test`'s assert messages. It's also a beginner's guide to GCC plugin development.

The plugin itself is available [here][gcc-assert-introspect].

## Poor man's asserts

A few weeks ago I started working on a small side project in C, nothing too fancy. As with all new
codebases, it was prone to many structural changes, API refactors etc. So I tried to add
as many asserts as possible, and also wrote some basic C tests. *When writing lots
of new code, I try to have tests written ASAP, to and fill the code with asserts since they help in quickly
catching problems common to a developing codebase (like missed refactors / renames, misused APIs, ...).*

One thing I dislike about those asserts it that watching them fail usually doesn't provide you with
all the information necessary to fix the problem. A common assert like:
```c
assert(n == 7);
```

will fail with:
```
main: main.c:6: main: Assertion `n == 7' failed.
```
telling you that `n` wasn't `7`, but not telling you **what** was the value `n`, which
might be required to identify the reason of failure.

The problem gets worse when you have more complex expressions, like `&&`, `||` or function calls.
So when an assert fails, I usually have to replace it with a `printf` of all expressions, recompile and rerun the failing
code just so I could know what the problem was.

Back in that side project, after a session of about 10 "replace-assert-with-printf-then-rerun" changes,
I thought there must be something I could do to improve it.

Then `py.test` popped into my mind. `py.test` is one the well-known Python test frameworks, and one of
the features I like in it is that you write your tests logic using plain `assert`s, but unlike the standard
Python `assert` failure (which raises an empty `AssertionError`), `py.test` prints a very elaborated
assert failure message. For example, the following assert:
```python
assert min([1, 2, 3]) - 5 + min(1, 2, 3) == max([5, 6, 5])
```

will fail with this exception:

    >       assert min([1, 2, 3]) - 5 + min(1, 2, 3) == max([5, 6, 5])
    E       assert ((1 - 5) + 1) == 6
    E        +  where 1 = min([1, 2, 3])
    E        +  and   1 = min(1, 2, 3)
    E        +  and   6 = max([5, 6, 5])

That's very cool. It would have been very useful if I had something similar in C. I decided to try and
build that something, for my C code.

## Adding introspection to asserts

I searched online and found [this][pytest-assert] read, which gives a brief introduction on assert rewriting
in `py.test`. The author (who wrote the feature in `py.test`) explains how subexpression information
can be displayed by modifying the AST (Abstract Syntax Tree) of your test code.

*If you're not sure what's AST, [this][ast-image] image from Wikipedia will help. It shows the AST of
a simple piece of code, written in its description. You can also take a look in [AST explorer][ast-explorer]
which provides online AST parsing of many languages (doesn't have C though)*

Python, being a dynamic language, allows you to modify the AST of modules during the loading process.
`py.test` uses this to dismantle your `assert`s into smaller code pieces that are executed one by one. If
any of them fails, the generated code is ready to print information about the exact point of failure.

Let's see a simple, manual "assert rewriting" example. To gain introspection into:

```python
assert n == 8 and myfunc() == 12
```

we can rewrite it as:

```python
if n == 8:
    _tmp = myfunc()
    if _tmp == 12:
        # all good! the assert passed
        pass
    else:
        raise AssertionError("{0} != 12 (myfunc() = {0})".format(_tmp))
else:
    raise AssertionError("{0} != 8 (n = {0})".format(n))
```

As you can see, even for simple expressions it quickly becomes complex. For example, if any of the subexpressions
has side-effects, it must be stored in a temporary variable (like `_tmp`) so we don't invoke
those side-effects multiple times. In the real world it's gets even more complicated ;)

Lucky for us, this conversion is entirely automatic!
`py.test` takes care of everything, and we get to write human-readable assert expressions.

# Back to C

C is not a dynamic language. I can't modify the AST of my program code in runtime. The compiler leaves
you with machine instructions, it's hard to extract accurate information about the original source code
by messing with them in runtime.

Test frameworks in C need to provide useful failure message when test asserts fail to help you with
debugging your tests, so they must do something more fancy than plain C `assert`s.

What's common for them is to provide you a set of assert functions, suitable for all comparison
types you may need.
For example, the library [Check][check] has the following set of functions/macros (excerpt from
[src/check.in.h][check-h])

```c
...
#define     ck_assert_int_eq(X, Y)   _ck_assert_int(X, ==, Y)
...
#define     ck_assert_int_ne(X, Y)   _ck_assert_int(X, !=, Y)
...
#define     ck_assert_int_lt(X, Y)   _ck_assert_int(X, <, Y)
...
#define     ck_assert_int_le(X, Y)   _ck_assert_int(X, <=, Y)
...
```

As you can see, you don't write the expression `X == Y` but instead use `ck_assert_int_eq` with the arguments
separated. `_ck_assert_int` can use the separated arguments to create a proper assert message:
```c
#define _ck_assert_int(X, OP, Y) do { \
  intmax_t _ck_x = (X); \
  intmax_t _ck_y = (Y); \
  ck_assert_msg(_ck_x OP _ck_y, "Assertion '%s' failed: %s == %jd, %s == %jd", #X" "#OP" "#Y, #X, _ck_x, #Y, _ck_y); \
} while (0)
```

This produces useful messages, but the downside is that you have to remember all the `ck_assert_*` functions (the
[list][check-api] is long), and write unintuitive code instead of a simple `==`.
If `ck_assert_int_eq` would accept the expression `X == Y` instead, it won't have any introspection on the
expression - it could only operate on the **value** of it.

This "problem" isn't affecting Check only - in C, and in most other compiled languages, you don't have access to
the information required to parse your code's own expression trees. You'll need to be a part of the compilation process in
order to get your hands on the AST. To be a part of the compilation process means, in most compiled languages,
that you are required to be a part of the compiler, or a plugin of it.

*Actually, Rust is the only language I know that lets **your** code intervene in it's own compilation process,
via procedural macros.*

*Update: Reddit comments mentioned [Catch2][catch2], a C++ test framework which (among others) exploits
operator precedence and overloading. Ultimately you have a macro `REQUIRE(x == 5)` and Catch2 can "parse" this
expression to yeild similar results with respect to what I'm trying to achieve here.
Not exactly AST parsing, but nonetheless it's very cool so I thought it's worth to mention here.*

We're in C, so... a GCC plugin it is.

![](/assets/img/plugin-vs-macros.jpeg?raw=true "And thanks to @sapir for that")

## GCC plugins intro

GCC provides [an API for plugins][plugins-api]. Plugins are programs built once, then loaded when invoking GCC. The can
modify its behvaior, add new features, etc. More specifically, your plugin can register callbacks which GCC
will call in different steps during compilation, allowing the plugin to mess around with GCC's internal objects.

The GCC wiki has a [short page][gcc-plugins] with some background on plugins. It also lists some existing
plugins, although the list isn't so long.
To get the hang of what plugins are capable of, here's a brief on one of the GCC plugins the Linux kernel uses
for its own build: named [randstruct], this plugin randomizes the layout of structs during the
kernel build process, so different kernel builds use different layout for their internal structs.
This is an obfuscation, done to help in exploit mitigation. No way you could do that without a plugin -
you'd have to change your code manually.

# Into GCC

Here's what I had in mind: I'll write a plugin that "walks" over functions' code, identifies calls
to `assert` and rewrites them with code that'll do all those nice printings. I didn't stop to think
how exactly this "code" is gonna work... I didn't even bother looking in `py.test`'s implementation,
though I wanted the plugin to produce similar messages.

So, GCC plugins. I had some experience writing those; a few months ago I wrote [struct_layout]. That plugin
processes struct definitions as GCC encounters them, then dumps them in a specialized format.

I started by copying the plugin boilerplate from it. A plugin is basically a shared object which exports a
`plugin_init` function. GCC calls it when the plugin is loaded, and your plugin can register callbacks for
various compilation events using `register_callback`.

[struct_layout] registered on the event `PLUGIN_FINISH_DECL`, which is generated *after finishing parsing a declaration*.
This time we need an event related to code parsing & AST. A quick look in [plugin.def] yields `PLUGIN_PRE_GENERICIZE`
which is documented as *allows to see low level AST in C and C++ frontends*. Perfect!

The required prototype for the callback is `void my_callback(void *event_data, void *user_data)`.
`user_data` is a "private data" pointer (we can pass to `register_callback` and get it here),
and `event_data` is the data itself: a `tree` object.

# GCC trees

From the [GCC docs][gcc-docs-tree]:

    The central data structure used by the internal representation is the tree.

Short enough, but it says a lot. Trees are a very central data structure, literally everything is represented by
a tree object, and those can wield different types and properties. That "internal representation", made of
trees, creates the AST.

*This article talks about the GCC internal representation called GENERIC. There's another internal representation
in GCC, named GIMPLE, but we won't cover it here.*

The AST is composed of multiple nodes; They can represent statements, declarations, types, ... you get my point.
We're interested in reading functions. From my previous work on plugins, I remembered there's a type of tree for
representing functions: it's called `FUNCTION_DECL`.

At this point I didn't know what's inside this `FUNCTION_DECL` tree. Plugin development guides are
scarce, but after a bit of searching I found a nice guide giving [concrete examples of AST nodes in GCC][gcc-ast].
I suggest you take a quick stop and read it (it's short). At least take a look at the fancy diagrams.

*Another useful document is [GCC GENERIC docs][gcc-generic] where you can find a short
paragraph on many (if not all?) tree types. I keept it open in one tab for the course of developemnt.*

# Plugin Basics

I think we know enough to start, let's get some code running. Start by setting up your environment for plugin development.
Some distros require the installation of a "GCC plugin dev" package (e.g Ubuntu), in others you get it in your
standard GCC installation (e.g Arch)

For Ubuntu:
```bash
# You'll need gcc-X-plugin-dev, where X is your GCC major. check it with gcc --version.
# For Ubuntu 18.04 that will be:
$ sudo apt-get install gcc-7-plugin-dev
```

Let's compile the empty plugin skeleton I described above. A minimal file is:
```c
#include <stdio.h>

#include <gcc-plugin.h>
#include <tree.h>
#include <plugin-version.h>

int plugin_is_GPL_compatible; // must be defined & exported for the plugin to be loaded

static void pre_genericize_callback(void *event_data, void *user_data) {
    tree t = (tree)event_data;

    // ...
}

int plugin_init(struct plugin_name_args *plugin_info, struct plugin_gcc_version *version) {
    printf("Loaded! compiled for GCC %s\n", gcc_version.basever);
    register_callback(plugin_info->base_name, PLUGIN_PRE_GENERICIZE, pre_genericize_callback, NULL);

    return 0;
}
```

Write it to `basic.c` and build it:
```bash
$ g++ -g -I`gcc -print-file-name=plugin`/include -fpic -shared -o basic.so basic.c
```

The `gcc -print-file-name=plugin` prints the location where plugin headers are installed. We also compile
with `-fpic -shared` to produce a shared library.

Now we'll try to load it.
```bash
$ touch empty.c
$ gcc -fplugin=./basic.so empty.c -c
Loaded! compiled for GCC 9.1.0
```

Great. Now what do we do with that tree we have in our callback? It's time to meet the single-handedly most useful
debugging function you can use while developing for GCC: `debug_tree`. It accepts a tree object and dumps it
recursively (to some depth), printing its type, its fields and descendants.

Add `#include <print-tree.h>` to the top, and change our callback function to this:
```c
static void pre_genericize_callback(void *event_data, void *user_data) {
    tree t = (tree)event_data;

    debug_tree(t);
}
```

We'll write a dummy function and compile it with the plugin active. `-fplugin=/path/to/plugin.so` tells GCC to load
our plugin.
```bash
# start by recompiling the plugin
$ g++ -g -I`gcc -print-file-name=plugin`/include -fpic -shared -o basic.so basic.c
$ echo 'int dummy(int n) { return n + 5; }' > dummy.c
$ gcc -fplugin=./basic.so dummy.c -c
```

You should see something like::

     <function_decl 0x7f13df35be00 dummy
        type <function_type 0x7f13df263bd0
            type <integer_type 0x7f13df2525e8 int public SI
    ...

This is the AST node representing our function. A few words on the format:
* Nodes (trees) begin with a `<` and end with a `>`.
* Node type follows the `<`; Here we have `function_decl`, `function_type` and `integer_type`. Those types are themselves
  trees.
* After the node type comes the node object addres in memory. It doesn't convery much info, but it's useful
  when comparing nodes because it tells you when two nodes are really equal (because they are the same object).
* After the address comes an optional string describing the node. For declarations, it's the name (our `function_decl`
  is named `dummy`). For types (such as `integer_type`) it's the type name (here `int`).
* The string before an opening `<` is the "name" of the field in the containing node. Here our
  `function_decl` has `type` of `function_type`, and `function_type` has `type` of `integer_type`.

# Functions in the AST

I don't see anything in the previous print about the function code, though! One of the diagrams in
[AST representation in GCC][gcc-ast] will be of help. It shows functions have a `DECL_SAVED_TREE` field, and gives usage
example:
```c
tree fnBody = DECL_SAVED_TREE (fnDecl);
```

The body is a tree itself, of course. So we can alter our `debug_tree(t)` to `debug_tree(DECL_SAVED_TREE(t))`.

Recompile the plugin, the recompile `dummy.c` with the plugin:

     <bind_expr 0x7fab614c3a80
        type <void_type 0x7fab6139bf18 void VOID
            align 8 symtab 0 alias set -1 canonical type 0x7fab6139bf18
            pointer_to_this <pointer_type 0x7fab613a30a8>>
        side-effects
        body <return_expr 0x7fab614b1d40 type <void_type 0x7fab6139bf18 void>
            side-effects
            arg 0 <modify_expr 0x7fab614a0b90 type <integer_type 0x7fab6139b5e8 int>
                side-effects arg 0 <result_decl 0x7fab613911e0 D.1794>
                arg 1 <plus_expr 0x7fab614a0b68 type <integer_type 0x7fab6139b5e8 int>
                    arg 0 <parm_decl 0x7fab614c4000 n>
                    arg 1 <integer_cst 0x7fab613a03f0 constant 5>
                    dummy.c:1:29 start: dummy.c:1:27 finish: dummy.c:1:31>
                dummy.c:1:29 start: dummy.c:1:27 finish: dummy.c:1:31>
            dummy.c:1:29 start: dummy.c:1:27 finish: dummy.c:1:31>
        block <block 0x7fab614a97e0 used
            supercontext <function_decl 0x7fab614a4e00 dummy type <function_type 0x7fab613acbd0>
                public static QI file dummy.c line 1 col 5 align 8 initial <block 0x7fab614a97e0> result <result_decl 0x7fab613911e0 D.1794> arguments <parm_decl 0x7fab614c4000 n>
                struct-function 0x7fab614c5000>>
        dummy.c:1:18 start: dummy.c:1:18 finish: dummy.c:1:18>

Lots of prints. Don't get scared though, read and "parse" it slowly because everything here is logical.
The function body is a `bind_expr`. What's that? Head to the [GENERIC docs][gcc-generic] and under Statements -> Blocks
you'll find a short explanation on `BIND_EXPR`:

    Block scopes and the variables they declare in GENERIC are expressed using the BIND_EXPR code, ...

So a `BIND_EXPR` is just... Code and variables enclosed in `{ }`. Very simple.

*Note: `debug_tree` prints type names like `bind_expr` and field names like `body` in lowercase, but in the source code
these are written in uppercase. From now on, I'll be using the source notation, uppercase.*

We can already see what the body contains, but let's try getting the inner body tree first. Most tree types have special
macros to access their fields; those can be found in `gcc/tree.h`. Time to grab a copy of the source!

*When working with the source code of a project, I like having its Git available, so I can look up in the history,
read commit messages, find out when a change was included into the mainline, etc. So I suggest you to clone GCC's code:
`git clone https://github.com/gcc-mirror/gcc.git`. You can also get the latest version only if you're hasty:
`git clone --depth 1 https://github.com/gcc-mirror/gcc.git`*

*By the way, we already have a local copy of `tree.h` and other API headers; We got them when we installed the
plugin dev package. Nonetheless, having the full source code (including the implementation of things) is useful.*

Open `gcc/tree.h` and search for "bind". If possible, when searching for symbols, use symbolic search in this file since
it's utterly huge. I use Sublime, so Ctrl+R "bind" led me to`BIND_EXPR_VARS`, `BIND_EXPR_BODY` and `BIND_EXPR_BLOCKS`.

`BIND_EXPR_BODY` sounds like it. Let's `debug_tree` it: `debug_tree(BIND_EXPR_BODY(DECL_SAVED_TREE(t)))`.

     <return_expr 0x7fc48ba6fd40
        type <void_type 0x7fc48b959f18 void VOID
            align 8 symtab 0 alias set -1 canonical type 0x7fc48b959f18
            pointer_to_this <pointer_type 0x7fc48b9610a8>>
        side-effects
        arg 0 <modify_expr 0x7fc48ba5eb90
            type <integer_type 0x7fc48b9595e8 int public SI
                size <integer_cst 0x7fc48b941f18 constant 32>
                unit size <integer_cst 0x7fc48b941f30 constant 4>
                align 32 symtab 0 alias set -1 canonical type 0x7fc48b9595e8 precision 32 min <integer_cst 0x7fc48b941ed0 -2147483648> max <integer_cst 0x7fc48b941ee8 2147483647>
                pointer_to_this <pointer_type 0x7fc48b961a80>>
            side-effects
            arg 0 <result_decl 0x7fc48b94f1e0 D.1794 type <integer_type 0x7fc48b9595e8 int>
                ignored SI file dummy.c line 1 col 5 size <integer_cst 0x7fc48b941f18 32> unit size <integer_cst 0x7fc48b941f30 4>
                align 32 context <function_decl 0x7fc48ba62e00 dummy>>
            arg 1 <plus_expr 0x7fc48ba5eb68 type <integer_type 0x7fc48b9595e8 int>
                arg 0 <parm_decl 0x7fc48ba82000 n>
                arg 1 <integer_cst 0x7fc48b95e3f0 constant 5>
                dummy.c:1:29 start: dummy.c:1:27 finish: dummy.c:1:31>
            dummy.c:1:29 start: dummy.c:1:27 finish: dummy.c:1:31>
        dummy.c:1:29 start: dummy.c:1:27 finish: dummy.c:1:31>

Walk it top down:
* `RETURN_EXPR` is explained in the GENERIC docs, Statements -> Basic Statements. Its first operand is the value to
  return, in our case a `MODIFY_EXPR`.
* `MODIFY_EXPR` is explained under Expressions -> Unary and Binary Expressions: A `MODIFY_EXPR`, simply, is an assignment
  (includes assignments like `+=` as well as `=`). Here its first operand is `RESULT_DECL`, which is a tree representing the
  return value of the current function - a value assigned to `RESULT_DECL` indicates it should be returned.
* The second operand of the `MODIFY_EXPR` is a `PLUS_EXPR`: a simple `+` between its 2 operands.
* The operands to our `PLUS_EXPR` are a `PARM_DECL` (represents a parameter to a function) and an `INTEGER_CST` which
  represents integer cosntants. The details displayed by `debug_tree` show us the `PARM_DECL` is named `n`, and the
  integer constant is `5`.

Okay, we know how to read and parse out simple AST nodes! Now comes the question - is it possible to overwrite them
during compilation? *(Well, ealier I did say it's possible... but let's see how)*

# AST modifications

To replace a node with another one, we'll need to create a new node first. Functions constructing new trees will begin with
`build`, and `tree.h` has plenty of them. Most are not documented in `tree.h` itself but instead
in their definition. My suggestion is that you try searching `tree.h` for keywords of what you need, it works
quite well.

For our modification test - let's modify the AST to make `dummy` return `n + 10` instead of `n + 5`. So we need to replace
the `INTEGER_CST` with another one, holding the value of 10.

Symbolic searching for "build int" yields `build_int_cst`. It accepts a "type" tree, and a value for the new const.
The "type" may be the tree representing the C type `int`, the C type `long`, etc. All those common types are declared in
`tree.h`. We will use `integer_type_node` for a plain `int`.

In `tree.h` we'll also find `TREE_OPERAND(NODE, I)`, which accesses the I'th operand of a node. This way we can walk
down the expression tree until we get to the node we wish to replace.

```c
static void pre_genericize_callback(void *event_data, void *user_data) {
    tree t = (tree)event_data;

    tree body = BIND_EXPR_BODY(DECL_SAVED_TREE(t));
    tree modify = TREE_OPERAND(body, 0);
    tree plus = TREE_OPERAND(modify, 1);
    // replace it! (TREE_OPERAND's result is an lvalue)
    TREE_OPERAND(plus, 1) = build_int_cst(integer_type_node, 10);
}
```

Recompile the plugin, recompile `dummy.c` with the plugin, and write another simple file `main.c`:
```c
#include <stdio.h>

int dummy(int n);

int main(void) {
    printf("%d\n", dummy(1));
    return 0;
}
```

Build (this time without the plugin):
```bash
$ gcc main.c dummy.o -o main  # note: use the dummy.o compiled with the plugin, not dummy.c!
```

And...
```bash
$ ./main
11
```

Worked! Hooray.

You can compile `dummy.c` again without the plugin to see the difference:

```bash
$ gcc main.c dummy.c -o main  # dummy.c this time
$ ./main
6
```

And that concludes it for this post.

I actually intended to focus on the assert rewriting part, but ended up talking about
GCC for the most part. I'm happy it came out this way, though - by this guide I wish to provide smoother entry to
plugin development for everyone. During my career as a developer I have spent many hours reading GCC's user docs, searching
for the right compiler flag for a task, etc. Gaining insight in the internals, being able to extend C & GCC with features I
couldn't have earlier - well, it was very nice, and I think every developer should try it sometime :)

In the next part, we'll talk about the assert rewriting logic itself, and how to embed it in the AST via the
plugin. We'll also dive deeper into GCC and GENERIC representation.

## GCC Plugin Development Resources

I collected here the references I gave through the article. Many we useful for me during this
project, some I found only afterwards. Anyway, all are useful and thus I put them here together so future plugin
developers can use them.

1. [AST representation in GCC][gcc-ast] - AST in GCC trees.
2. [GCC GENERIC trees][gcc-generic] - the official docs for tree types.
3. [Understanding Trees][gcc-understanding-trees] - in GCC wiki, short but contains useful examples.
4. [Playing with GCC plugins][playing-with-gcc-plugins] - simple plugin that logs structs' sizes. Insipired me to
   try plugins.
5. [A simple plugin for GCC][gcc-plugin-guide] - implements the `warn_unused_result` attribute
   correctly for C++. That guide works with GIMPLE, not with GENERIC as I did here, but some parts are common and
   nonetheless it's an interesting read.
6. [GCC tiny][gcc-tiny], by the same author of the previous entry - writing a new GCC front end for a simple language.
   Based on GENERIC. Very comprehensive.
7. This guide itself :) And its future parts, in days to come...

[gcc-assert-introspect]: https://github.com/Jongy/gcc_assert_introspect
[pytest-assert]: https://pybites.blogspot.com/2011/07/behind-scenes-of-pytests-new-assertion.html
[ast-image]: https://en.wikipedia.org/wiki/Abstract_syntax_tree#/media/File:Abstract_syntax_tree_for_Euclidean_algorithm.svg
[check]: https://libcheck.github.io/check/
[check-h]: https://github.com/libcheck/check/blob/master/src/check.h.in#L576
[check-api]: https://libcheck.github.io/check/doc/doxygen/html/check_8h.html
[plugins-api]: https://gcc.gnu.org/onlinedocs/gccint/Plugin-API.html#Plugin-API
[gcc-plugins]: https://gcc.gnu.org/wiki/plugins
[randstruct]: https://lwn.net/Articles/722293/
[struct_layout]: https://github.com/Jongy/struct_layout
[plugin.def]: https://github.com/gcc-mirror/gcc/blob/master/gcc/plugin.def
[gcc-plugin-guide]: https://thinkingeek.com/2015/08/16/a-simple-plugin-for-gcc-part-1/
[gcc-docs-tree]: https://gcc.gnu.org/onlinedocs/gccint/Tree-overview.html
[gcc-ast]: http://icps.u-strasbg.fr/~pop/gcc-ast.html
[gcc-generic]: https://gcc.gnu.org/onlinedocs/gccint/GENERIC.html
[gcc-understanding-trees]: https://gcc.gnu.org/wiki/GenericAPI
[gcc-tiny]: https://thinkingeek.com/gcc-tiny/
[playing-with-gcc-plugins]: https://rwmj.wordpress.com/2016/02/24/playing-with-gcc-plugins/
[ast-explorer]: https://astexplorer.net/
[catch2]: https://github.com/catchorg/Catch2
