/**
 * @file      regex.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/regex.h>
#include <bmds/string.h>

BMDS_NAMESPACE_BEGIN

Regex::~Regex() {
        if (_compiled) regfree(&_re);
        return;
}

Error Regex::compile(StringView pattern, unsigned int flags) {
        if (_compiled) regfree(&_re);
        _compiled = false;
        _error[0] = '\0';
        // regcomp() takes a NUL-terminated pattern; a view may not be one.
        Result<String> text = String::fromView(pattern);
        if (text.isError()) return text.error();
        int cflags = REG_NOSUB;
        if ((flags & Extended) != 0) cflags |= REG_EXTENDED;
        if ((flags & IgnoreCase) != 0) cflags |= REG_ICASE;
        const int r = regcomp(&_re, text.value().data(), cflags);
        if (r != 0) {
                if (r == REG_ESPACE) return Error::NoMem;
                (void)regerror(r, &_re, _error, sizeof(_error));
                return Error::Invalid;
        }
        _compiled = true;
        return Error::Ok;
}

bool Regex::search(const char *text) const {
        return _compiled && regexec(&_re, text, 0, nullptr, 0) == 0;
}

BMDS_NAMESPACE_END
