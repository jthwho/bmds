/**
 * @file      syntaxdump.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/syntax.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // Collects the first error of a chain of appends.
        class Out {
                public:
                        explicit Out(String &s) : _s(s) {}

                        Out &operator<<(StringView text) {
                                if (_err.isOk()) _err = _s.append(text);
                                return *this;
                        }

                        Out &operator<<(char c) {
                                if (_err.isOk()) _err = _s.append(c);
                                return *this;
                        }

                        Out &operator<<(int n) {
                                if (_err.isOk()) _err = _s.appendf("%d", n);
                                return *this;
                        }

                        Error error() const { return _err; }

                private:
                        String &_s;
                        Error   _err;
        };

        const char *opText(ParamOp op, bool colon) {
                switch (op) {
                        case ParamOp::Default: return colon ? ":-" : "-";
                        case ParamOp::Assign: return colon ? ":=" : "=";
                        case ParamOp::Error: return colon ? ":?" : "?";
                        case ParamOp::Alternative: return colon ? ":+" : "+";
                        case ParamOp::RemoveSmallestPrefix: return "#";
                        case ParamOp::RemoveLargestPrefix: return "##";
                        case ParamOp::RemoveSmallestSuffix: return "%";
                        case ParamOp::RemoveLargestSuffix: return "%%";
                        case ParamOp::None:
                        case ParamOp::Length: break;
                }
                return "";
        }

        void word(Out &o, const Word *w);

        void part(Out &o, const WordPart *p) {
                const bool wrap = p->quoted && p->kind != WordPart::Kind::Literal;
                if (wrap) o << '"';
                switch (p->kind) {
                        case WordPart::Kind::Literal:
                                if (p->quoted) {
                                        o << '\'' << p->text << '\'';
                                } else {
                                        o << p->text;
                                }
                                break;
                        case WordPart::Kind::Parameter:
                                o << "${";
                                if (p->op == ParamOp::Length) o << '#';
                                o << p->text << opText(p->op, p->colon);
                                if (p->arg != nullptr) word(o, p->arg);
                                o << '}';
                                break;
                        case WordPart::Kind::CommandSubstitution: o << "$(" << p->text << ')'; break;
                        case WordPart::Kind::Arithmetic: o << "$((" << p->text << "))"; break;
                }
                if (wrap) o << '"';
                return;
        }

        void word(Out &o, const Word *w) {
                if (w->parts == nullptr && w->hasQuotes) o << "''";
                for (const WordPart *p = w->parts; p != nullptr; p = p->next) part(o, p);
                return;
        }

        const char *redirectText(Redirection::Kind k) {
                switch (k) {
                        case Redirection::Kind::Input: return "<";
                        case Redirection::Kind::Output: return ">";
                        case Redirection::Kind::Append: return ">>";
                        case Redirection::Kind::Clobber: return ">|";
                        case Redirection::Kind::InputOutput: return "<>";
                        case Redirection::Kind::DupInput: return "<&";
                        case Redirection::Kind::DupOutput: return ">&";
                        case Redirection::Kind::OutputBoth: return "&>";
                        case Redirection::Kind::AppendBoth: return "&>>";
                        case Redirection::Kind::HereDoc: return "<<";
                        case Redirection::Kind::HereString: return "<<<";
                }
                return "?";
        }

        void redirections(Out &o, const Redirection *r) {
                for (; r != nullptr; r = r->next) {
                        o << " (";
                        if (r->fd >= 0) o << r->fd;
                        o << redirectText(r->kind);
                        if (r->stripTabs) o << '-';
                        o << ' ';
                        word(o, r->target);
                        if (r->body != nullptr) {
                                o << ' ';
                                word(o, r->body);
                        }
                        o << ')';
                }
                return;
        }

        void node(Out &o, const Node *n);

        void children(Out &o, const Node *first) {
                for (const Node *c = first; c != nullptr; c = c->next) {
                        o << ' ';
                        node(o, c);
                }
                return;
        }

        void body(Out &o, const Node *n) {
                o << ' ';
                node(o, n);
                return;
        }

        void node(Out &o, const Node *n) {
                const bool wrap = n->kind != NodeKind::Simple && n->redirects != nullptr;
                if (wrap) o << "(redirect ";
                switch (n->kind) {
                        case NodeKind::Simple: {
                                const auto *s = static_cast<const SimpleNode *>(n);
                                o << "(cmd";
                                for (const Assignment *a = s->assignments; a != nullptr; a = a->next) {
                                        o << ' ' << a->name << '=';
                                        word(o, a->value);
                                }
                                for (const Word *w = s->words; w != nullptr; w = w->next) {
                                        o << ' ';
                                        word(o, w);
                                }
                                redirections(o, n->redirects);
                                o << ')';
                                break;
                        }
                        case NodeKind::Pipeline: {
                                const auto *p = static_cast<const PipelineNode *>(n);
                                if (p->negated) o << "(! ";
                                if (p->commands->next != nullptr) {
                                        o << "(|";
                                        children(o, p->commands);
                                        o << ')';
                                } else {
                                        node(o, p->commands);
                                }
                                if (p->negated) o << ')';
                                break;
                        }
                        case NodeKind::AndOr: {
                                const auto *a = static_cast<const AndOrNode *>(n);
                                o << (a->isAnd ? "(&&" : "(||");
                                body(o, a->left);
                                body(o, a->right);
                                o << ')';
                                break;
                        }
                        case NodeKind::List:
                                o << "(;";
                                children(o, static_cast<const ListNode *>(n)->items);
                                o << ')';
                                break;
                        case NodeKind::If: {
                                const auto *i = static_cast<const IfNode *>(n);
                                o << "(if";
                                body(o, i->condition);
                                body(o, i->thenPart);
                                if (i->elsePart != nullptr) body(o, i->elsePart);
                                o << ')';
                                break;
                        }
                        case NodeKind::While:
                        case NodeKind::Until: {
                                const auto *l = static_cast<const LoopNode *>(n);
                                o << (n->kind == NodeKind::While ? "(while" : "(until");
                                body(o, l->condition);
                                body(o, l->body);
                                o << ')';
                                break;
                        }
                        case NodeKind::For: {
                                const auto *f = static_cast<const ForNode *>(n);
                                o << "(for " << f->variable;
                                if (f->hasIn) {
                                        o << " (";
                                        for (const Word *w = f->words; w != nullptr; w = w->next) {
                                                if (w != f->words) o << ' ';
                                                word(o, w);
                                        }
                                        o << ')';
                                }
                                body(o, f->body);
                                o << ')';
                                break;
                        }
                        case NodeKind::ArithFor: {
                                const auto *f = static_cast<const ArithForNode *>(n);
                                o << "(for ((" << f->init << "; " << f->condition << "; " << f->step << "))";
                                body(o, f->body);
                                o << ')';
                                break;
                        }
                        case NodeKind::Case: {
                                const auto *c = static_cast<const CaseNode *>(n);
                                o << "(case ";
                                word(o, c->subject);
                                for (const CaseItem *item = c->items; item != nullptr; item = item->next) {
                                        o << " (";
                                        for (const Word *w = item->patterns; w != nullptr; w = w->next) {
                                                if (w != item->patterns) o << '|';
                                                word(o, w);
                                        }
                                        if (item->body != nullptr) body(o, item->body);
                                        o << ')';
                                }
                                o << ')';
                                break;
                        }
                        case NodeKind::Group:
                        case NodeKind::Subshell:
                                o << (n->kind == NodeKind::Group ? "(group" : "(subshell");
                                body(o, static_cast<const BodyNode *>(n)->body);
                                o << ')';
                                break;
                        case NodeKind::Function: {
                                const auto *f = static_cast<const FunctionNode *>(n);
                                o << "(function " << f->name;
                                body(o, f->body);
                                o << ')';
                                break;
                        }
                        case NodeKind::Arithmetic:
                                o << "(arith " << static_cast<const ArithNode *>(n)->expression << ')';
                                break;
                        case NodeKind::Background:
                                o << "(&";
                                body(o, static_cast<const BackgroundNode *>(n)->body);
                                o << ')';
                                break;
                }
                if (wrap) {
                        redirections(o, n->redirects);
                        o << ')';
                }
                return;
        }

} // namespace

Error dumpSyntax(const Node *n, String &out) {
        Out o(out);
        node(o, n);
        return o.error();
}

Error dumpWord(const Word *w, String &out) {
        Out o(out);
        word(o, w);
        return o.error();
}

BMDS_NAMESPACE_END
