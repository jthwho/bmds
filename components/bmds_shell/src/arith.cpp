/**
 * @file      arith.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstdio>
#include <bmds/arith.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr int MaxRecursion = 10;

        bool isNameStart(char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        }

        bool isNameChar(char c) {
                return isNameStart(c) || (c >= '0' && c <= '9');
        }

        // A value, and the variable it came from (when it is an lvalue).
        struct Value {
                        int64_t    v = 0;
                        StringView name;
        };

        int64_t wrap(uint64_t u) {
                return static_cast<int64_t>(u);
        }

        class Evaluator {
                public:
                        Evaluator(StringView s, Variables &vars, Arith::Failure &failure, int depth)
                            : _s(s), _vars(vars), _failure(failure), _depth(depth) {}

                        Error run(int64_t &out) {
                                skipSpace();
                                if (_pos == _s.size()) {
                                        out = 0;
                                        return Error::Ok;
                                }
                                Value v;
                                if (!comma(v)) return _err;
                                if (_pos != _s.size()) return fail("syntax error in expression");
                                out = v.v;
                                return Error::Ok;
                        }

                private:
                        Error fail(const char *message) {
                                if (_err.isOk()) {
                                        _err = Error::Invalid;
                                        _failure.message = message;
                                        _failure.token = _s.mid(_pos);
                                }
                                return _err;
                        }

                        bool failed(const char *message) {
                                (void)fail(message);
                                return false;
                        }

                        void skipSpace() {
                                while (_pos < _s.size() &&
                                       (_s[_pos] == ' ' || _s[_pos] == '\t' || _s[_pos] == '\n' || _s[_pos] == '\r'))
                                        ++_pos;
                                return;
                        }

                        // Consumes @p op if it comes next (and isn't the start of a longer
                        // operator that @p notFollowedBy names).
                        bool accept(const char *op, const char *notFollowedBy = "") {
                                skipSpace();
                                const StringView o(op);
                                if (!_s.mid(_pos).startsWith(o)) return false;
                                const char next = _pos + o.size() < _s.size() ? _s[_pos + o.size()] : '\0';
                                if (next != '\0' && StringView(notFollowedBy).contains(next)) return false;
                                _pos += o.size();
                                return true;
                        }

                        bool load(Value &v) {
                                if (v.name.isEmpty()) return true;
                                const String *text = _vars.value(v.name);
                                if (text == nullptr || text->isEmpty()) {
                                        v.v = 0;
                                        return true;
                                }
                                if (_depth >= MaxRecursion) return failed("expression recursion level exceeded");
                                Evaluator inner(*text, _vars, _failure, _depth + 1);
                                int64_t   out = 0;
                                Error     err = inner.run(out);
                                if (err.isError()) {
                                        _err = err;
                                        return false;
                                }
                                v.v = out;
                                return true;
                        }

                        bool store(const Value &target, int64_t value) {
                                if (target.name.isEmpty()) return failed("attempted assignment to non-variable");
                                if (_skip > 0) return true;
                                char buf[24];
                                std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(value));
                                Error err = _vars.set(target.name, buf);
                                if (err.isError()) {
                                        _err = err;
                                        if (err == Error::ReadOnly) {
                                                _failure.message = "readonly variable";
                                                _failure.token = target.name;
                                        }
                                        return false;
                                }
                                return true;
                        }

                        bool comma(Value &v) {
                                if (!assign(v)) return false;
                                while (accept(",")) {
                                        if (!assign(v)) return false;
                                }
                                v.name = StringView();
                                return true;
                        }

                        bool assign(Value &v) {
                                if (!ternary(v)) return false;
                                static const char *const ops[] = {
                                        "<<=", ">>=", "+=", "-=", "*=", "/=", "%=", "&=", "^=", "|="};
                                const char *op = nullptr;
                                for (const char *o : ops) {
                                        if (accept(o)) {
                                                op = o;
                                                break;
                                        }
                                }
                                if (op == nullptr && !accept("=", "=")) return true;
                                const Value target = v;
                                Value       rhs;
                                if (!assign(rhs)) return false;
                                int64_t result = rhs.v;
                                if (op != nullptr) {
                                        Value cur = target;
                                        if (!load(cur)) return false;
                                        if (!binary(StringView(op).left(StringView(op).size() - 1), cur.v, rhs.v,
                                                    result))
                                                return false;
                                }
                                if (!store(target, result)) return false;
                                v.v = result;
                                v.name = StringView();
                                return true;
                        }

                        bool ternary(Value &v) {
                                if (!logicalOr(v)) return false;
                                if (!accept("?")) return true;
                                const bool cond = v.v != 0;
                                Value      a, b;
                                if (!cond) ++_skip;
                                const bool okA = comma(a);
                                if (!cond) --_skip;
                                if (!okA) return false;
                                if (!accept(":")) return failed("`:' expected for conditional expression");
                                if (cond) ++_skip;
                                const bool okB = assign(b);
                                if (cond) --_skip;
                                if (!okB) return false;
                                v.v = cond ? a.v : b.v;
                                v.name = StringView();
                                return true;
                        }

                        bool logicalOr(Value &v) {
                                if (!logicalAnd(v)) return false;
                                while (accept("||")) {
                                        const bool left = v.v != 0;
                                        Value      r;
                                        if (left) ++_skip;
                                        const bool ok = logicalAnd(r);
                                        if (left) --_skip;
                                        if (!ok) return false;
                                        v.v = left || r.v != 0;
                                        v.name = StringView();
                                }
                                return true;
                        }

                        bool logicalAnd(Value &v) {
                                if (!bitOr(v)) return false;
                                while (accept("&&")) {
                                        const bool left = v.v != 0;
                                        Value      r;
                                        if (!left) ++_skip;
                                        const bool ok = bitOr(r);
                                        if (!left) --_skip;
                                        if (!ok) return false;
                                        v.v = left && r.v != 0;
                                        v.name = StringView();
                                }
                                return true;
                        }

                        // Left-associative binary levels, tightest last.
                        bool bitOr(Value &v) { return level(v, 0); }

                        bool level(Value &v, int n) {
                                // Operators per level; each entry lists operators to reject
                                // as prefixes of longer ones.
                                struct Op {
                                                const char *text;
                                                const char *notFollowedBy;
                                };
                                static const Op levels[][4] = {
                                        {{"|", "|="}, {nullptr, nullptr}},
                                        {{"^", "="}, {nullptr, nullptr}},
                                        {{"&", "&="}, {nullptr, nullptr}},
                                        {{"==", ""}, {"!=", ""}, {nullptr, nullptr}},
                                        {{"<=", ""}, {">=", ""}, {"<", "<="}, {">", ">="}},
                                        {{"<<", "="}, {">>", "="}, {nullptr, nullptr}},
                                        {{"+", "+="}, {"-", "-="}, {nullptr, nullptr}},
                                        {{"*", "*="}, {"/", "="}, {"%", "="}, {nullptr, nullptr}},
                                };
                                constexpr int Levels = sizeof(levels) / sizeof(levels[0]);
                                if (n == Levels) return power(v);
                                if (!level(v, n + 1)) return false;
                                for (;;) {
                                        const char *op = nullptr;
                                        for (const Op &o : levels[n]) {
                                                if (o.text == nullptr) break;
                                                if (accept(o.text, o.notFollowedBy)) {
                                                        op = o.text;
                                                        break;
                                                }
                                        }
                                        if (op == nullptr) return true;
                                        if (!load(v)) return false;
                                        Value r;
                                        if (!level(r, n + 1) || !load(r)) return false;
                                        int64_t result = 0;
                                        if (!binary(op, v.v, r.v, result)) return false;
                                        v.v = result;
                                        v.name = StringView();
                                }
                        }

                        bool binary(StringView op, int64_t a, int64_t b, int64_t &out) {
                                const uint64_t ua = static_cast<uint64_t>(a), ub = static_cast<uint64_t>(b);
                                if (op == "+") {
                                        out = wrap(ua + ub);
                                } else if (op == "-") {
                                        out = wrap(ua - ub);
                                } else if (op == "*") {
                                        out = wrap(ua * ub);
                                } else if (op == "/" || op == "%") {
                                        if (b == 0) {
                                                if (_skip > 0) {
                                                        out = 0;
                                                        return true;
                                                }
                                                return failed("division by 0");
                                        }
                                        if (a == INT64_MIN && b == -1) {
                                                out = op == "/" ? INT64_MIN : 0;
                                        } else {
                                                out = op == "/" ? a / b : a % b;
                                        }
                                } else if (op == "<<") {
                                        out = wrap(ua << (ub & 63));
                                } else if (op == ">>") {
                                        out = a >> (ub & 63);
                                } else if (op == "&") {
                                        out = a & b;
                                } else if (op == "|") {
                                        out = a | b;
                                } else if (op == "^") {
                                        out = a ^ b;
                                } else if (op == "==") {
                                        out = a == b;
                                } else if (op == "!=") {
                                        out = a != b;
                                } else if (op == "<") {
                                        out = a < b;
                                } else if (op == "<=") {
                                        out = a <= b;
                                } else if (op == ">") {
                                        out = a > b;
                                } else if (op == ">=") {
                                        out = a >= b;
                                } else {
                                        return failed("syntax error in expression");
                                }
                                return true;
                        }

                        // `**` is right-associative and binds tighter than `*`, looser than unary minus.
                        bool power(Value &v) {
                                if (!unary(v)) return false;
                                if (!accept("**")) return true;
                                if (!load(v)) return false;
                                Value e;
                                if (!power(e) || !load(e)) return false;
                                if (e.v < 0) {
                                        if (_skip > 0) return true;
                                        return failed("exponent less than 0");
                                }
                                uint64_t result = 1, base = static_cast<uint64_t>(v.v);
                                for (int64_t k = 0; k < e.v; ++k) result *= base;
                                v.v = wrap(result);
                                v.name = StringView();
                                return true;
                        }

                        bool unary(Value &v) {
                                if (accept("++") || accept("--")) {
                                        const bool inc = _s[_pos - 1] == '+';
                                        if (!unary(v)) return false;
                                        Value cur = v;
                                        if (!load(cur)) return false;
                                        const int64_t result =
                                                wrap(static_cast<uint64_t>(cur.v) + (inc ? 1ull : ~0ull));
                                        if (!store(v, result)) return false;
                                        v.v = result;
                                        v.name = StringView();
                                        return true;
                                }
                                if (accept("-", "=") || accept("+", "=") || accept("!", "=") || accept("~")) {
                                        const char op = _s[_pos - 1];
                                        if (!unary(v) || !load(v)) return false;
                                        if (op == '-') v.v = wrap(0ull - static_cast<uint64_t>(v.v));
                                        if (op == '!') v.v = v.v == 0;
                                        if (op == '~') v.v = ~v.v;
                                        v.name = StringView();
                                        return true;
                                }
                                return postfix(v);
                        }

                        bool postfix(Value &v) {
                                if (!primary(v)) return false;
                                if (!v.name.isEmpty() && (accept("++") || accept("--"))) {
                                        const bool inc = _s[_pos - 1] == '+';
                                        Value      cur = v;
                                        if (!load(cur)) return false;
                                        if (!store(v, wrap(static_cast<uint64_t>(cur.v) + (inc ? 1ull : ~0ull))))
                                                return false;
                                        v.v = cur.v;
                                        v.name = StringView();
                                        return true;
                                }
                                return v.name.isEmpty() ? true : load(v);
                        }

                        bool primary(Value &v) {
                                skipSpace();
                                if (_pos == _s.size()) return failed("syntax error: operand expected");
                                const char c = _s[_pos];
                                if (c == '(') {
                                        ++_pos;
                                        if (!comma(v)) return false;
                                        if (!accept(")")) return failed("missing `)'");
                                        return true;
                                }
                                if (isNameStart(c)) {
                                        const size_t s = _pos;
                                        while (_pos < _s.size() && isNameChar(_s[_pos])) ++_pos;
                                        v.name = _s.mid(s, _pos - s);
                                        v.v = 0;
                                        return true;
                                }
                                if (c >= '0' && c <= '9') return number(v);
                                return failed("syntax error: operand expected");
                        }

                        bool number(Value &v) {
                                const size_t s = _pos;
                                while (_pos < _s.size() && (isNameChar(_s[_pos]) || _s[_pos] == '#' || _s[_pos] == '@'))
                                        ++_pos;
                                StringView   text = _s.mid(s, _pos - s);
                                uint64_t     base = 10;
                                const size_t hash = text.indexOf('#');
                                if (hash != StringView::NPos) {
                                        base = 0;
                                        for (char d : text.left(hash)) {
                                                if (d < '0' || d > '9') return failed("invalid number");
                                                base = base * 10 + static_cast<uint64_t>(d - '0');
                                                if (base > 64) break;
                                        }
                                        if (base < 2 || base > 64) return failed("invalid arithmetic base");
                                        text = text.mid(hash + 1);
                                } else if (text.size() > 1 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
                                        base = 16;
                                        text = text.mid(2);
                                } else if (text.size() > 1 && text[0] == '0') {
                                        base = 8;
                                        text = text.mid(1);
                                }
                                if (text.isEmpty()) return failed("invalid number");
                                uint64_t value = 0;
                                for (char d : text) {
                                        uint64_t digit;
                                        if (d >= '0' && d <= '9') {
                                                digit = static_cast<uint64_t>(d - '0');
                                        } else if (d >= 'a' && d <= 'z') {
                                                digit = static_cast<uint64_t>(d - 'a' + 10);
                                        } else if (d >= 'A' && d <= 'Z') {
                                                digit = static_cast<uint64_t>(d - 'A') + (base <= 36 ? 10u : 36u);
                                        } else if (d == '@') {
                                                digit = 62;
                                        } else if (d == '_') {
                                                digit = 63;
                                        } else {
                                                digit = 99;
                                        }
                                        if (digit >= base) return failed("value too great for base");
                                        value = value * base + digit;
                                }
                                v.v = wrap(value);
                                v.name = StringView();
                                return true;
                        }

                        StringView      _s;
                        size_t          _pos = 0;
                        Variables      &_vars;
                        Arith::Failure &_failure;
                        int             _depth;
                        int             _skip = 0;
                        Error           _err;
        };

} // namespace

Result<int64_t> Arith::evaluate(StringView expr, Variables &vars, Failure &failure) {
        Evaluator e(expr, vars, failure, 0);
        int64_t   out = 0;
        Error     err = e.run(out);
        return Result<int64_t>(out, err);
}

BMDS_NAMESPACE_END
