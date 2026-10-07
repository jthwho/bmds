/**
 * @file      mimeapps.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Which applications open a type: mimeapps.list (devplan/filetypes.md T6).
 */

#include <bmds/inireader.h>
#include <bmds/mimeapps.h>

BMDS_NAMESPACE_BEGIN

namespace MimeApps {

        size_t lookup(StringView text, StringView type, Span<StringView> out) {
                const size_t     slash = type.indexOf('/');
                const StringView media = slash != StringView::NPos ? type.left(slash) : StringView();
                size_t           count = 0;
                // The type's own line first, then its media's.
                for (int pass = 0; pass < 2; ++pass) {
                        IniReader reader(text);
                        for (IniReader::Item item = reader.next(); item.kind != IniReader::Kind::End;
                             item = reader.next()) {
                                if (item.kind != IniReader::Kind::Entry ||
                                    !item.name.equalsIgnoreCase("Default Applications"))
                                        continue;
                                const StringView key = item.key;
                                const bool       match = pass == 0 ? key.equalsIgnoreCase(type)
                                                                   : (!media.isEmpty() && key.size() == media.size() + 2 &&
                                                                key.left(media.size()).equalsIgnoreCase(media) &&
                                                                key.mid(media.size()) == StringView("/*"));
                                if (!match) continue;
                                StringView list = item.value;
                                for (StringView id = IniReader::nextListItem(list); !id.isEmpty() && count < out.size();
                                     id = IniReader::nextListItem(list))
                                        out[count++] = id;
                        }
                }
                return count;
        }

} // namespace MimeApps

BMDS_NAMESPACE_END
