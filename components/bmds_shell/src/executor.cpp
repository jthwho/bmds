/**
 * @file      executor.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The shell language interpreter: expansion, commands, pipelines,
 * compound commands, functions and redirections.
 */

#include <cstdio>
#include <cstring>
#include <new>
#include <bmds/arith.h>
#include <bmds/console.h>
#include <bmds/executor.h>
#include <bmds/glob.h>
#include <bmds/memory.h>
#include <bmds/memorystream.h>
#include <bmds/parser.h>
#include <bmds/pipe.h>
#include <bmds/shell.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr StringView ShellName = "bmds";
        constexpr StringView DefaultIfs = " \t\n";

        // Stack the executor's callers use above the point where it starts
        // measuring (the shell's read loop and line editor).
        constexpr size_t CallerStackBytes = 2048;

        bool isDigits(StringView s) {
                if (s.isEmpty()) return false;
                for (char c : s) {
                        if (c < '0' || c > '9') return false;
                }
                return true;
        }

        bool isName(StringView s) {
                if (s.isEmpty()) return false;
                for (size_t i = 0; i < s.size(); ++i) {
                        const char c = s[i];
                        const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
                        if (!alpha && !(i > 0 && c >= '0' && c <= '9')) return false;
                }
                return true;
        }

        // A here-document or here-string: a read-only stream over its text that
        // frees itself when its last handle closes.
        class TextStream final : public Stream {
                public:
                        static Result<FileHandle::Ptr> open(String text) {
                                void *mem = Memory::alloc(MemSpace::Psram, sizeof(TextStream));
                                if (mem == nullptr) return makeError<FileHandle::Ptr>(Error::NoMem);
                                TextStream *s = new (mem) TextStream(std::move(text));
                                auto [handle, err] = FileHandle::open(*s, FileHandle::Read);
                                if (err.isError()) s->destroy();
                                return Result<FileHandle::Ptr>(std::move(handle), err);
                        }

                        Result<size_t> read(WritableByteSpan buf, unsigned int) override {
                                const size_t left = _text.size() - _pos;
                                if (left == 0) return makeError<size_t>(Error::EndOfFile);
                                const size_t n = buf.size() < left ? buf.size() : left;
                                std::memcpy(buf.data(), _text.data() + _pos, n);
                                _pos += n;
                                return makeResult(n);
                        }

                        Result<size_t> write(ByteSpan, unsigned int) override {
                                return makeError<size_t>(Error::BadDescriptor);
                        }

                        void handleClosed(uint8_t) override {
                                destroy();
                                return;
                        }

                private:
                        explicit TextStream(String text) : _text(std::move(text)) {}
                        ~TextStream() = default;

                        void destroy() {
                                this->~TextStream();
                                Memory::free(this);
                                return;
                        }

                        String _text;
                        size_t _pos = 0;
        };

        bool isGlobMagic(char c) {
                return c == '*' || c == '?' || c == '[';
        }

        bool isGlobSpecial(char c) {
                return isGlobMagic(c) || c == ']' || c == '\\';
        }

        // One pipeline stage being run.
        struct StageRun {
                        Executor           *parent = nullptr;
                        const Node         *node = nullptr;
                        FdTable             fds;
                        ShellState          state; // workers: the copy they run on
                        int                 status = 0;
                        bool                runnable = true;
                        WorkerPool::Worker *worker = nullptr;
        };

} // namespace

void appendShellQuoted(String &out, StringView value) {
        bool plain = !value.isEmpty();
        for (char c : value) {
                const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                                StringView("_./:@%+,=-").contains(c);
                plain = plain && ok;
        }
        if (plain) {
                (void)out.append(value);
                return;
        }
        (void)out.append('\'');
        for (char c : value) {
                if (c == '\'') {
                        (void)out.append("'\\''");
                } else {
                        (void)out.append(c);
                }
        }
        (void)out.append('\'');
        return;
}

// ---------------------------------------------------------------------------
// ShellState
// ---------------------------------------------------------------------------

Result<ShellState> ShellState::clone() const {
        ShellState copy;
        auto [varsCopy, err] = vars.clone();
        copy.vars = std::move(varsCopy);
        if (err.isOk()) err = copy.functions.reserve(functions.size());
        for (size_t i = 0; err.isOk() && i < functions.size(); ++i) {
                ShellFunction f;
                err = f.name.assign(functions[i].name);
                if (err.isOk()) err = f.source.assign(functions[i].source);
                if (err.isOk()) err = copy.functions.append(std::move(f));
        }
        if (err.isOk()) err = copy.positional.reserve(positional.size());
        for (size_t i = 0; err.isOk() && i < positional.size(); ++i) {
                auto [p, perr] = positional[i].clone();
                err = perr;
                if (err.isOk()) err = copy.positional.append(std::move(p));
        }
        copy.options = options;
        copy.lastStatus = lastStatus;
        if (err.isOk()) err = copy.cwd.assign(cwd.view());
        return Result<ShellState>(std::move(copy), err);
}

const ShellFunction *ShellState::findFunction(StringView name) const {
        for (const ShellFunction &f : functions) {
                if (f.name == name) return &f;
        }
        return nullptr;
}

Error ShellState::defineFunction(StringView name, StringView source) {
        for (ShellFunction &f : functions) {
                if (f.name == name) return f.source.assign(source);
        }
        size_t at = 0;
        while (at < functions.size() && functions[at].name.view() < name) ++at;
        ShellFunction f;
        Error         err = f.name.assign(name);
        if (err.isOk()) err = f.source.assign(source);
        if (err.isOk()) err = functions.insert(at, std::move(f));
        return err;
}

void ShellState::removeFunction(StringView name) {
        for (size_t i = 0; i < functions.size(); ++i) {
                if (functions[i].name == name) {
                        functions.removeAt(i);
                        return;
                }
        }
        return;
}

void ShellState::optionLetters(String &out) const {
        if (options.errexit) (void)out.append('e');
        if (options.nounset) (void)out.append('u');
        if (options.xtrace) (void)out.append('x');
        if (options.noclobber) (void)out.append('C');
        return;
}

// ---------------------------------------------------------------------------
// Expansion
// ---------------------------------------------------------------------------

/*
 * Expands words. Fields mode splits unquoted expansion results on IFS and
 * gives "$@" one field per parameter; String mode joins everything into one
 * string; Pattern mode is String mode with quoted pattern characters
 * escaped, for case patterns and ${x#pattern}.
 */
class Expander {
        public:
                enum class Mode : uint8_t {
                        Fields,
                        String,
                        Pattern
                };

                Expander(Executor &ex, Mode mode, List<String> *fields) : _ex(ex), _mode(mode), _fields(fields) {
                        // A copy: the word may assign variables ($((x=1))), moving the table.
                        const String *ifs = ex._state.vars.value("IFS");
                        (void)_ifs.assign(ifs != nullptr ? ifs->view() : DefaultIfs);
                }

                Error word(const Word *w) {
                        const WordPart *p = w->parts;
                        // `~` and `~/...`: $HOME (not split or globbed). `~user` stays as is.
                        if (p != nullptr && p->kind == WordPart::Kind::Literal && !p->quoted &&
                            (p->text == "~" ? p->next == nullptr : p->text.startsWith("~/"))) {
                                const String *home = _ex._state.vars.value("HOME");
                                if (home != nullptr) {
                                        if (append(*home, true, false).isError()) return Error::Invalid;
                                        if (append(p->text.mid(1), false, false).isError()) return Error::Invalid;
                                        p = p->next;
                                }
                        }
                        Error err = parts(p);
                        if (err.isError()) return err;
                        if (_mode == Mode::Fields && (_exists || (w->parts == nullptr && w->hasQuotes)))
                                return emit(true);
                        return Error::Ok;
                }

                String take() { return std::move(_cur); }

        private:
                Error parts(const WordPart *p) {
                        for (; p != nullptr; p = p->next) {
                                Error err = part(p);
                                if (err.isError()) return err;
                        }
                        return Error::Ok;
                }

                Error part(const WordPart *p) {
                        switch (p->kind) {
                                case WordPart::Kind::Literal: return append(p->text, p->quoted, false);
                                case WordPart::Kind::Parameter: return parameter(p);
                                case WordPart::Kind::CommandSubstitution: {
                                        auto [text, err] = _ex.substitute(p->text);
                                        if (err.isError()) return fatal(err);
                                        // Abandoned inside (Ctrl-C, out of stack): so is this command.
                                        if (_ex._flow != Executor::Flow::Normal) return Error::Invalid;
                                        return append(text, p->quoted, true);
                                }
                                case WordPart::Kind::Arithmetic: {
                                        int64_t value = 0;
                                        Error   err = _ex.evaluate(p->text, value);
                                        if (err.isError()) return err;
                                        char buf[24];
                                        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(value));
                                        return append(buf, p->quoted, true);
                                }
                        }
                        return Error::Ok;
                }

                // Fails the expansion. `${x?}` and unset variables (status 127) end
                // the shell, as in bash; anything else abandons the command.
                Error fatal(Error err, int status = 1) {
                        if (err == Error::NoMem) _ex.report("out of memory");
                        _ex._errorStatus = status;
                        if (_ex._flow == Executor::Flow::Normal)
                                _ex.setFlow(status == 127 ? Executor::Flow::Exit : Executor::Flow::Abort);
                        return Error::Invalid;
                }

                Error push(char c) {
                        if (note(StringView(&c, 1), false).isError()) return Error::Invalid;
                        Error err = _cur.append(c);
                        return err.isError() ? fatal(err) : err;
                }

                // Pathname expansion bookkeeping, before @p text joins the field:
                // `_pat` is the field as a pattern (quoted special characters
                // escaped), kept only from the first character that makes it
                // differ from `_cur`; `_magic` is an unquoted `*`, `?` or `[`.
                Error note(StringView text, bool quoted) {
                        if (_mode != Mode::Fields) return Error::Ok;
                        if (!_patActive) {
                                bool needed = false;
                                for (char c : text) needed = needed || (quoted ? isGlobSpecial(c) : isGlobMagic(c));
                                if (!needed) return Error::Ok;
                                if (_pat.assign(_cur).isError()) return fatal(Error::NoMem);
                                _patActive = true;
                        }
                        for (char c : text) {
                                if (quoted && isGlobSpecial(c) && _pat.append('\\').isError())
                                        return fatal(Error::NoMem);
                                if (!quoted && isGlobMagic(c)) _magic = true;
                                if (_pat.append(c).isError()) return fatal(Error::NoMem);
                        }
                        return Error::Ok;
                }

                Error append(StringView text, bool quoted, bool expansion) {
                        if (quoted) _exists = true;
                        if (_mode == Mode::Fields && expansion && !quoted) return split(text);
                        _afterWhite = false;
                        if (!text.isEmpty()) _exists = true;
                        if (_mode == Mode::Pattern && quoted) {
                                for (char c : text) {
                                        if (StringView("*?[]\\").contains(c) && push('\\').isError())
                                                return Error::Invalid;
                                        if (push(c).isError()) return Error::Invalid;
                                }
                                return Error::Ok;
                        }
                        if (note(text, quoted).isError()) return Error::Invalid;
                        Error err = _cur.append(text);
                        return err.isError() ? fatal(err) : err;
                }

                bool isIfs(char c) const { return _ifs.view().contains(c); }

                static bool isWhite(char c) { return c == ' ' || c == '\t' || c == '\n'; }

                // Field splitting of an unquoted expansion result.
                Error split(StringView text) {
                        for (char c : text) {
                                if (!isIfs(c)) {
                                        if (push(c).isError()) return Error::Invalid;
                                        _exists = true;
                                        _afterWhite = false;
                                } else if (isWhite(c)) {
                                        if (_exists) {
                                                if (emit().isError()) return Error::Invalid;
                                                _afterWhite = true;
                                        }
                                } else {
                                        // A non-white IFS character ends a field, even an empty one,
                                        // unless white space just ended it.
                                        if (!_afterWhite && emit(true).isError()) return Error::Invalid;
                                        _afterWhite = false;
                                }
                        }
                        return Error::Ok;
                }

                Error emit(bool force = false) {
                        if (!_exists && !force) return Error::Ok;
                        Error  err;
                        size_t matches = 0;
                        // A field with unquoted magic is replaced by the paths it
                        // matches; with none, it stays as it is (bash's default).
                        if (_magic && Glob::hasMagic(_pat)) err = _ex.expandPaths(_pat, *_fields, matches);
                        if (err.isOk() && matches == 0) err = _fields->append(std::move(_cur));
                        _cur = String();
                        _pat.clear();
                        _patActive = false;
                        _magic = false;
                        _exists = false;
                        return err.isError() ? fatal(err) : err;
                }

                // Looks up a parameter. Specials format into @p scratch.
                bool lookup(StringView name, StringView &value, String &scratch) {
                        ShellState &st = _ex._state;
                        if (name == "?" || name == "#" || name == "$") {
                                const long n = name == "?"
                                                       ? st.lastStatus
                                                       : (name == "#" ? static_cast<long>(st.positional.size()) : 1);
                                if (scratch.appendf("%ld", n).isError()) return false;
                                value = scratch;
                                return true;
                        }
                        if (name == "-") {
                                st.optionLetters(scratch);
                                value = scratch;
                                return true;
                        }
                        if (name == "!") return false;
                        if (name == "@" || name == "*") {
                                const char sep = name == "*" ? (_ifs.isEmpty() ? '\0' : _ifs[0]) : ' ';
                                for (size_t i = 0; i < st.positional.size(); ++i) {
                                        if (i > 0 && sep != '\0') (void)scratch.append(sep);
                                        (void)scratch.append(st.positional[i]);
                                }
                                value = scratch;
                                return !st.positional.isEmpty();
                        }
                        if (isDigits(name)) {
                                unsigned long n = 0;
                                for (char c : name) n = n * 10 + static_cast<unsigned long>(c - '0');
                                if (n == 0) {
                                        value = ShellName;
                                        return true;
                                }
                                if (n > st.positional.size()) return false;
                                value = st.positional[n - 1];
                                return true;
                        }
                        const String *v = st.vars.value(name);
                        if (v == nullptr) return false;
                        value = *v;
                        return true;
                }

                Error unbound(StringView name) {
                        _ex.report("%.*s: unbound variable", static_cast<int>(name.size()), name.data());
                        return fatal(Error::Invalid, 127);
                }

                // "$@", "$*", $@, $* without an operation.
                Error positional(const WordPart *p) {
                        const List<String> &params = _ex._state.positional;
                        const bool          at = p->text == "@";
                        if (_mode == Mode::Fields && at && p->quoted) {
                                for (size_t i = 0; i < params.size(); ++i) {
                                        if (i > 0 && emit(true).isError()) return Error::Invalid;
                                        if (append(params[i], true, true).isError()) return Error::Invalid;
                                }
                                return Error::Ok;
                        }
                        if (_mode == Mode::Fields && !p->quoted) {
                                for (size_t i = 0; i < params.size(); ++i) {
                                        if (i > 0 && emit().isError()) return Error::Invalid;
                                        if (split(params[i]).isError()) return Error::Invalid;
                                }
                                return Error::Ok;
                        }
                        String     scratch;
                        StringView value;
                        (void)lookup(p->text, value, scratch);
                        return append(value, p->quoted, true);
                }

                Error subExpand(const Word *w, Mode mode, String &out) {
                        if (w == nullptr) return Error::Ok;
                        Expander sub(_ex, mode, nullptr);
                        Error    err = sub.word(w);
                        out = sub.take();
                        return err;
                }

                Error parameter(const WordPart *p) {
                        const StringView name = p->text;
                        const bool       special = name == "@" || name == "*";
                        if (special && p->op == ParamOp::None) return positional(p);
                        String     scratch;
                        StringView value;
                        const bool set = lookup(name, value, scratch);
                        const bool nounset = _ex._state.options.nounset && !special;
                        switch (p->op) {
                                case ParamOp::None:
                                        if (!set && nounset) return unbound(name);
                                        return append(value, p->quoted, true);
                                case ParamOp::Length: {
                                        if (!set && nounset) return unbound(name);
                                        char         buf[12];
                                        const size_t n = special ? _ex._state.positional.size() : value.size();
                                        std::snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(n));
                                        return append(buf, p->quoted, true);
                                }
                                case ParamOp::Default:
                                case ParamOp::Assign:
                                case ParamOp::Error:
                                case ParamOp::Alternative: break;
                                case ParamOp::RemoveSmallestPrefix:
                                case ParamOp::RemoveLargestPrefix:
                                case ParamOp::RemoveSmallestSuffix:
                                case ParamOp::RemoveLargestSuffix: {
                                        if (!set && nounset) return unbound(name);
                                        // Own the value: the pattern may assign variables ($((x=1))).
                                        String owned;
                                        if (owned.assign(value).isError()) return fatal(Error::NoMem);
                                        value = owned;
                                        String pattern;
                                        if (subExpand(p->arg, Mode::Pattern, pattern).isError()) return Error::Invalid;
                                        const bool prefix = p->op == ParamOp::RemoveSmallestPrefix ||
                                                            p->op == ParamOp::RemoveLargestPrefix;
                                        const bool longest = p->op == ParamOp::RemoveLargestPrefix ||
                                                             p->op == ParamOp::RemoveLargestSuffix;
                                        const size_t n = prefix ? Glob::matchPrefix(pattern, value, longest)
                                                                : Glob::matchSuffix(pattern, value, longest);
                                        StringView   rest = value;
                                        if (n != StringView::NPos)
                                                rest = prefix ? value.mid(n) : value.left(value.size() - n);
                                        return append(rest, p->quoted, true);
                                }
                        }
                        const bool use = set && !(p->colon && value.isEmpty());
                        if (p->quoted) _exists = true;
                        switch (p->op) {
                                case ParamOp::Default:
                                        if (use) return append(value, p->quoted, true);
                                        return p->arg != nullptr ? parts(p->arg->parts) : Error::Ok;
                                case ParamOp::Alternative:
                                        if (!use) return Error::Ok;
                                        return p->arg != nullptr ? parts(p->arg->parts) : Error::Ok;
                                case ParamOp::Assign: {
                                        if (use) return append(value, p->quoted, true);
                                        if (!isName(name)) {
                                                _ex.report("$%.*s: cannot assign in this way",
                                                           static_cast<int>(name.size()), name.data());
                                                return fatal(Error::Invalid);
                                        }
                                        String v;
                                        if (subExpand(p->arg, Mode::String, v).isError()) return Error::Invalid;
                                        Error err = _ex._state.vars.set(name, v);
                                        if (err == Error::ReadOnly) {
                                                _ex.report("%.*s: readonly variable", static_cast<int>(name.size()),
                                                           name.data());
                                                return fatal(err);
                                        }
                                        if (err.isError()) return fatal(err);
                                        return append(v, p->quoted, true);
                                }
                                default: { // Error
                                        if (use) return append(value, p->quoted, true);
                                        String message;
                                        if (subExpand(p->arg, Mode::String, message).isError()) return Error::Invalid;
                                        if (message.isEmpty())
                                                (void)message.append(p->colon ? "parameter null or not set"
                                                                              : "parameter not set");
                                        _ex.report("%.*s: %s", static_cast<int>(name.size()), name.data(),
                                                   message.cStr());
                                        return fatal(Error::Invalid, 127);
                                }
                        }
                }

                Executor     &_ex;
                Mode          _mode;
                List<String> *_fields;
                String        _ifs;
                String        _cur;
                String        _pat; // see note()
                bool          _patActive = false;
                bool          _magic = false;
                bool          _exists = false;     // the current field exists (even if empty)
                bool          _afterWhite = false; // white IFS space just ended a field
};

Error Executor::expandWords(const Word *first, List<String> &out) {
        for (const Word *w = first; w != nullptr; w = w->next) {
                Expander e(*this, Expander::Mode::Fields, &out);
                Error    err = e.word(w);
                if (err.isError()) return err;
        }
        return Error::Ok;
}

Error Executor::expandString(const Word *word, String &out) {
        Expander e(*this, Expander::Mode::String, nullptr);
        Error    err = e.word(word);
        out = e.take();
        return err;
}

Error Executor::expandPattern(const Word *word, String &out) {
        Expander e(*this, Expander::Mode::Pattern, nullptr);
        Error    err = e.word(word);
        out = e.take();
        return err;
}

// ---------------------------------------------------------------------------
// Executor
// ---------------------------------------------------------------------------

struct Executor::LocalSave {
                String  name;
                bool    existed = false;
                String  value;
                uint8_t flags = 0;
};

Executor::Executor(Shell &shell, ShellState &state, FdTable &fds, size_t stackBytes)
    : _shell(shell), _state(state), _fds(&fds), _ownArena(MemSpace::Psram, 4096), _arena(&_ownArena),
      _stackBudget(stackBytes > StackReserveBytes + CallerStackBytes ? stackBytes - StackReserveBytes - CallerStackBytes
                                                                     : 0) {}

Executor::Executor(Executor &parent, ShellState &state, FdTable &fds)
    : _shell(parent._shell), _state(state), _fds(&fds), _ownArena(MemSpace::Psram, 4096), _arena(parent._arena),
      _stackBase(parent._stackBase), _stackBudget(parent._stackBudget), _loopDepth(parent._loopDepth),
      _callDepth(parent._callDepth), _sourceDepth(parent._sourceDepth) {}

// The real stack position. (Not the address of a local: sanitizers may
// put locals on a separate "fake stack".)
static uintptr_t stackPosition() {
        return reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
}

bool Executor::stackExhausted() {
        const uintptr_t sp = stackPosition();
        return _stackBase > sp && _stackBase - sp > _stackBudget;
}

// A subshell that ran out of stack, or was interrupted, takes its parent with it.
void Executor::inheritAbort(const Executor &child) {
        if (child._flow == Flow::Interrupt) setFlow(Flow::Interrupt);
        if (child._overflowed) {
                _overflowed = true;
                setFlow(Flow::Abort);
        }
        return;
}

bool Executor::interrupted() {
        if (!_shell.console().interrupt().isRaised()) return false;
        setFlow(Flow::Interrupt);
        return true;
}

void Executor::report(const char *fmt, ...) {
        char    buf[CommandContext::PrintBufferBytes];
        va_list args;
        va_start(args, fmt);
        int len = std::vsnprintf(buf, sizeof(buf) - 1, fmt, args);
        va_end(args);
        if (len < 0) return;
        if (static_cast<size_t>(len) > sizeof(buf) - 2) len = static_cast<int>(sizeof(buf) - 2);
        buf[len++] = '\n';
        FileHandle *err = _fds->handle(FdTable::Stderr);
        if (err != nullptr) (void)err->write(StringView(buf, static_cast<size_t>(len)), WaitForever);
        return;
}

int Executor::expansionFailed() {
        const int status = _errorStatus;
        _errorStatus = 1;
        return finish(status, false);
}

// `set -x`: the command as it runs, after `PS4`, on stderr.
void Executor::xtrace(StringView text) {
        const String *ps4 = _state.vars.value("PS4");
        String        line;
        (void)line.append(ps4 != nullptr ? ps4->view() : StringView("+ "));
        (void)line.append(text);
        (void)line.append('\n');
        FileHandle *err = _fds->handle(FdTable::Stderr);
        if (err != nullptr) (void)err->write(line, WaitForever);
        return;
}

int Executor::finish(int status, bool errexitApplies) {
        _state.lastStatus = status;
        if (errexitApplies && status != 0 && _state.options.errexit && _errexitSuppressed == 0 && _flow == Flow::Normal)
                setFlow(Flow::Exit);
        return status;
}

int Executor::execute(const Node *n) {
        if (_stackBase == 0) _stackBase = stackPosition();
        if (_flow != Flow::Normal) return _state.lastStatus;
        if (stackExhausted()) {
                report("maximum nesting depth exceeded");
                _overflowed = true;
                setFlow(Flow::Abort);
                return finish(1, false);
        }
        switch (n->kind) {
                case NodeKind::Simple: return runSimple(static_cast<const SimpleNode *>(n));
                case NodeKind::Pipeline: return runPipeline(static_cast<const PipelineNode *>(n));
                case NodeKind::AndOr: {
                        const auto *a = static_cast<const AndOrNode *>(n);
                        ++_errexitSuppressed;
                        const int left = execute(a->left);
                        --_errexitSuppressed;
                        if (_flow != Flow::Normal) return left;
                        if ((left == 0) == a->isAnd) return execute(a->right);
                        return finish(left, false);
                }
                case NodeKind::List: {
                        int status = 0;
                        for (const Node *item = static_cast<const ListNode *>(n)->items; item != nullptr;
                             item = item->next) {
                                status = execute(item);
                                if (_flow != Flow::Normal) break;
                        }
                        return status;
                }
                case NodeKind::Function: {
                        const auto *f = static_cast<const FunctionNode *>(n);
                        Error       err = _state.defineFunction(f->name, f->source);
                        if (err.isError()) report("out of memory");
                        return finish(err.isOk() ? 0 : 1, false);
                }
                default: return runCompound(n);
        }
}

int Executor::runCompound(const Node *n) {
        if (n->redirects == nullptr) return runCompoundBody(n);
        FdTable fds = *_fds;
        if (applyRedirects(n->redirects, fds).isError()) return finish(1, true);
        FdTable *saved = _fds;
        _fds = &fds;
        const int status = runCompoundBody(n);
        _fds = saved;
        return status;
}

int Executor::runCompoundBody(const Node *n) {
        switch (n->kind) {
                case NodeKind::If: {
                        const auto *i = static_cast<const IfNode *>(n);
                        ++_errexitSuppressed;
                        const int cond = execute(i->condition);
                        --_errexitSuppressed;
                        if (_flow != Flow::Normal) return cond;
                        if (cond == 0) return execute(i->thenPart);
                        if (i->elsePart != nullptr) return execute(i->elsePart);
                        return finish(0, false);
                }
                case NodeKind::While:
                case NodeKind::Until: return runLoop(static_cast<const LoopNode *>(n));
                case NodeKind::For: return runFor(static_cast<const ForNode *>(n));
                case NodeKind::ArithFor: return runArithFor(static_cast<const ArithForNode *>(n));
                case NodeKind::Case: return runCase(static_cast<const CaseNode *>(n));
                case NodeKind::Group: return execute(static_cast<const BodyNode *>(n)->body);
                case NodeKind::Subshell: return runSubshell(static_cast<const BodyNode *>(n)->body);
                case NodeKind::Arithmetic: {
                        int64_t value = 0;
                        if (evaluate(static_cast<const ArithNode *>(n)->expression, value).isError())
                                return finish(1, false);
                        return finish(value != 0 ? 0 : 1, true);
                }
                default: return finish(0, false);
        }
}

// After a loop body: true to go round again. Consumes the loop's share of
// a break/continue.
bool Executor::loopIteration(bool &stop) {
        stop = false;
        switch (_flow) {
                case Flow::Normal: return true;
                case Flow::Break:
                        if (--_flowCount == 0) _flow = Flow::Normal;
                        stop = true;
                        return false;
                case Flow::Continue:
                        if (_flowCount > 1) {
                                --_flowCount; // continue an outer loop
                                stop = true;
                                return false;
                        }
                        _flow = Flow::Normal;
                        return true;
                default: stop = true; return false;
        }
}

int Executor::runLoop(const LoopNode *n) {
        int  status = 0;
        bool stop = false;
        ++_loopDepth;
        while (!interrupted()) {
                ++_errexitSuppressed;
                const int cond = execute(n->condition);
                --_errexitSuppressed;
                if (_flow != Flow::Normal) {
                        (void)loopIteration(stop);
                        if (stop) break;
                        continue;
                }
                if ((cond == 0) != (n->kind == NodeKind::While)) break;
                status = execute(n->body);
                if (!loopIteration(stop) || stop) break;
        }
        --_loopDepth;
        return finish(_flow == Flow::Interrupt ? Shell::ExitInterrupted : status, false);
}

int Executor::runFor(const ForNode *n) {
        if (!isName(n->variable)) {
                report("`%.*s': not a valid identifier", static_cast<int>(n->variable.size()), n->variable.data());
                return finish(1, false);
        }
        List<String> values;
        if (n->hasIn) {
                if (expandWords(n->words, values).isError()) return expansionFailed();
        } else {
                for (const String &p : _state.positional) {
                        auto [copy, err] = p.clone();
                        if (err.isError() || values.append(std::move(copy)).isError()) {
                                report("out of memory");
                                return finish(1, false);
                        }
                }
        }
        int  status = 0;
        bool stop = false;
        ++_loopDepth;
        for (const String &v : values) {
                if (interrupted()) break;
                Error err = _state.vars.set(n->variable, v);
                if (err.isError()) {
                        report("%.*s: %s", static_cast<int>(n->variable.size()), n->variable.data(),
                               err == Error::ReadOnly ? "readonly variable" : "out of memory");
                        status = 1;
                        break;
                }
                status = execute(n->body);
                if (!loopIteration(stop) || stop) break;
        }
        --_loopDepth;
        return finish(_flow == Flow::Interrupt ? Shell::ExitInterrupted : status, false);
}

int Executor::runArithFor(const ArithForNode *n) {
        int64_t value = 0;
        if (!n->init.isEmpty() && evaluate(n->init, value).isError()) return finish(1, false);
        int  status = 0;
        bool stop = false;
        ++_loopDepth;
        while (!interrupted()) {
                if (!n->condition.isEmpty()) {
                        if (evaluate(n->condition, value).isError()) {
                                status = 1;
                                break;
                        }
                        if (value == 0) break;
                }
                status = execute(n->body);
                if (!loopIteration(stop) || stop) break;
                if (!n->step.isEmpty() && evaluate(n->step, value).isError()) {
                        status = 1;
                        break;
                }
        }
        --_loopDepth;
        return finish(_flow == Flow::Interrupt ? Shell::ExitInterrupted : status, false);
}

int Executor::runCase(const CaseNode *n) {
        String subject;
        if (expandString(n->subject, subject).isError()) return expansionFailed();
        for (const CaseItem *item = n->items; item != nullptr; item = item->next) {
                for (const Word *p = item->patterns; p != nullptr; p = p->next) {
                        String pattern;
                        if (expandPattern(p, pattern).isError()) return expansionFailed();
                        if (!Glob::match(pattern, subject)) continue;
                        if (item->body == nullptr) return finish(0, false);
                        return execute(item->body);
                }
        }
        return finish(0, false);
}

int Executor::runSubshell(const Node *body) {
        auto [state, err] = _state.clone();
        if (err.isError()) {
                report("out of memory");
                return finish(1, true);
        }
        FdTable   fds = *_fds;
        Executor  child(*this, state, fds);
        const int status = child.execute(body);
        inheritAbort(child);
        return finish(status, true);
}

Error Executor::evaluate(StringView expression, int64_t &value) {
        const Arena::Mark mark = _arena->mark();
        SyntaxError       syntax;
        auto [word, err] = parseQuotedText(expression, *_arena, syntax);
        String text;
        if (err.isOk()) err = expandString(word, text);
        // A substitution in the expression abandoned the command: don't evaluate.
        if (err.isOk() && _flow != Flow::Normal) err = Error::Invalid;
        _arena->rewind(mark);
        if (err == Error::NoMem) report("out of memory");
        if (err.isError()) {
                if (_flow == Flow::Normal) setFlow(Flow::Abort);
                return Error::Invalid;
        }
        Arith::Failure failure;
        auto [result, aerr] = Arith::evaluate(text, _state.vars, failure);
        if (aerr.isError()) {
                if (aerr == Error::NoMem) {
                        report("out of memory");
                } else if (failure.message != nullptr) {
                        report("%s: %s (error token is \"%.*s\")", text.cStr(), failure.message,
                               static_cast<int>(failure.token.size()), failure.token.data());
                }
                setFlow(Flow::Abort);
                return Error::Invalid;
        }
        value = result;
        return Error::Ok;
}

Result<String> Executor::substitute(StringView source) {
        MemoryStream out(MemSpace::Psram);
        int          status = 0;
        {
                auto [handle, err] = FileHandle::open(out, FileHandle::Write);
                if (err.isError()) return makeError<String>(err);
                auto [state, cerr] = _state.clone();
                if (cerr.isError()) return makeError<String>(cerr);
                FdTable fds = *_fds;
                (void)fds.set(FdTable::Stdout, std::move(handle));
                Executor child(*this, state, fds);
                status = child.runSource(source);
                inheritAbort(child);
        }
        _substStatus = status;
        _substRan = true;
        String text = out.takeContents();
        size_t n = text.size();
        while (n > 0 && text[n - 1] == '\n') --n;
        text.truncate(n);
        return makeResult(std::move(text));
}

Vfs::Context Executor::vfs() const {
        return Vfs::Context{_state.cwd.isEmpty() ? StringView("/") : _state.cwd.view(), &_shell.console()};
}

Error Executor::expandPaths(StringView pattern, List<String> &out, size_t &count) {
        auto [n, err] = Glob::expandPaths(pattern, vfs(), out);
        count = n;
        return err;
}

// A redirection's file or descriptor: expanded as an argument (split and
// globbed), and it must come to exactly one word.
Error Executor::expandTarget(const Word *word, String &out) {
        List<String> fields;
        Error        err = expandWords(word, fields);
        if (err.isError()) return err;
        if (fields.size() != 1) {
                report("%.*s: ambiguous redirect", static_cast<int>(word->source.size()), word->source.data());
                return Error::Invalid;
        }
        out = std::move(fields[0]);
        return Error::Ok;
}

Error Executor::openTarget(const Redirection *r, StringView path, FileHandle::Ptr &out) {
        using Kind = Redirection::Kind;
        uint8_t flags = Vfs::Write | Vfs::Create | Vfs::Truncate;
        switch (r->kind) {
                case Kind::Input: flags = Vfs::Read; break;
                case Kind::InputOutput: flags = Vfs::Read | Vfs::Write | Vfs::Create; break;
                case Kind::Append:
                case Kind::AppendBoth: flags = Vfs::Write | Vfs::Create | Vfs::Append; break;
                case Kind::Clobber: break;
                default:
                        // `>`, `&>`, `>&file` with noclobber: not over an existing file.
                        if (_state.options.noclobber) {
                                const Result<FileInfo> info = Vfs::stat(path, vfs());
                                if (info.error().isOk() && info.value().type == FileType::File) {
                                        report("%.*s: cannot overwrite existing file", static_cast<int>(path.size()),
                                               path.data());
                                        return Error::Exists;
                                }
                                if (info.error() == Error::NotExist) flags = Vfs::Write | Vfs::Create | Vfs::Exclusive;
                        }
                        break;
        }
        auto [h, err] = path.isEmpty() ? makeError<FileHandle::Ptr>(Error::NotExist) : Vfs::open(path, flags, vfs());
        if (err.isError()) {
                report("%.*s: %s", static_cast<int>(path.size()), path.data(), CommandContext::errorText(err));
                return err;
        }
        out = std::move(h);
        return Error::Ok;
}

Error Executor::applyRedirects(const Redirection *r, FdTable &fds) {
        for (; r != nullptr; r = r->next) {
                using Kind = Redirection::Kind;
                const bool input = r->kind == Kind::Input || r->kind == Kind::InputOutput ||
                                   r->kind == Kind::DupInput || r->kind == Kind::HereDoc || r->kind == Kind::HereString;
                const int fd = r->fd >= 0 ? r->fd : (input ? 0 : 1);
                if (fd >= FdTable::MaxFds) {
                        report("%d: Bad file descriptor", fd);
                        return Error::BadDescriptor;
                }
                String text;
                Error  err = r->kind == Kind::HereDoc      ? expandString(r->body, text)
                             : r->kind == Kind::HereString ? expandString(r->target, text)
                                                           : expandTarget(r->target, text);
                if (err.isError()) return err;
                switch (r->kind) {
                        case Kind::HereDoc:
                        case Kind::HereString: {
                                if (r->kind == Kind::HereString && text.append('\n').isError()) return Error::NoMem;
                                auto [h, oerr] = TextStream::open(std::move(text));
                                if (oerr.isError()) {
                                        report("out of memory");
                                        return oerr;
                                }
                                (void)fds.set(fd, std::move(h));
                                break;
                        }
                        case Kind::DupInput:
                        case Kind::DupOutput: {
                                if (text == "-") {
                                        (void)fds.close(fd);
                                        break;
                                }
                                if (isDigits(text) && text.size() <= 4) {
                                        int source = 0;
                                        for (char c : text) source = source * 10 + (c - '0');
                                        if (fds.dup2(source, fd).isError()) {
                                                report("%d: Bad file descriptor", source);
                                                return Error::BadDescriptor;
                                        }
                                        break;
                                }
                                if (r->kind != Kind::DupOutput || r->fd >= 0) {
                                        report("%s: ambiguous redirect", text.cStr());
                                        return Error::Invalid;
                                }
                                // `>&file` is `&>file`.
                                FileHandle::Ptr h;
                                if (openTarget(r, text, h).isError()) return Error::Invalid;
                                (void)fds.set(1, std::move(h));
                                (void)fds.dup2(1, 2);
                                break;
                        }
                        default: {
                                FileHandle::Ptr h;
                                if (openTarget(r, text, h).isError()) return Error::Invalid;
                                (void)fds.set(fd, std::move(h));
                                if (r->kind == Kind::OutputBoth || r->kind == Kind::AppendBoth) (void)fds.dup2(fd, 2);
                                break;
                        }
                }
        }
        return Error::Ok;
}

int Executor::runSimple(const SimpleNode *n) {
        if (interrupted()) return finish(Shell::ExitInterrupted, false);
        _substRan = false;
        List<String> args;
        // A substitution may have abandoned the command (Ctrl-C, stack) mid-expansion.
        if (expandWords(n->words, args).isError() || _flow != Flow::Normal) return expansionFailed();

        if (args.isEmpty()) {
                // Assignments (and redirections) only.
                for (const Assignment *a = n->assignments; a != nullptr; a = a->next) {
                        String value;
                        if (expandString(a->value, value).isError()) return expansionFailed();
                        if (_state.options.xtrace) {
                                String trace;
                                (void)trace.append(a->name);
                                (void)trace.append('=');
                                appendShellQuoted(trace, value);
                                xtrace(trace);
                        }
                        Error err = _state.vars.set(a->name, value);
                        if (err.isError()) {
                                report("%.*s: %s", static_cast<int>(a->name.size()), a->name.data(),
                                       err == Error::ReadOnly ? "readonly variable" : "out of memory");
                                // As in bash, a failed assignment abandons the command.
                                setFlow(Flow::Abort);
                                return finish(1, false);
                        }
                }
                if (n->redirects != nullptr) {
                        FdTable fds = *_fds;
                        if (applyRedirects(n->redirects, fds).isError()) return finish(1, true);
                }
                return finish(_substRan ? _substStatus : 0, true);
        }

        // `NAME=value cmd`: set for the command, then put back.
        List<LocalSave> saved;
        int             status = 0;
        bool            ok = true;
        String          trace;
        for (const Assignment *a = n->assignments; a != nullptr && ok; a = a->next) {
                String value;
                if (expandString(a->value, value).isError()) return expansionFailed();
                if (_state.options.xtrace) {
                        (void)trace.append(a->name);
                        (void)trace.append('=');
                        appendShellQuoted(trace, value);
                        (void)trace.append(' ');
                }
                LocalSave   s;
                const auto *e = _state.vars.find(a->name);
                ok = s.name.assign(a->name).isOk();
                if (e != nullptr) {
                        s.existed = true;
                        s.flags = e->flags;
                        ok = ok && s.value.assign(e->value).isOk();
                }
                ok = ok && saved.append(std::move(s)).isOk();
                Error err = ok ? _state.vars.set(a->name, value) : Error(Error::NoMem);
                if (err.isOk()) err = _state.vars.addFlags(a->name, Variables::Exported);
                if (err.isError()) {
                        report("%.*s: %s", static_cast<int>(a->name.size()), a->name.data(),
                               err == Error::ReadOnly ? "readonly variable" : "out of memory");
                        status = 1;
                        ok = false;
                }
        }
        if (ok && _state.options.xtrace) {
                for (size_t i = 0; i < args.size(); ++i) {
                        if (i > 0) (void)trace.append(' ');
                        appendShellQuoted(trace, args[i]);
                }
                xtrace(trace);
        }
        if (ok) status = runCommand(args, n->redirects);
        for (size_t i = saved.size(); i-- > 0;) {
                const LocalSave &s = saved[i];
                (void)_state.vars.restore(s.name, s.existed, s.value, s.flags);
        }
        return finish(status, true);
}

int Executor::runCommand(List<String> &args, const Redirection *redirects) {
        FdTable fds = *_fds;
        if (redirects != nullptr && applyRedirects(redirects, fds).isError()) return 1;
        FdTable *saved = _fds;
        _fds = &fds;
        const int status = invoke(args, true);
        _fds = saved;
        return status;
}

int Executor::callFunction(const ShellFunction &fn, List<String> &args) {
        if (_callDepth >= MaxCallDepth) {
                // As bash with FUNCNEST: abandon the command.
                report("%s: maximum function nesting level exceeded (%d)", fn.name.cStr(), MaxCallDepth);
                setFlow(Flow::Abort);
                return 1;
        }
        // A copy: the function may redefine itself while it runs.
        auto [source, err] = fn.source.clone();
        List<String> params;
        if (err.isOk() && args.size() > 1) err = params.reserve(args.size() - 1);
        if (err.isError()) {
                report("out of memory");
                return 1;
        }
        for (size_t i = 1; i < args.size(); ++i) (void)params.append(std::move(args[i]));
        std::swap(_state.positional, params);
        List<LocalSave>  locals;
        List<LocalSave> *outer = _locals;
        _locals = &locals;
        ++_callDepth;
        int status = runSource(source);
        if (_flow == Flow::Return) {
                clearFlow();
                status = _state.lastStatus;
        }
        --_callDepth;
        _locals = outer;
        for (size_t i = locals.size(); i-- > 0;) {
                const LocalSave &s = locals[i];
                (void)_state.vars.restore(s.name, s.existed, s.value, s.flags);
        }
        std::swap(_state.positional, params);
        return status;
}

Error Executor::declareLocal(StringView name, const StringView *value) {
        if (_callDepth == 0) return Error::InvalidState;
        const Variables::Entry *e = _state.vars.find(name);
        if (e != nullptr && (e->flags & Variables::ReadOnly) != 0) return Error::ReadOnly;
        if (_locals != nullptr) {
                bool saved = false;
                for (const LocalSave &s : *_locals) saved = saved || s.name == name;
                if (!saved) {
                        LocalSave s;
                        Error     err = s.name.assign(name);
                        if (e != nullptr) {
                                s.existed = true;
                                s.flags = e->flags;
                                if (err.isOk()) err = s.value.assign(e->value);
                        }
                        if (err.isOk()) err = _locals->append(std::move(s));
                        if (err.isError()) return err;
                }
        }
        if (value != nullptr) return _state.vars.set(name, *value);
        return _state.vars.restore(name, true, StringView(), 0);
}

int Executor::runSource(StringView source, bool script) {
        const Arena::Mark mark = _arena->mark();
        Parser            parser(source, *_arena);
        int               status = 0;
        for (;;) {
                auto [node, err] = parser.next();
                if (err.isError()) {
                        if (err == Error::NoMem) {
                                report("out of memory");
                        } else if (err == Error::NotReady) {
                                report("syntax error: unexpected end of file");
                        } else {
                                String message;
                                (void)appendSyntaxError(parser.error(), message);
                                report("line %u: %s", parser.error().line, message.cStr());
                        }
                        status = finish(2, false);
                        break;
                }
                if (node == nullptr) break;
                status = execute(node);
                _arena->rewind(mark);
                if (script && _flow == Flow::Abort && !_overflowed) clearFlow();
                if (_flow != Flow::Normal) break;
        }
        _arena->rewind(mark);
        return status;
}

Error Executor::readFile(StringView path, String &out) {
        out.clear();
        auto [file, err] = Vfs::open(path, Vfs::Read, vfs());
        if (err.isError()) return err;
        char buf[512];
        for (;;) {
                auto [n, rerr] =
                        file->read(WritableByteSpan(reinterpret_cast<uint8_t *>(buf), sizeof(buf)), WaitForever);
                if (rerr == Error::EndOfFile || (rerr.isOk() && n == 0)) return Error::Ok;
                if (rerr.isError()) return rerr;
                err = out.append(StringView(buf, n));
                if (err.isError()) return err;
        }
}

int Executor::runSourced(StringView source, Span<char *const> args) {
        List<String> params;
        const bool   hasArgs = !args.isEmpty();
        if (hasArgs) {
                Error err = params.reserve(args.size());
                for (size_t i = 0; err.isOk() && i < args.size(); ++i) {
                        auto [p, perr] = String::fromView(args[i]);
                        err = perr.isOk() ? params.append(std::move(p)) : perr;
                }
                if (err.isError()) {
                        report("out of memory");
                        return finish(1, false);
                }
                std::swap(_state.positional, params);
        }
        ++_sourceDepth;
        int status = runSource(source, true);
        --_sourceDepth;
        if (_flow == Flow::Return) {
                clearFlow();
                status = _state.lastStatus;
        }
        if (hasArgs) std::swap(_state.positional, params);
        return status;
}

int Executor::runInSubshell(StringView source, Span<char *const> args, const ShellOptions *options) {
        auto [state, err] = _state.clone();
        List<String> params;
        if (err.isOk()) err = params.reserve(args.size());
        for (size_t i = 0; err.isOk() && i < args.size(); ++i) {
                auto [p, perr] = String::fromView(args[i]);
                err = perr.isOk() ? params.append(std::move(p)) : perr;
        }
        if (err.isError()) {
                report("out of memory");
                return 1;
        }
        state.positional = std::move(params);
        if (options != nullptr) state.options = *options;
        FdTable  fds = *_fds;
        Executor child(*this, state, fds);
        // A script of its own: not inside the caller's functions or sourced scripts.
        child._callDepth = 0;
        child._sourceDepth = 0;
        child._loopDepth = 0;
        const int status = child.runSource(source, true);
        inheritAbort(child);
        return status;
}

void Executor::stageMain(void *arg) {
        StageRun *run = static_cast<StageRun *>(arg);
        Executor  child(run->parent->_shell, run->state, run->fds, Shell::WorkerStackBytes);
        child._callDepth = run->parent->_callDepth;
        child._sourceDepth = run->parent->_sourceDepth;
        run->status = child.execute(run->node);
        // Closing this stage's ends gives its neighbors EOF / a broken pipe.
        run->fds.closeAll();
        return;
}

int Executor::runPipeline(const PipelineNode *p) {
        size_t count = 0;
        for (const Node *c = p->commands; c != nullptr; c = c->next) ++count;
        if (count == 1) {
                if (p->negated) ++_errexitSuppressed;
                const int status = execute(p->commands);
                if (p->negated) --_errexitSuppressed;
                return finish(p->negated ? status == 0 : status, false);
        }
        if (count > Shell::MaxStages) {
                report("too many pipeline stages (at most %u)", static_cast<unsigned>(Shell::MaxStages));
                return finish(1, true);
        }
        List<StageRun> runs;
        if (runs.reserve(count).isError()) { // workers hold pointers into it
                report("out of memory");
                return finish(1, true);
        }
        for (const Node *c = p->commands; c != nullptr; c = c->next) {
                StageRun run;
                run.parent = this;
                run.node = c;
                run.fds = *_fds;
                (void)runs.append(std::move(run));
        }
        for (size_t i = 0; i + 1 < count; ++i) {
                auto [ends, err] = Pipe::create(&_shell.console().interrupt());
                if (err.isError()) {
                        report("cannot create pipe: %s", err.desc());
                        return finish(1, true);
                }
                (void)runs[i].fds.set(FdTable::Stdout, std::move(ends.write));
                (void)runs[i + 1].fds.set(FdTable::Stdin, std::move(ends.read));
        }
        // Earlier stages are subshells on workers; the last runs here.
        for (size_t i = 0; i + 1 < count; ++i) {
                StageRun &run = runs[i];
                auto [state, cerr] = _state.clone();
                Error err = cerr;
                if (err.isOk()) {
                        run.state = std::move(state);
                        auto [worker, werr] = _shell.workers().start(&Executor::stageMain, &run);
                        err = werr;
                        run.worker = worker;
                }
                if (err.isError()) {
                        report("cannot start pipeline stage: %s", err.desc());
                        run.status = 1;
                        run.fds.closeAll();
                }
        }
        StageRun &last = runs.back();
        FdTable  *saved = _fds;
        _fds = &last.fds;
        if (p->negated) ++_errexitSuppressed;
        last.status = execute(last.node);
        if (p->negated) --_errexitSuppressed;
        _fds = saved;
        last.fds.closeAll();
        for (StageRun &run : runs) {
                if (run.worker != nullptr) _shell.workers().wait(run.worker);
        }
        const int status = p->negated ? last.status == 0 : last.status;
        return finish(status, !p->negated);
}

BMDS_NAMESPACE_END
