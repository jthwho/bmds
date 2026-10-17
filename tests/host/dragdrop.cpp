/**
 * @file      dragdrop.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Drag and drop's pure parts (devplan/clipboard.md §5f).
 */

#include <doctest.h>

#include <string>
#include <bmds/desktopentry.h>
#include <bmds/dragdrop.h>

using bmds::DropAction;
using bmds::Span;
using bmds::StringView;
namespace DropActions = bmds::DropActions;
namespace DragImage = bmds::DragImage;

namespace {

        std::string str(StringView v) {
                return std::string(v.data(), v.size());
        }

        std::string quoted(const char *word) {
                bmds::String out(bmds::MemSpace::Internal);
                REQUIRE(bmds::ShellQuote::append(out, StringView(word)).isOk());
                return str(out.view());
        }

} // namespace

TEST_CASE("DropActions: keys first, then the target's default, the source's, the first offered") {
        using DropActions::CanCopy;
        using DropActions::CanLink;
        using DropActions::CanMove;
        const uint8_t all = CanCopy | CanMove | CanLink;
        // Keys: Ctrl copies, Shift moves, both link; not offered is none.
        CHECK(DropActions::choose(all, true, false, DropAction::Move, DropAction::Move) == DropAction::Copy);
        CHECK(DropActions::choose(all, false, true, DropAction::Copy, DropAction::Copy) == DropAction::Move);
        CHECK(DropActions::choose(all, true, true, DropAction::Copy, DropAction::Copy) == DropAction::Link);
        CHECK(DropActions::choose(CanCopy, false, true, DropAction::Copy, DropAction::Copy) == DropAction::None);
        // No keys: the target's default, then the source's preferred, then Copy, Move, Link.
        CHECK(DropActions::choose(all, false, false, DropAction::Move, DropAction::Copy) == DropAction::Move);
        CHECK(DropActions::choose(CanCopy | CanLink, false, false, DropAction::Move, DropAction::Link) ==
              DropAction::Link);
        CHECK(DropActions::choose(CanMove | CanLink, false, false, DropAction::Copy, DropAction::None) ==
              DropAction::Move);
        CHECK(DropActions::choose(CanLink, false, false, DropAction::None, DropAction::None) == DropAction::Link);
        CHECK(DropActions::choose(0, false, false, DropAction::Copy, DropAction::Copy) == DropAction::None);
        // Moved and Ask are never offered bits.
        CHECK(DropActions::bit(DropAction::Moved) == 0);
        CHECK(!DropActions::allows(all, DropAction::Ask));
}

TEST_CASE("ShellQuote: plain words as they are, the rest in single quotes") {
        CHECK(quoted("/sd0/music/a.mp3") == "/sd0/music/a.mp3");
        CHECK(quoted("a-b_c+d:e@f%g,h=i") == "a-b_c+d:e@f%g,h=i");
        CHECK(quoted("/home/My Files") == "'/home/My Files'");
        CHECK(quoted("it's") == "'it'\\''s'");
        CHECK(quoted("$HOME*") == "'$HOME*'");
        CHECK(quoted("") == "''");
        CHECK(quoted("~x") == "'~x'");
}

TEST_CASE("DragImage: a box around the pressed item, the press inside, items held whole") {
        // A 32 x 48 icon cell: the box centred on it.
        const DragImage::Box icon{100, 200, 32, 48};
        const DragImage::Box b = DragImage::around(icon, 110, 210);
        CHECK(b == DragImage::Box{52, 160, 128, 128});
        CHECK(DragImage::holds(b, icon));
        // A wide details row: its start kept with the press, as much as fits.
        const DragImage::Box row{0, 40, 400, 18};
        const DragImage::Box r = DragImage::around(row, 20, 45);
        CHECK(r.x <= 20);
        CHECK(r.x + r.width > 20);
        CHECK(!DragImage::holds(r, row));
        // The press far right on the row: the box moves to hold it.
        const DragImage::Box far = DragImage::around(row, 390, 45);
        CHECK(far.x <= 390);
        CHECK(far.x + far.width > 390);
        // Negative places (a view scrolled) work.
        const DragImage::Box neg = DragImage::around(DragImage::Box{-20, -10, 32, 32}, -10, 0);
        CHECK(neg.x == -68);
        CHECK(DragImage::holds(neg, DragImage::Box{-20, -10, 32, 32}));
        CHECK(!DragImage::holds(neg, DragImage::Box{60, 0, 32, 32}));
        CHECK(!DragImage::holds(neg, DragImage::Box{0, 0, 0, 10}));
}

TEST_CASE("DesktopEntry: Exec with several files") {
        char        buffer[160];
        StringView  args[8];
        const char *two[] = {"/a/x y", "/b/z"};
        const auto  split = [&](const char *exec, size_t n) {
                StringView files[2] = {StringView(two[0]), StringView(two[1])};
                auto [count, err] = bmds::DesktopEntry::splitExec(
                        exec, buffer, sizeof(buffer), Span<StringView>(args, 8), Span<const StringView>(files, n));
                if (err.isError()) return std::string("error");
                std::string all;
                for (size_t i = 0; i < count; ++i) all += (i > 0 ? "|" : "") + str(args[i]);
                return all;
        };
        CHECK(split("view %F", 2) == "view|/a/x y|/b/z");
        CHECK(split("view %U --", 2) == "view|/a/x y|/b/z|--");
        CHECK(split("view %f", 2) == "view|/a/x y");
        CHECK(split("view --in=%F", 2) == "view|--in=/a/x y");
        CHECK(split("view", 2) == "view|/a/x y|/b/z");
        CHECK(split("view %F", 0) == "view");
        CHECK(split("a b c d e f %F", 2) == "a|b|c|d|e|f|/a/x y|/b/z");
        CHECK(split("a b c d e f g %F", 2) == "error"); // nine arguments
        CHECK(bmds::DesktopEntry::takesFileList("view %F"));
        CHECK(bmds::DesktopEntry::takesFileList("view %U"));
        CHECK(bmds::DesktopEntry::takesFileList("view"));
        CHECK(bmds::DesktopEntry::takesFileList("view 100%%"));
        CHECK(!bmds::DesktopEntry::takesFileList("view %f"));
        CHECK(!bmds::DesktopEntry::takesFileList("view %u %i"));
}
