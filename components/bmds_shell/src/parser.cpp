/**
 * @file      parser.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Lexer and recursive-descent parser for the shell language.
 */

#include <cstring>
#include <bmds/parser.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr const char *UnexpectedToken = "syntax error near unexpected token `%.*s'";
        constexpr const char *BadSubstitution = "%.*s: bad substitution";

        enum class Tok : uint8_t {
                Error, // lexing failed; the status says why
                End,
                Newline,
                Word,
                ArithCmd, // `(( ... ))` in command position
                Semi,
                DSemi,
                Amp,
                AndIf,
                Pipe,
                PipeAmp,
                OrIf,
                LParen,
                RParen,
                // Redirection operators, Less..TLess
                Less,
                Great,
                DGreat,
                Clobber,
                LessGreat,
                LessAnd,
                GreatAnd,
                AndGreat,
                AndDGreat,
                DLess,
                DLessDash,
                TLess,
        };

        bool isRedirect(Tok t) {
                return t >= Tok::Less && t <= Tok::TLess;
        }

        struct Token {
                        Tok        kind = Tok::End;
                        int        ioNumber = -1;
                        Word      *word = nullptr;
                        StringView text;  // the token as written
                        StringView arith; // ArithCmd: the expression
                        size_t     start = 0;
                        size_t     end = 0;
        };

        bool isDigit(char c) {
                return c >= '0' && c <= '9';
        }

        bool isNameStart(char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        }

        bool isNameChar(char c) {
                return isNameStart(c) || isDigit(c);
        }

        bool isSpecialParam(char c) {
                return c == '?' || c == '#' || c == '@' || c == '*' || c == '$' || c == '!' || c == '-';
        }

        bool isOperatorChar(char c) {
                return c == ';' || c == '&' || c == '|' || c == '(' || c == ')' || c == '<' || c == '>';
        }

        bool isWordBreak(char c) {
                return c == ' ' || c == '\t' || c == '\n' || isOperatorChar(c);
        }

        bool isValidName(StringView s) {
                if (s.isEmpty() || !isNameStart(s[0])) return false;
                for (char c : s) {
                        if (!isNameChar(c)) return false;
                }
                return true;
        }

        // The text of a word with no quoting and no expansions, or empty.
        StringView plainText(const Word *w) {
                if (w == nullptr || w->parts == nullptr || w->hasQuotes) return StringView();
                if (w->parts->next != nullptr) return StringView();
                const WordPart *p = w->parts;
                if (p->kind != WordPart::Kind::Literal || p->quoted) return StringView();
                return p->text;
        }

        struct HereDoc {
                        Redirection *redir = nullptr;
                        StringView   delimiter;
                        bool         quoted = false;
        };

        class Lexer {
                public:
                        Lexer(StringView src, size_t pos, Arena &arena, SyntaxError &error)
                            : _src(src), _pos(pos), _end(src.size()), _arena(arena), _error(error) {}

                        size_t position() const { return _pos; }

                        // Here-documents whose bodies are still to come (at the next newline).
                        unsigned pendingHereDocs() const { return _hereCount; }

                        void reset(size_t pos) {
                                _pos = pos;
                                return;
                        }

                        Error lex(Token &t, bool commandStart) {
                                t = Token();
                                Error err = skipBlanks();
                                if (err.isError()) return err;
                                t.start = _pos;
                                if (_pos >= _end) {
                                        // A here-document still waiting for its body.
                                        if (_hereCount > 0) return Error::NotReady;
                                        t.kind = Tok::End;
                                } else if (_src[_pos] == '\n') {
                                        ++_pos;
                                        t.kind = Tok::Newline;
                                        err = readHereDocs();
                                } else if (commandStart && _src[_pos] == '(' && at(_pos + 1, '(')) {
                                        size_t inner = 0;
                                        bool   arith = false;
                                        err = scanArith(_pos + 2, inner, arith);
                                        if (err.isOk() && arith) {
                                                t.kind = Tok::ArithCmd;
                                                t.arith = _src.mid(_pos + 2, inner - (_pos + 2));
                                                _pos = inner + 2;
                                        } else if (err.isOk()) {
                                                err = lexOperator(t, -1);
                                        }
                                } else if (isOperatorChar(_src[_pos])) {
                                        err = lexOperator(t, -1);
                                } else if (isDigit(_src[_pos]) && ioNumberAhead()) {
                                        int n = 0;
                                        while (isDigit(_src[_pos])) n = n * 10 + (_src[_pos++] - '0');
                                        err = lexOperator(t, n);
                                } else {
                                        err = lexWord(t);
                                }
                                t.end = _pos;
                                t.text = _src.mid(t.start, t.end - t.start);
                                return err;
                        }

                        // Registers a here-document; its body is read at the next newline.
                        Error addHereDoc(Redirection *r, const Token &delimiter) {
                                if (_hereCount == MaxHereDocs) return fail("too many here-documents", StringView());
                                HereDoc &h = _hereDocs[_hereCount++];
                                h.redir = r;
                                h.quoted = delimiter.word->hasQuotes;
                                const Word *w = delimiter.word;
                                bool        literal = true;
                                size_t      length = 0;
                                for (const WordPart *p = w->parts; p != nullptr; p = p->next) {
                                        literal = literal && p->kind == WordPart::Kind::Literal;
                                        h.quoted = h.quoted || p->quoted;
                                        length += p->text.size();
                                }
                                if (!literal) {
                                        h.delimiter = delimiter.text;
                                } else if (w->parts != nullptr && w->parts->next == nullptr) {
                                        h.delimiter = w->parts->text;
                                } else {
                                        // Pieces joined by quote removal (`"E"OF`).
                                        char *buf = static_cast<char *>(_arena.alloc(length + 1, 1));
                                        if (buf == nullptr) return Error::NoMem;
                                        size_t n = 0;
                                        for (const WordPart *p = w->parts; p != nullptr; p = p->next) {
                                                if (!p->text.isEmpty())
                                                        std::memcpy(buf + n, p->text.data(), p->text.size());
                                                n += p->text.size();
                                        }
                                        h.delimiter = StringView(buf, n);
                                }
                                return Error::Ok;
                        }

                        // The whole input as here-document-style quoted text.
                        Error lexQuotedText(Word *&out) {
                                Word *w = _arena.create<Word>();
                                if (w == nullptr) return Error::NoMem;
                                w->hasQuotes = true;
                                WordPart *tail = nullptr;
                                Error     err = lexQuotedContent(w, tail, false);
                                out = w;
                                return err;
                        }

                        Error fail(const char *format, StringView token) {
                                _error.format = format;
                                _error.token = token;
                                _error.offset = _pos;
                                return Error::Invalid;
                        }

                private:
                        static constexpr unsigned MaxHereDocs = 8;

                        bool at(size_t i, char c) const { return i < _end && _src[i] == c; }

                        char ch(size_t i) const { return i < _end ? _src[i] : '\0'; }

                        // Input ran out mid-construct: more is needed, unless we are
                        // inside a here-document body, which is complete by now.
                        Error incomplete() {
                                if (_end != _src.size())
                                        return fail("unexpected end of here-document line", StringView());
                                return Error::NotReady;
                        }

                        bool ioNumberAhead() const {
                                size_t i = _pos;
                                while (i < _end && isDigit(_src[i])) ++i;
                                return i - _pos <= 4 && (at(i, '<') || at(i, '>'));
                        }

                        Error skipBlanks() {
                                while (_pos < _end) {
                                        const char c = _src[_pos];
                                        if (c == ' ' || c == '\t') {
                                                ++_pos;
                                        } else if (c == '\\' && at(_pos + 1, '\n')) {
                                                // A continuation at the very end wants another line.
                                                _pos += 2;
                                                if (_pos >= _end) return incomplete();
                                        } else if (c == '\\' && _pos + 1 == _end) {
                                                return incomplete();
                                        } else if (c == '#') {
                                                while (_pos < _end && _src[_pos] != '\n') ++_pos;
                                        } else {
                                                break;
                                        }
                                }
                                return Error::Ok;
                        }

                        Error lexOperator(Token &t, int ioNumber) {
                                const char c = _src[_pos];
                                const char n1 = ch(_pos + 1);
                                const char n2 = ch(_pos + 2);
                                size_t     len = 1;
                                switch (c) {
                                        case ';':
                                                t.kind = n1 == ';' ? Tok::DSemi : Tok::Semi;
                                                len = n1 == ';' ? 2 : 1;
                                                break;
                                        case '&':
                                                if (n1 == '&') {
                                                        t.kind = Tok::AndIf;
                                                        len = 2;
                                                } else if (n1 == '>') {
                                                        t.kind = n2 == '>' ? Tok::AndDGreat : Tok::AndGreat;
                                                        len = n2 == '>' ? 3 : 2;
                                                } else {
                                                        t.kind = Tok::Amp;
                                                }
                                                break;
                                        case '|':
                                                if (n1 == '|') {
                                                        t.kind = Tok::OrIf;
                                                        len = 2;
                                                } else if (n1 == '&') {
                                                        t.kind = Tok::PipeAmp;
                                                        len = 2;
                                                } else {
                                                        t.kind = Tok::Pipe;
                                                }
                                                break;
                                        case '(': t.kind = Tok::LParen; break;
                                        case ')': t.kind = Tok::RParen; break;
                                        case '<':
                                                if (n1 == '<' && n2 == '<') {
                                                        t.kind = Tok::TLess;
                                                        len = 3;
                                                } else if (n1 == '<' && n2 == '-') {
                                                        t.kind = Tok::DLessDash;
                                                        len = 3;
                                                } else if (n1 == '<') {
                                                        t.kind = Tok::DLess;
                                                        len = 2;
                                                } else if (n1 == '&') {
                                                        t.kind = Tok::LessAnd;
                                                        len = 2;
                                                } else if (n1 == '>') {
                                                        t.kind = Tok::LessGreat;
                                                        len = 2;
                                                } else {
                                                        t.kind = Tok::Less;
                                                }
                                                break;
                                        default: // '>'
                                                if (n1 == '>') {
                                                        t.kind = Tok::DGreat;
                                                        len = 2;
                                                } else if (n1 == '&') {
                                                        t.kind = Tok::GreatAnd;
                                                        len = 2;
                                                } else if (n1 == '|') {
                                                        t.kind = Tok::Clobber;
                                                        len = 2;
                                                } else {
                                                        t.kind = Tok::Great;
                                                }
                                                break;
                                }
                                _pos += len;
                                t.ioNumber = ioNumber;
                                if (ioNumber >= 0 && !isRedirect(t.kind))
                                        return fail(UnexpectedToken, _src.mid(_pos - len, len));
                                return Error::Ok;
                        }

                        Error addPart(Word *w, WordPart *&tail, WordPart::Kind kind, StringView text, bool quoted,
                                      WordPart **made = nullptr) {
                                WordPart *p = _arena.create<WordPart>();
                                if (p == nullptr) return Error::NoMem;
                                p->kind = kind;
                                p->text = text;
                                p->quoted = quoted;
                                if (tail == nullptr) {
                                        w->parts = p;
                                } else {
                                        tail->next = p;
                                }
                                tail = p;
                                if (made != nullptr) *made = p;
                                return Error::Ok;
                        }

                        Error literal(Word *w, WordPart *&tail, size_t start, size_t length, bool quoted) {
                                return addPart(w, tail, WordPart::Kind::Literal, _src.mid(start, length), quoted);
                        }

                        Error lexWord(Token &t) {
                                Word *w = _arena.create<Word>();
                                if (w == nullptr) return Error::NoMem;
                                WordPart    *tail = nullptr;
                                Error        err;
                                const size_t start = _pos;
                                while (_pos < _end && err.isOk()) {
                                        const char c = _src[_pos];
                                        if (isWordBreak(c)) break;
                                        if (c == '\\') {
                                                if (_pos + 1 == _end) return incomplete();
                                                if (_src[_pos + 1] == '\n') {
                                                        _pos += 2;
                                                        if (_pos >= _end) return incomplete();
                                                        continue;
                                                }
                                                err = literal(w, tail, _pos + 1, 1, true);
                                                _pos += 2;
                                        } else if (c == '\'') {
                                                err = lexSingleQuoted(w, tail);
                                        } else if (c == '"') {
                                                err = lexDoubleQuoted(w, tail);
                                        } else if (c == '$') {
                                                err = lexDollar(w, tail, false);
                                        } else if (c == '`') {
                                                err = lexBackquote(w, tail, false);
                                        } else {
                                                const size_t s = _pos;
                                                while (_pos < _end && !isWordBreak(_src[_pos]) &&
                                                       !StringView("\\'\"$`").contains(_src[_pos]))
                                                        ++_pos;
                                                err = literal(w, tail, s, _pos - s, false);
                                        }
                                }
                                w->source = _src.mid(start, _pos - start);
                                t.kind = Tok::Word;
                                t.word = w;
                                return err;
                        }

                        Error lexSingleQuoted(Word *w, WordPart *&tail) {
                                size_t close = _pos + 1;
                                while (close < _end && _src[close] != '\'') ++close;
                                if (close >= _end) return incomplete();
                                w->hasQuotes = true;
                                Error err = literal(w, tail, _pos + 1, close - _pos - 1, true);
                                _pos = close + 1;
                                return err;
                        }

                        Error lexDoubleQuoted(Word *w, WordPart *&tail) {
                                w->hasQuotes = true;
                                ++_pos;
                                return lexQuotedContent(w, tail, true);
                        }

                        // Double-quoted text (up to the closing quote) or a here-document
                        // body (up to `_end`): `$`, backquotes and some backslashes are special.
                        Error lexQuotedContent(Word *w, WordPart *&tail, bool untilQuote) {
                                for (;;) {
                                        if (_pos >= _end) return untilQuote ? incomplete() : Error(Error::Ok);
                                        const char c = _src[_pos];
                                        Error      err;
                                        if (untilQuote && c == '"') {
                                                ++_pos;
                                                return Error::Ok;
                                        }
                                        if (c == '\\') {
                                                const char n = ch(_pos + 1);
                                                if (_pos + 1 >= _end && untilQuote) return incomplete();
                                                if (n == '\n') {
                                                        _pos += 2;
                                                        continue;
                                                }
                                                if (n == '$' || n == '`' || n == '\\' || (untilQuote && n == '"')) {
                                                        err = literal(w, tail, _pos + 1, 1, true);
                                                        _pos += 2;
                                                } else {
                                                        err = literal(w, tail, _pos, 1, true);
                                                        ++_pos;
                                                }
                                        } else if (c == '$') {
                                                err = lexDollar(w, tail, true);
                                        } else if (c == '`') {
                                                err = lexBackquote(w, tail, true);
                                        } else {
                                                const size_t s = _pos;
                                                while (_pos < _end && _src[_pos] != '\\' && _src[_pos] != '$' &&
                                                       _src[_pos] != '`' && !(untilQuote && _src[_pos] == '"'))
                                                        ++_pos;
                                                err = literal(w, tail, s, _pos - s, true);
                                        }
                                        if (err.isError()) return err;
                                }
                        }

                        Error lexDollar(Word *w, WordPart *&tail, bool quoted) {
                                const size_t p = _pos;
                                const char   n = ch(p + 1);
                                if (p + 1 >= _end) {
                                        ++_pos;
                                        return literal(w, tail, p, 1, quoted);
                                }
                                if (n == '(') {
                                        if (at(p + 2, '(')) {
                                                size_t inner = 0;
                                                bool   arith = false;
                                                Error  err = scanArith(p + 3, inner, arith);
                                                if (err.isError()) return err;
                                                if (arith) {
                                                        _pos = inner + 2;
                                                        return addPart(w, tail, WordPart::Kind::Arithmetic,
                                                                       _src.mid(p + 3, inner - (p + 3)), quoted);
                                                }
                                        }
                                        size_t inner = 0;
                                        Error  err = scanParen(p + 2, inner);
                                        if (err.isError()) return err;
                                        _pos = inner + 1;
                                        return addPart(w, tail, WordPart::Kind::CommandSubstitution,
                                                       _src.mid(p + 2, inner - (p + 2)), quoted);
                                }
                                if (n == '{') return lexBraceParam(w, tail, quoted);
                                if (isNameStart(n)) {
                                        size_t e = p + 1;
                                        while (e < _end && isNameChar(_src[e])) ++e;
                                        _pos = e;
                                        return addPart(w, tail, WordPart::Kind::Parameter, _src.mid(p + 1, e - p - 1),
                                                       quoted);
                                }
                                if (isDigit(n) || isSpecialParam(n)) {
                                        _pos = p + 2;
                                        return addPart(w, tail, WordPart::Kind::Parameter, _src.mid(p + 1, 1), quoted);
                                }
                                ++_pos;
                                return literal(w, tail, p, 1, quoted);
                        }

                        Error lexBraceParam(Word *w, WordPart *&tail, bool quoted) {
                                const size_t start = _pos;
                                size_t       i = _pos + 2;
                                bool         length = false;
                                if (at(i, '#') && i + 1 < _end && _src[i + 1] != '}' &&
                                    (isNameStart(_src[i + 1]) || isDigit(_src[i + 1]) || isSpecialParam(_src[i + 1]))) {
                                        length = true;
                                        ++i;
                                }
                                const size_t nameStart = i;
                                if (i < _end && isNameStart(_src[i])) {
                                        while (i < _end && isNameChar(_src[i])) ++i;
                                } else if (i < _end && isDigit(_src[i])) {
                                        while (i < _end && isDigit(_src[i])) ++i;
                                } else if (i < _end && isSpecialParam(_src[i])) {
                                        ++i;
                                }
                                if (i >= _end) return incomplete();
                                if (i == nameStart) return badSubstitution(start);
                                WordPart *part = nullptr;
                                Error     err = addPart(w, tail, WordPart::Kind::Parameter,
                                                        _src.mid(nameStart, i - nameStart), quoted, &part);
                                if (err.isError()) return err;
                                if (_src[i] == '}') {
                                        part->op = length ? ParamOp::Length : ParamOp::None;
                                        _pos = i + 1;
                                        return Error::Ok;
                                }
                                if (length) return badSubstitution(start);
                                if (_src[i] == ':') {
                                        part->colon = true;
                                        if (++i >= _end) return incomplete();
                                }
                                switch (_src[i]) {
                                        case '-': part->op = ParamOp::Default; break;
                                        case '=': part->op = ParamOp::Assign; break;
                                        case '?': part->op = ParamOp::Error; break;
                                        case '+': part->op = ParamOp::Alternative; break;
                                        case '#':
                                                if (part->colon) return badSubstitution(start);
                                                part->op = at(i + 1, '#') ? ParamOp::RemoveLargestPrefix
                                                                          : ParamOp::RemoveSmallestPrefix;
                                                if (part->op == ParamOp::RemoveLargestPrefix) ++i;
                                                break;
                                        case '%':
                                                if (part->colon) return badSubstitution(start);
                                                part->op = at(i + 1, '%') ? ParamOp::RemoveLargestSuffix
                                                                          : ParamOp::RemoveSmallestSuffix;
                                                if (part->op == ParamOp::RemoveLargestSuffix) ++i;
                                                break;
                                        default: return badSubstitution(start);
                                }
                                _pos = i + 1;
                                // A pattern isn't quoted by double quotes around the
                                // expansion, only by quotes inside it (as in bash).
                                const bool pattern = part->op >= ParamOp::RemoveSmallestPrefix;
                                return lexParamWord(part->arg, pattern ? false : quoted);
                        }

                        Error badSubstitution(size_t start) {
                                // Only once the closing brace has arrived: until then
                                // the expansion may just be incomplete.
                                size_t e = start;
                                while (e < _end && _src[e] != '}') ++e;
                                if (e >= _end) return incomplete();
                                _pos = start;
                                return fail(BadSubstitution, _src.mid(start, e + 1 - start));
                        }

                        // The word of `${x op word}`, up to the closing brace.
                        Error lexParamWord(Word *&arg, bool quoted) {
                                Word *a = _arena.create<Word>();
                                if (a == nullptr) return Error::NoMem;
                                arg = a;
                                WordPart *tail = nullptr;
                                for (;;) {
                                        if (_pos >= _end) return incomplete();
                                        const char c = _src[_pos];
                                        Error      err;
                                        if (c == '}') {
                                                ++_pos;
                                                return Error::Ok;
                                        }
                                        if (c == '\\') {
                                                if (_pos + 1 >= _end) return incomplete();
                                                if (_src[_pos + 1] == '\n') {
                                                        _pos += 2;
                                                        continue;
                                                }
                                                err = literal(a, tail, _pos + 1, 1, true);
                                                _pos += 2;
                                        } else if (c == '\'' && !quoted) {
                                                err = lexSingleQuoted(a, tail);
                                        } else if (c == '"') {
                                                err = lexDoubleQuoted(a, tail);
                                        } else if (c == '$') {
                                                err = lexDollar(a, tail, quoted);
                                        } else if (c == '`') {
                                                err = lexBackquote(a, tail, quoted);
                                        } else {
                                                // Inside double quotes a single quote is just a character.
                                                const size_t s = _pos;
                                                ++_pos;
                                                while (_pos < _end && !StringView("}\\'\"$`").contains(_src[_pos]))
                                                        ++_pos;
                                                err = literal(a, tail, s, _pos - s, quoted);
                                        }
                                        if (err.isError()) return err;
                                }
                        }

                        Error lexBackquote(Word *w, WordPart *&tail, bool quoted) {
                                // Inside backquotes, a backslash before $ ` \ (and " within
                                // double quotes) is removed.
                                size_t i = _pos + 1;
                                size_t escapes = 0;
                                while (i < _end && _src[i] != '`') {
                                        if (_src[i] == '\\' && i + 1 < _end) {
                                                const char n = _src[i + 1];
                                                if (n == '$' || n == '`' || n == '\\' || (quoted && n == '"'))
                                                        ++escapes;
                                                ++i;
                                        }
                                        ++i;
                                }
                                if (i >= _end) return incomplete();
                                const size_t start = _pos + 1;
                                StringView   text = _src.mid(start, i - start);
                                if (escapes > 0) {
                                        char *buf = static_cast<char *>(_arena.alloc(text.size(), 1));
                                        if (buf == nullptr) return Error::NoMem;
                                        size_t n = 0;
                                        for (size_t k = 0; k < text.size(); ++k) {
                                                const char nx = k + 1 < text.size() ? text[k + 1] : '\0';
                                                if (text[k] == '\\' &&
                                                    (nx == '$' || nx == '`' || nx == '\\' || (quoted && nx == '"')))
                                                        ++k;
                                                buf[n++] = text[k];
                                        }
                                        text = StringView(buf, n);
                                }
                                _pos = i + 1;
                                return addPart(w, tail, WordPart::Kind::CommandSubstitution, text, quoted);
                        }

                        // Skips a quoted string starting at @p i (the quote); returns the
                        // index after it in @p after.
                        Error skipQuoted(size_t i, size_t &after) {
                                const char q = _src[i];
                                ++i;
                                while (i < _end && _src[i] != q) {
                                        if (q != '\'' && _src[i] == '\\') {
                                                i += 2;
                                                continue;
                                        }
                                        if (q == '"' && _src[i] == '$' && at(i + 1, '(')) {
                                                size_t inner = 0;
                                                Error  err = scanParen(i + 2, inner);
                                                if (err.isError()) return err;
                                                i = inner + 1;
                                                continue;
                                        }
                                        ++i;
                                }
                                if (i >= _end) return incomplete();
                                after = i + 1;
                                return Error::Ok;
                        }

                        // From just after `$(`, finds the matching `)`. Counts
                        // parentheses outside quotes (a `case` pattern's unbalanced `)`
                        // inside `$(...)` is not supported).
                        Error scanParen(size_t i, size_t &inner) {
                                int depth = 1;
                                while (i < _end) {
                                        const char c = _src[i];
                                        if (c == '\\') {
                                                i += 2;
                                        } else if (c == '\'' || c == '"' || c == '`') {
                                                Error err = skipQuoted(i, i);
                                                if (err.isError()) return err;
                                        } else if (c == '(') {
                                                ++depth;
                                                ++i;
                                        } else if (c == ')') {
                                                if (--depth == 0) {
                                                        inner = i;
                                                        return Error::Ok;
                                                }
                                                ++i;
                                        } else {
                                                ++i;
                                        }
                                }
                                return incomplete();
                        }

                        // From just after `((`, finds the closing `))`. As in bash, if the
                        // first `)` at depth 0 isn't followed by another, it isn't
                        // arithmetic (@p arith false): `$((cmd) )` is a command substitution.
                        Error scanArith(size_t i, size_t &inner, bool &arith) {
                                int depth = 0;
                                while (i < _end) {
                                        const char c = _src[i];
                                        if (c == '\\') {
                                                i += 2;
                                                continue;
                                        }
                                        if (c == '\'' || c == '"' || c == '`') {
                                                Error err = skipQuoted(i, i);
                                                if (err.isError()) return err;
                                                continue;
                                        }
                                        if (c == '(') {
                                                ++depth;
                                        } else if (c == ')') {
                                                if (depth == 0) {
                                                        if (i + 1 >= _end) return incomplete();
                                                        arith = _src[i + 1] == ')';
                                                        inner = i;
                                                        return Error::Ok;
                                                }
                                                --depth;
                                        }
                                        ++i;
                                }
                                return incomplete();
                        }

                        Error readHereDocs() {
                                for (unsigned k = 0; k < _hereCount; ++k) {
                                        HereDoc &h = _hereDocs[k];
                                        Word    *body = _arena.create<Word>();
                                        if (body == nullptr) return Error::NoMem;
                                        body->hasQuotes = true;
                                        WordPart *tail = nullptr;
                                        for (;;) {
                                                if (_pos >= _end) return Error::NotReady;
                                                size_t nl = _pos;
                                                while (nl < _end && _src[nl] != '\n') ++nl;
                                                size_t cs = _pos;
                                                if (h.redir->stripTabs) {
                                                        while (cs < nl && _src[cs] == '\t') ++cs;
                                                }
                                                if (_src.mid(cs, nl - cs) == h.delimiter) {
                                                        _pos = nl < _end ? nl + 1 : _end;
                                                        break;
                                                }
                                                if (nl >= _end) return Error::NotReady;
                                                Error err;
                                                if (h.quoted) {
                                                        err = literal(body, tail, cs, nl + 1 - cs, true);
                                                } else {
                                                        // Expansions in the body; bounded to this line.
                                                        const size_t end = _end;
                                                        _end = nl + 1;
                                                        _pos = cs;
                                                        err = lexQuotedContent(body, tail, false);
                                                        _end = end;
                                                }
                                                if (err.isError()) return err;
                                                _pos = nl + 1;
                                        }
                                        h.redir->body = body;
                                }
                                _hereCount = 0;
                                return Error::Ok;
                        }

                        StringView   _src;
                        size_t       _pos;
                        size_t       _end;
                        Arena       &_arena;
                        SyntaxError &_error;
                        HereDoc      _hereDocs[MaxHereDocs];
                        unsigned     _hereCount = 0;
        };

} // namespace

// The grammar, over one complete command. A friend of Parser.
class ParserImpl {
        public:
                explicit ParserImpl(Parser &p) : _p(p), _lex(p._source, p._end, p._arena, p._error) {}

                Result<Node *> run() {
                        skipNewlines();
                        if (failed()) return finish(nullptr);
                        if (peek(true).kind == Tok::End) {
                                _p._end = _lex.position();
                                return makeResult<Node *>(nullptr);
                        }
                        Node *node = parseList();
                        if (node != nullptr) {
                                const Token &t = peek();
                                if (t.kind == Tok::Newline) {
                                        (void)take();
                                } else if (t.kind != Tok::End) {
                                        node = unexpected(t);
                                }
                        }
                        if (node != nullptr) _p._end = _lex.position();
                        return finish(node);
                }

        private:
                // Bounds recursion depth.
                class DepthGuard {
                        public:
                                explicit DepthGuard(ParserImpl &p) : _p(p) { ++_p._depth; }
                                ~DepthGuard() { --_p._depth; }
                                DepthGuard(const DepthGuard &) = delete;
                                DepthGuard &operator=(const DepthGuard &) = delete;

                        private:
                                ParserImpl &_p;
                };

                Result<Node *> finish(Node *node) {
                        if (_status.isError()) {
                                // Offset to line, for the message.
                                unsigned line = 1;
                                for (size_t i = 0; i < _p._error.offset && i < _p._source.size(); ++i)
                                        line += _p._source[i] == '\n';
                                _p._error.line = line;
                                return makeError<Node *>(_status);
                        }
                        return makeResult(node);
                }

                bool failed() const { return _status.isError(); }

                const Token &peek(bool commandStart = false) {
                        // Only `((` lexes differently in command position.
                        if (_have && (_haveMode == commandStart || _tok.kind != Tok::LParen)) return _tok;
                        if (_have) _lex.reset(_tok.start);
                        Error err = _lex.lex(_tok, commandStart);
                        if (err.isError()) {
                                if (_status.isOk()) _status = err;
                                _tok.kind = Tok::Error;
                        }
                        _have = true;
                        _haveMode = commandStart;
                        return _tok;
                }

                Token take() {
                        _have = false;
                        _lastEnd = _tok.end;
                        return _tok;
                }

                void skipNewlines() {
                        while (peek(true).kind == Tok::Newline) (void)take();
                        return;
                }

                std::nullptr_t fail(const char *format, StringView token, size_t offset) {
                        if (_status.isOk()) {
                                _status = Error::Invalid;
                                _p._error.format = format;
                                _p._error.token = token;
                                _p._error.offset = offset;
                        }
                        return nullptr;
                }

                std::nullptr_t noMem() {
                        if (_status.isOk()) _status = Error::NoMem;
                        return nullptr;
                }

                // A token that can't come here. At the end of input, the
                // command is just incomplete.
                std::nullptr_t unexpected(const Token &t) {
                        if (t.kind == Tok::Error) return nullptr;
                        if (t.kind == Tok::End) {
                                if (_status.isOk()) _status = Error::NotReady;
                                return nullptr;
                        }
                        return fail(UnexpectedToken, t.kind == Tok::Newline ? StringView("newline") : t.text, t.start);
                }

                static bool isWord(const Token &t, const char *word) {
                        return t.kind == Tok::Word && plainText(t.word) == StringView(word);
                }

                // Tokens that end a compound list.
                static bool isTerminator(const Token &t) {
                        if (t.kind == Tok::End || t.kind == Tok::RParen || t.kind == Tok::DSemi) return true;
                        if (t.kind != Tok::Word) return false;
                        const StringView w = plainText(t.word);
                        return w == "then" || w == "else" || w == "elif" || w == "fi" || w == "do" || w == "done" ||
                               w == "esac" || w == "}";
                }

                bool expect(const char *word) {
                        const Token &t = peek(true);
                        if (!isWord(t, word)) {
                                (void)unexpected(t);
                                return false;
                        }
                        (void)take();
                        return true;
                }

                template <typename T, typename... Args> T *make(Args &&...args) {
                        T *p = _p._arena.create<T>(static_cast<Args &&>(args)...);
                        if (p == nullptr) (void)noMem();
                        return p;
                }

                // An and-or list; with a `&` after it, a Background node around it (with its text,
                // which a subshell process parses again).
                Node *parseItem() {
                        const Token &first = peek(true);
                        if (first.kind == Tok::Error) return nullptr;
                        const size_t start = first.start;
                        Node        *n = parseAndOr();
                        if (n == nullptr || peek().kind != Tok::Amp) return n;
                        BackgroundNode *b = make<BackgroundNode>();
                        if (b == nullptr) return nullptr;
                        b->body = n;
                        b->source = _p._source.mid(start, _lastEnd - start);
                        b->hereDocAfter = _lex.pendingHereDocs() > 0;
                        (void)take();
                        return b;
                }

                // and_or (('&' | ';') and_or)* up to a newline or the end.
                Node *parseList() {
                        Node *first = parseItem();
                        if (first == nullptr) return nullptr;
                        ListNode *list = nullptr;
                        Node     *tail = first;
                        for (;;) {
                                // `&` separates as `;` does.
                                if (tail->kind != NodeKind::Background) {
                                        if (peek().kind != Tok::Semi) break;
                                        (void)take();
                                }
                                const Token &n = peek(true);
                                if (n.kind == Tok::Newline || n.kind == Tok::End) break;
                                Node *next = parseItem();
                                if (next == nullptr) return nullptr;
                                if (list == nullptr) {
                                        list = make<ListNode>();
                                        if (list == nullptr) return nullptr;
                                        list->items = first;
                                }
                                tail->next = next;
                                tail = next;
                        }
                        return list != nullptr ? list : first;
                }

                // The body of a compound command: and_or lists separated by ';' or
                // newlines, up to a terminator.
                Node *parseCompoundList() {
                        skipNewlines();
                        const Token &t = peek(true);
                        if (isTerminator(t)) return unexpected(t);
                        Node *first = parseItem();
                        if (first == nullptr) return nullptr;
                        ListNode *list = nullptr;
                        Node     *tail = first;
                        for (;;) {
                                if (tail->kind != NodeKind::Background) {
                                        const Token &s = peek();
                                        if (s.kind != Tok::Semi && s.kind != Tok::Newline) break;
                                        (void)take();
                                }
                                skipNewlines();
                                const Token &n = peek(true);
                                if (n.kind == Tok::Error) return nullptr;
                                if (isTerminator(n)) break;
                                Node *next = parseItem();
                                if (next == nullptr) return nullptr;
                                if (list == nullptr) {
                                        list = make<ListNode>();
                                        if (list == nullptr) return nullptr;
                                        list->items = first;
                                }
                                tail->next = next;
                                tail = next;
                        }
                        return list != nullptr ? list : first;
                }

                Node *parseAndOr() {
                        Node *left = parsePipeline();
                        while (left != nullptr) {
                                const Token &t = peek();
                                if (t.kind != Tok::AndIf && t.kind != Tok::OrIf) break;
                                const bool isAnd = t.kind == Tok::AndIf;
                                (void)take();
                                skipNewlines();
                                Node *right = parsePipeline();
                                if (right == nullptr) return nullptr;
                                AndOrNode *n = make<AndOrNode>();
                                if (n == nullptr) return nullptr;
                                n->left = left;
                                n->right = right;
                                n->isAnd = isAnd;
                                left = n;
                        }
                        return left;
                }

                Node *parsePipeline() {
                        bool negated = false;
                        if (isWord(peek(true), "!")) {
                                (void)take();
                                negated = true;
                        }
                        Node *first = parseCommand();
                        if (first == nullptr) return nullptr;
                        Node  *tail = first;
                        size_t count = 1;
                        for (;;) {
                                const Token &t = peek();
                                if (t.kind != Tok::Pipe && t.kind != Tok::PipeAmp) break;
                                if (t.kind == Tok::PipeAmp && !addStderrToPipe(tail)) return nullptr;
                                (void)take();
                                skipNewlines();
                                Node *next = parseCommand();
                                if (next == nullptr) return nullptr;
                                tail->next = next;
                                tail = next;
                                ++count;
                        }
                        if (count == 1 && !negated) return first;
                        PipelineNode *p = make<PipelineNode>();
                        if (p == nullptr) return nullptr;
                        p->commands = first;
                        p->negated = negated;
                        return p;
                }

                // `a |& b`: `2>&1` after a's own redirections.
                bool addStderrToPipe(Node *command) {
                        Redirection *r = make<Redirection>();
                        Word        *w = make<Word>();
                        WordPart    *p = make<WordPart>();
                        if (r == nullptr || w == nullptr || p == nullptr) return false;
                        p->text = StringView("1");
                        w->parts = p;
                        r->kind = Redirection::Kind::DupOutput;
                        r->fd = 2;
                        r->target = w;
                        Redirection **link = &command->redirects;
                        while (*link != nullptr) link = &(*link)->next;
                        *link = r;
                        return true;
                }

                Node *parseCommand() {
                        DepthGuard guard(*this);
                        if (_depth > Parser::MaxDepth) return fail("nesting too deep", StringView(), _tok.start);
                        const Token &t = peek(true);
                        Node        *node = nullptr;
                        switch (t.kind) {
                                case Tok::ArithCmd: {
                                        ArithNode *a = make<ArithNode>();
                                        if (a == nullptr) return nullptr;
                                        a->expression = take().arith;
                                        node = a;
                                        break;
                                }
                                case Tok::LParen: node = parseBody(NodeKind::Subshell); break;
                                case Tok::Word: {
                                        const StringView w = plainText(t.word);
                                        if (w == "{") {
                                                node = parseBody(NodeKind::Group);
                                        } else if (w == "if") {
                                                node = parseIf();
                                        } else if (w == "while" || w == "until") {
                                                node = parseLoop(w == "while" ? NodeKind::While : NodeKind::Until);
                                        } else if (w == "for") {
                                                node = parseFor();
                                        } else if (w == "case") {
                                                node = parseCase();
                                        } else if (w == "function") {
                                                return parseFunctionKeyword();
                                        } else if (isTerminator(t)) {
                                                return unexpected(t);
                                        } else {
                                                return parseSimple();
                                        }
                                        break;
                                }
                                default:
                                        if (isRedirect(t.kind)) return parseSimple();
                                        return unexpected(t);
                        }
                        if (node == nullptr) return nullptr;
                        return parseRedirects(node) ? node : nullptr;
                }

                bool parseRedirects(Node *node) {
                        Redirection **link = &node->redirects;
                        while (*link != nullptr) link = &(*link)->next;
                        while (isRedirect(peek().kind)) {
                                Redirection *r = parseRedirect();
                                if (r == nullptr) return false;
                                *link = r;
                                link = &r->next;
                        }
                        return !failed();
                }

                Redirection *parseRedirect() {
                        const Token  op = take();
                        Redirection *r = make<Redirection>();
                        if (r == nullptr) return nullptr;
                        r->fd = op.ioNumber;
                        switch (op.kind) {
                                case Tok::Less: r->kind = Redirection::Kind::Input; break;
                                case Tok::Great: r->kind = Redirection::Kind::Output; break;
                                case Tok::DGreat: r->kind = Redirection::Kind::Append; break;
                                case Tok::Clobber: r->kind = Redirection::Kind::Clobber; break;
                                case Tok::LessGreat: r->kind = Redirection::Kind::InputOutput; break;
                                case Tok::LessAnd: r->kind = Redirection::Kind::DupInput; break;
                                case Tok::GreatAnd: r->kind = Redirection::Kind::DupOutput; break;
                                case Tok::AndGreat: r->kind = Redirection::Kind::OutputBoth; break;
                                case Tok::AndDGreat: r->kind = Redirection::Kind::AppendBoth; break;
                                case Tok::DLess: r->kind = Redirection::Kind::HereDoc; break;
                                case Tok::DLessDash:
                                        r->kind = Redirection::Kind::HereDoc;
                                        r->stripTabs = true;
                                        break;
                                default: r->kind = Redirection::Kind::HereString; break;
                        }
                        const Token &t = peek();
                        if (t.kind != Tok::Word) {
                                // Even at the end of input: bash reports the newline.
                                if (t.kind == Tok::End) return fail(UnexpectedToken, StringView("newline"), t.start);
                                return unexpected(t);
                        }
                        const Token target = take();
                        r->target = target.word;
                        if (r->kind == Redirection::Kind::HereDoc) {
                                Error err = _lex.addHereDoc(r, target);
                                if (err.isError()) {
                                        if (_status.isOk()) _status = err;
                                        return nullptr;
                                }
                        }
                        return r;
                }

                Node *parseSimple() {
                        SimpleNode *n = make<SimpleNode>();
                        if (n == nullptr) return nullptr;
                        Assignment  *lastAssign = nullptr;
                        Word        *lastWord = nullptr;
                        Redirection *lastRedir = nullptr;
                        for (;;) {
                                const Token &t = peek();
                                if (isRedirect(t.kind)) {
                                        Redirection *r = parseRedirect();
                                        if (r == nullptr) return nullptr;
                                        if (lastRedir == nullptr) {
                                                n->redirects = r;
                                        } else {
                                                lastRedir->next = r;
                                        }
                                        lastRedir = r;
                                } else if (t.kind == Tok::Word) {
                                        if (n->words == nullptr) {
                                                Assignment *a = nullptr;
                                                if (!toAssignment(t.word, a)) return nullptr;
                                                if (a != nullptr) {
                                                        (void)take();
                                                        if (lastAssign == nullptr) {
                                                                n->assignments = a;
                                                        } else {
                                                                lastAssign->next = a;
                                                        }
                                                        lastAssign = a;
                                                        continue;
                                                }
                                        }
                                        const Token w = take();
                                        const bool  first = n->words == nullptr;
                                        if (first) {
                                                n->words = w.word;
                                        } else {
                                                lastWord->next = w.word;
                                        }
                                        lastWord = w.word;
                                        // `name()`: a function definition.
                                        if (first && n->assignments == nullptr && n->redirects == nullptr &&
                                            peek().kind == Tok::LParen) {
                                                return parseFunctionRest(w);
                                        }
                                } else {
                                        break;
                                }
                        }
                        return failed() ? nullptr : n;
                }

                // `NAME=...` as an assignment, or @p out null if the word isn't one.
                bool toAssignment(Word *w, Assignment *&out) {
                        out = nullptr;
                        const WordPart *first = w->parts;
                        if (first == nullptr || first->kind != WordPart::Kind::Literal || first->quoted) return true;
                        const size_t eq = first->text.indexOf('=');
                        if (eq == StringView::NPos || !isValidName(first->text.left(eq))) return true;
                        Assignment *a = make<Assignment>();
                        Word       *value = make<Word>();
                        if (a == nullptr || value == nullptr) return false;
                        a->name = first->text.left(eq);
                        a->value = value;
                        value->hasQuotes = w->hasQuotes;
                        const StringView rest = first->text.mid(eq + 1);
                        if (!rest.isEmpty()) {
                                WordPart *p = make<WordPart>();
                                if (p == nullptr) return false;
                                p->text = rest;
                                p->next = first->next;
                                value->parts = p;
                        } else {
                                value->parts = first->next;
                        }
                        out = a;
                        return true;
                }

                // After `name`: `() body`.
                Node *parseFunctionRest(const Token &name) {
                        const StringView n = plainText(name.word);
                        if (n.isEmpty()) return fail(UnexpectedToken, StringView("("), peek().start);
                        (void)take(); // (
                        const Token &close = peek();
                        if (close.kind != Tok::RParen) return unexpected(close);
                        (void)take();
                        return parseFunctionBody(n);
                }

                Node *parseFunctionKeyword() {
                        (void)take(); // function
                        const Token &t = peek();
                        if (t.kind != Tok::Word) return unexpected(t);
                        const StringView name = plainText(t.word);
                        if (name.isEmpty()) return fail(UnexpectedToken, t.text, t.start);
                        (void)take();
                        if (peek().kind == Tok::LParen) {
                                (void)take();
                                const Token &close = peek();
                                if (close.kind != Tok::RParen) return unexpected(close);
                                (void)take();
                        }
                        return parseFunctionBody(name);
                }

                Node *parseFunctionBody(StringView name) {
                        skipNewlines();
                        const Token first = peek(true);
                        if (first.kind == Tok::Error) return nullptr;
                        const size_t start = first.start;
                        Node        *body = parseCommand();
                        if (body == nullptr) return nullptr;
                        if (body->kind == NodeKind::Simple || body->kind == NodeKind::Pipeline ||
                            body->kind == NodeKind::Function) {
                                return fail(UnexpectedToken, first.text, start);
                        }
                        FunctionNode *f = make<FunctionNode>();
                        if (f == nullptr) return nullptr;
                        f->name = name;
                        f->body = body;
                        f->source = _p._source.mid(start, _lastEnd - start);
                        return f;
                }

                Node *parseBody(NodeKind kind) {
                        (void)take(); // `{` or `(`
                        BodyNode *n = make<BodyNode>(kind);
                        if (n == nullptr) return nullptr;
                        n->body = parseCompoundList();
                        if (n->body == nullptr) return nullptr;
                        if (kind == NodeKind::Group) return expect("}") ? n : nullptr;
                        const Token &t = peek();
                        if (t.kind != Tok::RParen) return unexpected(t);
                        (void)take();
                        return n;
                }

                Node *parseIf() {
                        (void)take(); // if
                        IfNode *n = parseIfTail();
                        if (n == nullptr) return nullptr;
                        return expect("fi") ? n : nullptr;
                }

                // condition; then body; [elif ... | else body] -- without the fi.
                IfNode *parseIfTail() {
                        IfNode *n = make<IfNode>();
                        if (n == nullptr) return nullptr;
                        if ((n->condition = parseCompoundList()) == nullptr) return nullptr;
                        if (!expect("then")) return nullptr;
                        if ((n->thenPart = parseCompoundList()) == nullptr) return nullptr;
                        const Token &t = peek(true);
                        if (isWord(t, "elif")) {
                                (void)take();
                                if ((n->elsePart = parseIfTail()) == nullptr) return nullptr;
                        } else if (isWord(t, "else")) {
                                (void)take();
                                if ((n->elsePart = parseCompoundList()) == nullptr) return nullptr;
                        }
                        return n;
                }

                Node *parseLoop(NodeKind kind) {
                        (void)take(); // while / until
                        LoopNode *n = make<LoopNode>(kind);
                        if (n == nullptr) return nullptr;
                        if ((n->condition = parseCompoundList()) == nullptr) return nullptr;
                        return parseDoGroup(n->body) ? n : nullptr;
                }

                bool parseDoGroup(Node *&body) {
                        if (!expect("do")) return false;
                        if ((body = parseCompoundList()) == nullptr) return false;
                        return expect("done");
                }

                Node *parseFor() {
                        (void)take(); // for
                        const Token &t = peek(true);
                        if (t.kind == Tok::ArithCmd) return parseArithFor();
                        if (t.kind != Tok::Word) return unexpected(t);
                        // The name is checked when the loop runs, as bash does.
                        const StringView name = plainText(t.word).isEmpty() ? t.text : plainText(t.word);
                        (void)take();
                        ForNode *n = make<ForNode>();
                        if (n == nullptr) return nullptr;
                        n->variable = name;
                        skipNewlines();
                        if (isWord(peek(), "in")) {
                                (void)take();
                                n->hasIn = true;
                                Word *last = nullptr;
                                for (;;) {
                                        const Token &w = peek();
                                        if (w.kind != Tok::Word) break;
                                        Word *word = take().word;
                                        if (last == nullptr) {
                                                n->words = word;
                                        } else {
                                                last->next = word;
                                        }
                                        last = word;
                                }
                                const Token &sep = peek();
                                if (sep.kind != Tok::Semi && sep.kind != Tok::Newline) return unexpected(sep);
                                (void)take();
                        } else if (peek().kind == Tok::Semi) {
                                (void)take();
                        }
                        skipNewlines();
                        return parseDoGroup(n->body) ? n : nullptr;
                }

                Node *parseArithFor() {
                        const Token   t = take();
                        ArithForNode *n = make<ArithForNode>();
                        if (n == nullptr) return nullptr;
                        // Split at the two top-level semicolons.
                        StringView parts[3];
                        size_t     count = 0, start = 0;
                        int        depth = 0;
                        for (size_t i = 0; i <= t.arith.size(); ++i) {
                                const char c = i < t.arith.size() ? t.arith[i] : ';';
                                if (c == '(') ++depth;
                                if (c == ')') --depth;
                                if (c == ';' && depth == 0) {
                                        if (count == 3) break;
                                        parts[count++] = t.arith.mid(start, i - start).trimmed();
                                        start = i + 1;
                                }
                        }
                        if (count != 3 || start <= t.arith.size()) {
                                return fail("syntax error: arithmetic expression required", t.text, t.start);
                        }
                        n->init = parts[0];
                        n->condition = parts[1];
                        n->step = parts[2];
                        if (peek().kind == Tok::Semi) (void)take();
                        skipNewlines();
                        return parseDoGroup(n->body) ? n : nullptr;
                }

                Node *parseCase() {
                        (void)take(); // case
                        const Token &s = peek();
                        if (s.kind != Tok::Word) return unexpected(s);
                        CaseNode *n = make<CaseNode>();
                        if (n == nullptr) return nullptr;
                        n->subject = take().word;
                        skipNewlines();
                        if (!expect("in")) return nullptr;
                        skipNewlines();
                        CaseItem *last = nullptr;
                        for (;;) {
                                const Token &t = peek();
                                if (isWord(t, "esac")) {
                                        (void)take();
                                        return n;
                                }
                                if (t.kind == Tok::LParen) (void)take();
                                CaseItem *item = make<CaseItem>();
                                if (item == nullptr) return nullptr;
                                Word *lastPattern = nullptr;
                                for (;;) {
                                        const Token &p = peek();
                                        if (p.kind != Tok::Word) return unexpected(p);
                                        Word *w = take().word;
                                        if (lastPattern == nullptr) {
                                                item->patterns = w;
                                        } else {
                                                lastPattern->next = w;
                                        }
                                        lastPattern = w;
                                        if (peek().kind != Tok::Pipe) break;
                                        (void)take();
                                }
                                const Token &close = peek();
                                if (close.kind != Tok::RParen) return unexpected(close);
                                (void)take();
                                skipNewlines();
                                const Token &b = peek(true);
                                if (b.kind != Tok::DSemi && !isWord(b, "esac")) {
                                        if ((item->body = parseCompoundList()) == nullptr) return nullptr;
                                }
                                if (last == nullptr) {
                                        n->items = item;
                                } else {
                                        last->next = item;
                                }
                                last = item;
                                const Token &e = peek(true);
                                if (e.kind == Tok::DSemi) {
                                        (void)take();
                                        skipNewlines();
                                } else if (!isWord(e, "esac")) {
                                        return unexpected(e);
                                }
                        }
                }

                Parser  &_p;
                Lexer    _lex;
                Token    _tok;
                bool     _have = false;
                bool     _haveMode = false;
                Error    _status;
                unsigned _depth = 0;
                size_t   _lastEnd = 0;
};

Parser::Parser(StringView source, Arena &arena) : _source(source), _arena(arena) {}

Result<Word *> parseQuotedText(StringView text, Arena &arena, SyntaxError &error) {
        error = SyntaxError();
        Lexer lexer(text, 0, arena, error);
        Word *word = nullptr;
        Error err = lexer.lexQuotedText(word);
        if (err == Error::NotReady) {
                error.format = "unexpected end of expression";
                err = Error::Invalid;
        }
        return Result<Word *>(word, err);
}

Error appendSyntaxError(const SyntaxError &error, String &out) {
        if (error.format == nullptr) return out.append("syntax error");
        if (StringView(error.format).contains("%.*s"))
                return out.appendf(error.format, static_cast<int>(error.token.size()), error.token.data());
        return out.append(error.format);
}

Result<Node *> Parser::next() {
        _error = SyntaxError();
        ParserImpl impl(*this);
        return impl.run();
}

BMDS_NAMESPACE_END
