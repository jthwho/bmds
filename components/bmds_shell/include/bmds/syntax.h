/**
 * @file      syntax.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The shell's syntax tree, as produced by `Parser`.
 *
 * Every object lives in the parser's `Arena` and is trivially destructible;
 * the tree is freed with the arena. Lists are singly linked through `next`
 * (each object is in at most one list). Text (`StringView`) points into the
 * parsed source, which must outlive the tree.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/string.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

struct Word;

/** @brief What a `${...}` expansion does with its parameter. */
enum class ParamOp : uint8_t {
        None,                 ///< `$x`, `${x}`
        Length,               ///< `${#x}`
        Default,              ///< `${x-w}`, `${x:-w}`
        Assign,               ///< `${x=w}`, `${x:=w}`
        Error,                ///< `${x?w}`, `${x:?w}`
        Alternative,          ///< `${x+w}`, `${x:+w}`
        RemoveSmallestPrefix, ///< `${x#p}`
        RemoveLargestPrefix,  ///< `${x##p}`
        RemoveSmallestSuffix, ///< `${x%p}`
        RemoveLargestSuffix,  ///< `${x%%p}`
};

/**
 * @brief One piece of a word.
 *
 * Words keep their quoting until expansion: `quoted` marks text from
 * quotes or a backslash (never split, never a pattern), and an expansion
 * inside double quotes (its result is not split).
 */
struct WordPart {
                /** @brief Kind of piece. */
                enum class Kind : uint8_t {
                        Literal,             ///< `text` as is.
                        Parameter,           ///< `$name` / `${...}`: `text` is the name, `op`/`arg` the rest.
                        CommandSubstitution, ///< `$(...)` / backquotes: `text` is the command's source.
                        Arithmetic,          ///< `$((...))`: `text` is the expression's source.
                };

                Kind       kind = Kind::Literal;
                bool       quoted = false;     ///< Quoted text, or an expansion inside double quotes.
                bool       colon = false;      ///< `Default`..`Alternative`: written with `:` (null counts as unset).
                ParamOp    op = ParamOp::None; ///< `Parameter`: the operation.
                StringView text;               ///< See `Kind`.
                Word      *arg = nullptr;      ///< `Parameter` with an operation that takes a word.
                WordPart  *next = nullptr;
};

/** @brief A word: a sequence of parts that expands to zero or more fields. */
struct Word {
                WordPart  *parts = nullptr;
                Word      *next = nullptr;
                bool       hasQuotes = false; ///< Contains quotes, so it is a word even when empty (`""`).
                StringView source;            ///< As written, for messages (empty for words made up by the parser).
};

/** @brief `name=value` before a command. */
struct Assignment {
                StringView  name;
                Word       *value = nullptr; ///< Never null; may have no parts (empty).
                Assignment *next = nullptr;
};

/** @brief A redirection as written. */
struct Redirection {
                /** @brief The operator. */
                enum class Kind : uint8_t {
                        Input,       ///< `<`
                        Output,      ///< `>`
                        Append,      ///< `>>`
                        Clobber,     ///< `>|`
                        InputOutput, ///< `<>`
                        DupInput,    ///< `<&`
                        DupOutput,   ///< `>&`
                        OutputBoth,  ///< `&>`
                        AppendBoth,  ///< `&>>`
                        HereDoc,     ///< `<<`, `<<-`
                        HereString,  ///< `<<<`
                };

                Kind         kind = Kind::Output;
                int          fd = -1;           ///< The descriptor written before the operator, or -1 (default).
                bool         stripTabs = false; ///< `<<-`
                Word        *target = nullptr;  ///< File, descriptor (`&`), string, or here-doc delimiter.
                Word        *body = nullptr;    ///< `HereDoc`: the body (quoted literal if the delimiter was quoted).
                Redirection *next = nullptr;
};

/** @brief Kind of syntax tree node. */
enum class NodeKind : uint8_t {
        Simple,     ///< `SimpleNode`
        Pipeline,   ///< `PipelineNode`
        AndOr,      ///< `AndOrNode`
        List,       ///< `ListNode`
        If,         ///< `IfNode`
        While,      ///< `LoopNode`
        Until,      ///< `LoopNode`
        For,        ///< `ForNode`
        ArithFor,   ///< `ArithForNode`
        Case,       ///< `CaseNode`
        Group,      ///< `BodyNode`: `{ ...; }`
        Subshell,   ///< `BodyNode`: `( ... )`
        Function,   ///< `FunctionNode`
        Arithmetic, ///< `ArithNode`: `(( ... ))`
        Background, ///< `BackgroundNode`: `... &`
};

/**
 * @brief Base of every node. Downcast with `static_cast` after checking `kind`.
 */
struct Node {
                NodeKind     kind;
                Redirection *redirects = nullptr; ///< A command's redirections (compound commands too).
                Node        *next = nullptr;      ///< Next in a pipeline or list.

                explicit Node(NodeKind k) : kind(k) {}
};

/** @brief `a=1 cmd arg... >file` */
struct SimpleNode : Node {
                Assignment *assignments = nullptr;
                Word       *words = nullptr; ///< May be null (assignments or redirections only).
                SimpleNode() : Node(NodeKind::Simple) {}
};

/**
 * @brief `[!] a | b | c` (also a lone `! a`). `|&` adds `2>&1` to the left command.
 *
 * `source` is the commands' text (without `!`), which a job-control shell
 * runs as a subshell process (devplan/processes.md §3.7.1).
 */
struct PipelineNode : Node {
                Node      *commands = nullptr;
                StringView source; ///< The commands as written.
                bool       negated = false;
                bool       hasHereDoc = false; ///< A command has a here-document (its body may be outside `source`).
                PipelineNode() : Node(NodeKind::Pipeline) {}
};

/** @brief `a && b`, `a || b` (left-associative). */
struct AndOrNode : Node {
                Node *left = nullptr;
                Node *right = nullptr;
                bool  isAnd = true;
                AndOrNode() : Node(NodeKind::AndOr) {}
};

/** @brief `a; b; c` (or newline-separated, in a compound command). */
struct ListNode : Node {
                Node *items = nullptr;
                ListNode() : Node(NodeKind::List) {}
};

/** @brief `if c; then t; [elif ...;] [else e;] fi` (`elif` is a nested IfNode in `elsePart`). */
struct IfNode : Node {
                Node *condition = nullptr;
                Node *thenPart = nullptr;
                Node *elsePart = nullptr;
                IfNode() : Node(NodeKind::If) {}
};

/** @brief `while c; do b; done`, `until c; do b; done` */
struct LoopNode : Node {
                Node *condition = nullptr;
                Node *body = nullptr;
                explicit LoopNode(NodeKind k) : Node(k) {}
};

/** @brief `for x [in w...]; do b; done` */
struct ForNode : Node {
                StringView variable;
                Word      *words = nullptr;
                bool       hasIn = false; ///< Without `in`, the loop runs over `"$@"`.
                Node      *body = nullptr;
                ForNode() : Node(NodeKind::For) {}
};

/** @brief `for ((init; condition; step)); do b; done` (each part may be empty). */
struct ArithForNode : Node {
                StringView init;
                StringView condition;
                StringView step;
                Node      *body = nullptr;
                ArithForNode() : Node(NodeKind::ArithFor) {}
};

/** @brief One `pattern|pattern) body ;;` of a case. */
struct CaseItem {
                Word     *patterns = nullptr;
                Node     *body = nullptr; ///< May be null (empty).
                CaseItem *next = nullptr;
};

/** @brief `case w in items esac` */
struct CaseNode : Node {
                Word     *subject = nullptr;
                CaseItem *items = nullptr;
                CaseNode() : Node(NodeKind::Case) {}
};

/** @brief `{ body; }` or `( body )` */
struct BodyNode : Node {
                Node *body = nullptr;
                explicit BodyNode(NodeKind k) : Node(k) {}
};

/**
 * @brief `name() body`, `function name body`
 *
 * Functions are kept as source text (`source`: the body and its
 * redirections) and parsed again when called, so the definition outlives
 * the arena the tree was parsed into.
 */
struct FunctionNode : Node {
                StringView name;
                Node      *body = nullptr;
                StringView source;
                FunctionNode() : Node(NodeKind::Function) {}
};

/**
 * @brief `body &`: an and-or list run as a job, not waited for.
 *
 * `source` is its text, which a subshell process parses again (the tree
 * goes with the parser's arena).
 */
struct BackgroundNode : Node {
                Node      *body = nullptr;
                StringView source;               ///< The and-or list as written.
                bool       hereDocAfter = false; ///< A here-document's body follows the line (not in `source`).
                BackgroundNode() : Node(NodeKind::Background) {}
};

/** @brief `(( expression ))` */
struct ArithNode : Node {
                StringView expression;
                ArithNode() : Node(NodeKind::Arithmetic) {}
};

/**
 * @brief Appends a compact, unambiguous rendering of @p node to @p out.
 *
 * For tests and debugging. Words render as written, with `'…'` for quoted
 * literals and `"…"` around quoted expansions; commands as S-expressions,
 * e.g. `(&& (cmd test -n x) (cmd echo yes (> /dev/null)))`.
 *
 * @return `Ok` or `NoMem`.
 */
Error dumpSyntax(const Node *node, String &out);

/** @brief Appends a rendering of @p word (see `dumpSyntax()`). */
Error dumpWord(const Word *word, String &out);

BMDS_NAMESPACE_END
