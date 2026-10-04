/**
 * @file      pipeline.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/pipeline.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr const char *SyntaxError = "syntax error near unexpected token";
        constexpr const char *NotSupported = "unsupported operator";

        bool isSeparator(char c) {
                return c == ' ' || c == '\t';
        }

        bool isOperator(char c) {
                return c == '|' || c == '&' || c == ';' || c == '<' || c == '>';
        }

        enum class TokenKind : uint8_t {
                Word,
                Pipe,
                PipeBoth,
                Redirect
        };

        // Redirection operators before their target is known.
        enum class RedirectOp : uint8_t {
                In,
                Out,
                Append,
                DupIn,
                DupOut,
                OutBoth,
                AppendBoth
        };

        struct Token {
                        TokenKind  kind = TokenKind::Word;
                        RedirectOp op = RedirectOp::Out;
                        int        fd = -1; // explicit descriptor before a redirection, or -1
                        char      *text = nullptr;
        };

        bool isNumber(const char *s, int &value) {
                if (*s == '\0') return false;
                long v = 0;
                for (; *s != '\0'; ++s) {
                        if (*s < '0' || *s > '9') return false;
                        v = v * 10 + (*s - '0');
                        if (v > 9999) return false;
                }
                value = static_cast<int>(v);
                return true;
        }

        class Lexer {
                public:
                        Lexer(char *line, List<Token> &tokens, ParseError &error)
                            : _in(line), _out(line), _tokens(tokens), _error(error) {}

                        Error run() {
                                for (;;) {
                                        while (isSeparator(*_in)) ++_in;
                                        if (*_in == '\0' || *_in == '#') return Error::Ok;
                                        Error err = isOperator(*_in) ? lexOperator(*_in, -1) : lexWord();
                                        if (err.isError()) return err;
                                }
                        }

                private:
                        // `_in` reads and `_out` writes the same buffer. Quotes and
                        // escapes only ever drop characters and operators aren't
                        // copied, so `_out` never overtakes `_in`.
                        Error lexWord() {
                                char *start = _out;
                                bool  digitsOnly = true;
                                while (*_in != '\0' && !isSeparator(*_in) && !isOperator(*_in)) {
                                        if (*_in == '"') {
                                                digitsOnly = false;
                                                ++_in;
                                                while (*_in != '\0' && *_in != '"') {
                                                        if (*_in == '\\' && (_in[1] == '"' || _in[1] == '\\')) ++_in;
                                                        *_out++ = *_in++;
                                                }
                                                if (*_in == '\0') return unterminated();
                                                ++_in;
                                        } else if (*_in == '\'') {
                                                digitsOnly = false;
                                                ++_in;
                                                while (*_in != '\0' && *_in != '\'') *_out++ = *_in++;
                                                if (*_in == '\0') return unterminated();
                                                ++_in;
                                        } else if (*_in == '\\' && _in[1] != '\0') {
                                                digitsOnly = false;
                                                ++_in;
                                                *_out++ = *_in++;
                                        } else {
                                                if (*_in < '0' || *_in > '9') digitsOnly = false;
                                                *_out++ = *_in++;
                                        }
                                }
                                // Read what ended the word before terminating it: `_out`
                                // may equal `_in` here.
                                const char next = *_in;
                                *_out++ = '\0';
                                if (digitsOnly && (next == '<' || next == '>')) {
                                        // `2>`: the number names the descriptor; it isn't a word.
                                        int fd = 0;
                                        if (isNumber(start, fd)) {
                                                _out = start;
                                                return lexOperator(next, fd);
                                        }
                                }
                                Token t;
                                t.text = start;
                                Error err = _tokens.append(t);
                                if (err.isError()) return err;
                                // The terminator may have overwritten `next`, so act on the copy.
                                if (isSeparator(next)) ++_in;
                                if (isOperator(next)) return lexOperator(next, -1);
                                return Error::Ok;
                        }

                        // @p first is `*_in`, passed separately because terminating the
                        // previous word may have overwritten it.
                        Error lexOperator(char first, int fd) {
                                Token t;
                                t.kind = TokenKind::Redirect;
                                t.fd = fd;
                                const char second = _in[1];
                                size_t     length = 1;
                                switch (first) {
                                        case '|':
                                                if (second == '|') return unsupported("||");
                                                t.kind = second == '&' ? TokenKind::PipeBoth : TokenKind::Pipe;
                                                length = second == '&' ? 2 : 1;
                                                break;
                                        case '&':
                                                if (second == '&') return unsupported("&&");
                                                if (second != '>') return unsupported("&");
                                                t.op = _in[2] == '>' ? RedirectOp::AppendBoth : RedirectOp::OutBoth;
                                                length = _in[2] == '>' ? 3 : 2;
                                                break;
                                        case ';': return unsupported(";");
                                        case '<':
                                                if (second == '<') return unsupported("<<");
                                                t.op = second == '&' ? RedirectOp::DupIn : RedirectOp::In;
                                                length = second == '&' ? 2 : 1;
                                                break;
                                        default: // '>'
                                                if (second == '>') {
                                                        t.op = RedirectOp::Append;
                                                        length = 2;
                                                } else if (second == '&') {
                                                        t.op = RedirectOp::DupOut;
                                                        length = 2;
                                                } else {
                                                        t.op = RedirectOp::Out;
                                                        length = second == '|' ? 2 : 1;
                                                }
                                                break;
                                }
                                if ((first == '|' || first == '&') && fd >= 0) return syntax(first == '|' ? "|" : "&");
                                _in += length;
                                return _tokens.append(t);
                        }

                        Error unterminated() {
                                _error.message = "unterminated quote";
                                _error.token = nullptr;
                                return Error::Invalid;
                        }

                        Error unsupported(const char *token) {
                                _error.message = NotSupported;
                                _error.token = token;
                                return Error::Invalid;
                        }

                        Error syntax(const char *token) {
                                _error.message = SyntaxError;
                                _error.token = token;
                                return Error::Invalid;
                        }

                        const char  *_in;
                        char        *_out;
                        List<Token> &_tokens;
                        ParseError  &_error;
        };

        const char *opText(RedirectOp op) {
                switch (op) {
                        case RedirectOp::In: return "<";
                        case RedirectOp::Out: return ">";
                        case RedirectOp::Append: return ">>";
                        case RedirectOp::DupIn: return "<&";
                        case RedirectOp::DupOut: return ">&";
                        case RedirectOp::OutBoth: return "&>";
                        case RedirectOp::AppendBoth: return "&>>";
                }
                return "?";
        }

        Error addFileRedirect(Stage &stage, Redirect::Kind kind, int fd, const char *path, bool isInput) {
                Redirect r;
                r.kind = kind;
                r.fd = fd;
                r.path = path;
                r.isInput = isInput;
                return stage.redirects.append(r);
        }

        Error addDuplicate(Stage &stage, int fd, int sourceFd, bool isInput) {
                Redirect r;
                r.kind = Redirect::Kind::Duplicate;
                r.fd = fd;
                r.sourceFd = sourceFd;
                r.isInput = isInput;
                return stage.redirects.append(r);
        }

        // Adds the redirections for @p t with target @p word.
        Error addRedirect(Stage &stage, const Token &t, char *word, ParseError &error) {
                const bool input = t.op == RedirectOp::In || t.op == RedirectOp::DupIn;
                const int  fd = t.fd >= 0 ? t.fd : (input ? 0 : 1);
                int        source = 0;
                switch (t.op) {
                        case RedirectOp::In: return addFileRedirect(stage, Redirect::Kind::Input, fd, word, true);
                        case RedirectOp::Out: return addFileRedirect(stage, Redirect::Kind::Output, fd, word, false);
                        case RedirectOp::Append: return addFileRedirect(stage, Redirect::Kind::Append, fd, word, false);
                        case RedirectOp::DupIn:
                        case RedirectOp::DupOut:
                                if (word[0] == '-' && word[1] == '\0') {
                                        Redirect r;
                                        r.kind = Redirect::Kind::Close;
                                        r.fd = fd;
                                        r.isInput = input;
                                        return stage.redirects.append(r);
                                }
                                if (isNumber(word, source)) return addDuplicate(stage, fd, source, input);
                                if (t.op == RedirectOp::DupOut && t.fd < 0) {
                                        // `>&file` is bash's older spelling of `&>file`.
                                        Error err = addFileRedirect(stage, Redirect::Kind::Output, 1, word, false);
                                        return err.isError() ? err : addDuplicate(stage, 2, 1, false);
                                }
                                error.message = "ambiguous redirect";
                                error.token = word;
                                return Error::Invalid;
                        case RedirectOp::OutBoth:
                        case RedirectOp::AppendBoth: {
                                const Redirect::Kind kind =
                                        t.op == RedirectOp::OutBoth ? Redirect::Kind::Output : Redirect::Kind::Append;
                                Error err = addFileRedirect(stage, kind, 1, word, false);
                                return err.isError() ? err : addDuplicate(stage, 2, 1, false);
                        }
                }
                return Error::Invalid;
        }

} // namespace

Error parsePipeline(char *line, Pipeline &out, ParseError &error) {
        out.stages.clear();
        List<Token> tokens;
        Error       err = Lexer(line, tokens, error).run();
        if (err.isError()) return err;
        if (tokens.isEmpty()) return Error::Ok;

        if ((err = out.stages.append(Stage())).isError()) return err;
        for (size_t i = 0; i < tokens.size(); ++i) {
                const Token &t = tokens[i];
                Stage       &stage = out.stages.back();
                switch (t.kind) {
                        case TokenKind::Word: err = stage.args.append(t.text); break;
                        case TokenKind::Redirect:
                                if (i + 1 == tokens.size() || tokens[i + 1].kind != TokenKind::Word) {
                                        error.message = SyntaxError;
                                        error.token = i + 1 == tokens.size() ? "newline" : "|";
                                        if (i + 1 < tokens.size() && tokens[i + 1].kind == TokenKind::Redirect)
                                                error.token = opText(tokens[i + 1].op);
                                        return Error::Invalid;
                                }
                                err = addRedirect(stage, t, tokens[++i].text, error);
                                break;
                        case TokenKind::Pipe:
                        case TokenKind::PipeBoth:
                                if (stage.args.isEmpty() && stage.redirects.isEmpty()) {
                                        error.message = SyntaxError;
                                        error.token = t.kind == TokenKind::Pipe ? "|" : "|&";
                                        return Error::Invalid;
                                }
                                // `|&` is `2>&1 |`, applied after the stage's own redirections.
                                if (t.kind == TokenKind::PipeBoth) err = addDuplicate(stage, 2, 1, false);
                                if (err.isOk()) err = out.stages.append(Stage());
                                break;
                }
                if (err.isError()) return err;
        }
        const Stage &last = out.stages.back();
        if (last.args.isEmpty() && last.redirects.isEmpty()) {
                error.message = SyntaxError;
                error.token = "newline";
                return Error::Invalid;
        }
        return Error::Ok;
}

BMDS_NAMESPACE_END
