/**
 * @file      gui.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The toolkit's pure parts (devplan/gui.md): configuration files, launcher
 * entries, menu geometry, line layout, scroll bars and window furniture.
 */

#include <doctest.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <bmds/bevel.h>
#include <bmds/desktopentry.h>
#include <bmds/filenamelist.h>
#include <bmds/font.h>
#include <bmds/furniturelayout.h>
#include <bmds/icongrid.h>
#include <bmds/itemlayout.h>
#include <bmds/itemmodel.h>
#include <bmds/itemselection.h>
#include <bmds/inflate.h>
#include <bmds/inireader.h>
#include <bmds/lineeditmodel.h>
#include <bmds/linelayout.h>
#include <bmds/menulayout.h>
#include <bmds/mnemonic.h>
#include <bmds/pngdecoder.h>
#include <bmds/program.h>
#include <bmds/programimage.h>
#include <bmds/scrollmodel.h>
#include <bmds/slidermodel.h>
#include <bmds/switcherlayout.h>
#include <bmds/tabrow.h>
#include <bmds/treerows.h>
#include <bmds/windowstack.h>

using bmds::DesktopEntry;
using bmds::Error;
using bmds::IniReader;
using bmds::LineLayout;
using bmds::MenuLayout;
using bmds::Rect;
using bmds::Span;
using bmds::StringView;

namespace {

        std::string str(StringView v) {
                return std::string(v.data(), v.size());
        }

        std::vector<std::string> split(StringView exec) {
                char       buffer[256];
                StringView args[DesktopEntry::MaxArgs];
                auto [n, err] = DesktopEntry::splitExec(exec, buffer, sizeof(buffer), Span<StringView>(args));
                std::vector<std::string> out;
                if (err.isError()) {
                        out.push_back("error");
                        return out;
                }
                for (size_t i = 0; i < n; ++i) out.push_back(str(args[i]));
                return out;
        }

        struct Warnings {
                        std::vector<uint32_t> lines;
                        static void           warn(void *arg, uint32_t line, const char *) {
                                static_cast<Warnings *>(arg)->lines.push_back(line);
                                return;
                        }
        };

} // namespace

TEST_CASE("IniReader: groups, entries, comments, invalid lines") {
        IniReader r("# a comment\r\n"
                    "\n"
                    "  [Panel]  \r\n"
                    "Edge = top\n"
                    "; another\n"
                    "Items=launcher;windows;clock\n"
                    "nonsense\n"
                    "[]\n"
                    "=x\n"
                    "[Clock UTC]\n"
                    "Format=%H:%M:%S   ");
        auto      i = r.next();
        CHECK(i.kind == IniReader::Kind::Group);
        CHECK(str(i.name) == "Panel");
        CHECK(i.line == 3);
        i = r.next();
        CHECK(i.kind == IniReader::Kind::Entry);
        CHECK(str(i.name) == "Panel");
        CHECK(str(i.key) == "Edge");
        CHECK(str(i.value) == "top");
        i = r.next();
        CHECK(str(i.key) == "Items");
        CHECK(i.line == 6);
        for (uint32_t line : {7u, 8u, 9u}) {
                i = r.next();
                CHECK(i.kind == IniReader::Kind::Invalid);
                CHECK(i.line == line);
        }
        i = r.next();
        CHECK(i.kind == IniReader::Kind::Group);
        CHECK(str(i.name) == "Clock UTC");
        i = r.next();
        CHECK(str(i.value) == "%H:%M:%S");
        CHECK(str(r.group()) == "Clock UTC");
        CHECK(r.next().kind == IniReader::Kind::End);
        CHECK(r.next().kind == IniReader::Kind::End);
}

TEST_CASE("IniReader: lists, escapes, booleans") {
        StringView               rest = " launcher ;; windows;a\\;b;clock; ";
        std::vector<std::string> items;
        for (StringView v = IniReader::nextListItem(rest); !v.isEmpty(); v = IniReader::nextListItem(rest))
                items.push_back(str(v));
        CHECK(items == std::vector<std::string>{"launcher", "windows", "a\\;b", "clock"});

        char out[16];
        CHECK(IniReader::unescape("a\\sb\\\\c\\;d\\te", out, sizeof(out)).isOk());
        CHECK(std::string(out) == "a b\\c;d\te");
        CHECK(IniReader::unescape("0123456789abcdef", out, sizeof(out)) == Error::BufferTooSmall);
        CHECK(IniReader::unescape("0123456789abcde", out, sizeof(out)).isOk());

        bool b = false;
        CHECK(IniReader::toBool("TRUE", b).isOk());
        CHECK(b);
        CHECK(IniReader::toBool("no", b).isOk());
        CHECK(!b);
        CHECK(IniReader::toBool("maybe", b) == Error::Invalid);
}

TEST_CASE("DesktopEntry: the keys used, others ignored") {
        Warnings w;
        auto [e, err] = DesktopEntry::parse("[Desktop Entry]\n"
                                            "Type=Application\n"
                                            "Name=Text\\sEditor\n"
                                            "Name[de]=Texteditor\n"
                                            "Comment=Edits text\n"
                                            "Exec=vi %F\n"
                                            "Icon=accessories-text-editor\n"
                                            "Terminal=true\n"
                                            "Categories=Utility;TextEditor;\n"
                                            "X-Other=1\n"
                                            "NoDisplay=perhaps\n"
                                            "junk\n"
                                            "[Desktop Action New]\n"
                                            "Name=Not this\n"
                                            "Exec=nope\n",
                                            Warnings::warn, &w);
        REQUIRE(err.isOk());
        CHECK(std::string(e.name) == "Text Editor");
        CHECK(std::string(e.comment) == "Edits text");
        CHECK(std::string(e.exec) == "vi %F");
        CHECK(std::string(e.icon) == "accessories-text-editor");
        CHECK(e.terminal);
        CHECK(!e.noDisplay);
        CHECK(!e.hidden);
        CHECK(DesktopEntry::hasCategory(e.categories, "TextEditor"));
        CHECK(!DesktopEntry::hasCategory(e.categories, "Text"));
        CHECK(w.lines == std::vector<uint32_t>{11, 12});
}

TEST_CASE("DesktopEntry: what isn't an entry") {
        CHECK(DesktopEntry::parse("Name=x\nExec=y\n").error() == Error::Invalid);
        CHECK(DesktopEntry::parse("[Desktop Entry]\nExec=y\n").error() == Error::Invalid);
        CHECK(DesktopEntry::parse("[Desktop Entry]\nName=x\n").error() == Error::Invalid);
        CHECK(DesktopEntry::parse("[Desktop Entry]\nName=\nExec=y\n").error() == Error::Invalid);
        CHECK(DesktopEntry::parse("[Desktop Entry]\nType=Link\nName=x\nExec=y\n").error() == Error::NotSupported);
        std::string longName = "[Desktop Entry]\nExec=y\nName=" + std::string(64, 'n') + "\n";
        CHECK(DesktopEntry::parse(StringView(longName.data(), longName.size())).error() == Error::BufferTooSmall);
        // Hidden needs nothing else: it hides another file of its name.
        auto [hidden, err] = DesktopEntry::parse("[Desktop Entry]\nHidden=true\n");
        CHECK(err.isOk());
        CHECK(hidden.hidden);
}

TEST_CASE("DesktopEntry: Exec split by the specification's quoting") {
        using V = std::vector<std::string>;
        CHECK(split("terminal") == V{"terminal"});
        CHECK(split("  ls   -l\t/boot  ") == V{"ls", "-l", "/boot"});
        CHECK(split("sh -c \"echo hi; sleep 1\"") == V{"sh", "-c", "echo hi; sleep 1"});
        CHECK(split("echo \"a \\\"b\\\" \\$c \\\\ \\`d\\`\"") == V{"echo", "a \"b\" $c \\ `d`"});
        CHECK(split("a\"b c\"d") == V{"ab cd"});
        CHECK(split("vi %F") == V{"vi"});
        CHECK(split("vi %f --x=%u 100%%") == V{"vi", "--x=", "100%"});
        CHECK(split("echo \"\"") == V{"echo", ""});
        CHECK(split("echo \"%f\"") == V{"echo", "%f"});
        CHECK(split("") == V{"error"});
        CHECK(split("%f %U") == V{"error"});
        CHECK(split("echo \"open") == V{"error"});

        char       small[7];
        StringView args[2];
        CHECK(DesktopEntry::splitExec("abc de", small, sizeof(small), Span<StringView>(args)).value() == 2);
        CHECK(DesktopEntry::splitExec("abc def", small, sizeof(small), Span<StringView>(args)).error() ==
              Error::BufferTooSmall);
        char big[64];
        CHECK(DesktopEntry::splitExec("a b c", big, sizeof(big), Span<StringView>(args)).error() ==
              Error::BufferTooSmall);
}

TEST_CASE("MenuLayout: rows, widths, hits and keys") {
        const MenuLayout::Item items[] = {
                {"Restore", "", MenuLayout::Disabled}, {"Minimize", "", 0},
                {"", "", MenuLayout::Separator},       {"Close", "Alt+F4", 0},
                {"More", "", MenuLayout::Submenu},
        };
        MenuLayout m;
        REQUIRE(m.layout(Span<const MenuLayout::Item>(items), bmds::Fonts::unscii16).isOk());
        CHECK(m.count() == 5);
        CHECK(m.width() % 4 == 0);
        // The text column, the longest text, the shortcut and the arrow fit.
        CHECK(m.width() >= MenuLayout::TextX + 8 * 8 + MenuLayout::ShortcutGap + 6 * 8 + MenuLayout::ArrowWidth);
        CHECK(m.height() == 2 * MenuLayout::InsetY + 4 * MenuLayout::ItemHeight + MenuLayout::SeparatorHeight);
        // Rows touch, top to bottom.
        uint32_t y = MenuLayout::InsetY;
        for (size_t i = 0; i < m.count(); ++i) {
                const Rect r = m.itemRect(i);
                CHECK(r.y == y);
                CHECK(r.x == MenuLayout::Edge);
                CHECK(r.right() == m.width() - MenuLayout::Edge);
                y = r.bottom();
                CHECK(m.itemAt(10, r.y) == int(i));
                CHECK(m.itemAt(10, int(r.bottom()) - 1) == int(i));
        }
        CHECK(m.itemAt(10, 0) == -1);
        CHECK(m.itemAt(0, 10) == -1);
        CHECK(m.itemAt(m.width() - 1, 10) == -1);
        CHECK(m.itemAt(10, m.height() - 1) == -1);
        CHECK(m.shortcutX(3) % 4 == 0);
        CHECK(m.shortcutX(3) + 6 * 8 <= m.arrowX());
        // The keys skip the disabled item and the separator, around the ends.
        CHECK(m.next(-1, 1) == 1);
        CHECK(m.next(1, 1) == 3);
        CHECK(m.next(4, 1) == 1);
        CHECK(m.next(1, -1) == 4);
        CHECK(m.next(-1, -1) == 4);
        CHECK(!m.isSelectable(0));
        CHECK(!m.isSelectable(2));

        const MenuLayout::Item none[] = {{"x", "", MenuLayout::Disabled}};
        REQUIRE(m.layout(Span<const MenuLayout::Item>(none), bmds::Fonts::unscii16).isOk());
        CHECK(m.width() == MenuLayout::MinWidth);
        CHECK(m.next(-1, 1) == -1);
        MenuLayout::Item many[MenuLayout::MaxItems + 1];
        CHECK(m.layout(Span<const MenuLayout::Item>(many), bmds::Fonts::unscii16) == Error::Full);
}

TEST_CASE("MenuLayout: placing a menu on the screen") {
        const Rect screen{0, 24, 1280, 696}; // below a panel
        // A context menu at a point: below and right of it.
        CHECK(MenuLayout::place(120, 100, Rect{400, 300, 0, 0}, screen, false) == Rect{400, 300, 120, 100});
        // Off the right edge: moved left, on a 4-pixel column.
        CHECK(MenuLayout::place(120, 100, Rect{1250, 300, 0, 0}, screen, false) == Rect{1160, 300, 120, 100});
        CHECK(MenuLayout::place(120, 100, Rect{1203, 300, 0, 0}, screen, false).x == 1160);
        // Off the bottom: above the point, or the button.
        CHECK(MenuLayout::place(120, 100, Rect{400, 680, 0, 0}, screen, false) == Rect{400, 580, 120, 100});
        CHECK(MenuLayout::place(120, 100, Rect{400, 660, 60, 24}, screen, false) == Rect{400, 560, 120, 100});
        // A button on the panel: below it, inside the area.
        CHECK(MenuLayout::place(120, 100, Rect{0, 0, 60, 24}, screen, false) == Rect{0, 24, 120, 100});
        // Taller than the area: cut to it.
        CHECK(MenuLayout::place(120, 900, Rect{400, 300, 0, 0}, screen, false) == Rect{400, 24, 120, 696});
        // A submenu: right of its item, first rows level; else left of the menu.
        const Rect item{500, 200, 116, 20};
        CHECK(MenuLayout::place(120, 100, item, screen, true) ==
              Rect{616, uint16_t(200 - MenuLayout::InsetY), 120, 100});
        const Rect right{1150, 200, 116, 20};
        CHECK(MenuLayout::place(120, 100, right, screen, true).x == 1028);
}

TEST_CASE("IconGrid: wanted cells kept, the rest in free cells down then across") {
        using bmds::IconGrid;
        using Cell = IconGrid::Cell;
        Cell out[6];
        // None wanted: down the first column, then the next.
        const Cell none[5] = {};
        REQUIRE(IconGrid::place(bmds::Span<const Cell>(none, 5), 3, 2, bmds::Span<Cell>(out, 6)).isOk());
        CHECK(out[0] == Cell{0, 0});
        CHECK(out[1] == Cell{0, 1});
        CHECK(out[2] == Cell{1, 0});
        CHECK(out[4] == Cell{2, 0});
        // Wanted cells kept; a taken one, or one off the grid, placed as the rest.
        const Cell some[5] = {Cell{}, Cell{0, 0}, Cell{2, 1}, Cell{0, 0}, Cell{5, 0}};
        REQUIRE(IconGrid::place(bmds::Span<const Cell>(some, 5), 3, 2, bmds::Span<Cell>(out, 6)).isOk());
        CHECK(out[1] == Cell{0, 0});
        CHECK(out[2] == Cell{2, 1});
        CHECK(out[0] == Cell{0, 1}); // the first free one
        CHECK(out[3] == Cell{1, 0});
        CHECK(out[4] == Cell{1, 1});
        // More items than cells: further columns, off the grid.
        REQUIRE(IconGrid::place(bmds::Span<const Cell>(none, 5), 1, 2, bmds::Span<Cell>(out, 6)).isOk());
        CHECK(out[4] == Cell{2, 0});
        // The cell under a point, the nearest outside the grid.
        CHECK(IconGrid::cellAt(0, 0, 80, 76, 4, 3) == Cell{0, 0});
        CHECK(IconGrid::cellAt(159, 76, 80, 76, 4, 3) == Cell{1, 1});
        CHECK(IconGrid::cellAt(-5, 9999, 80, 76, 4, 3) == Cell{0, 2});
        CHECK(IconGrid::cellAt(9999, -1, 80, 76, 4, 3) == Cell{3, 0});
        CHECK(!Cell{}.isValid());
}

TEST_CASE("SwitcherLayout: a grid of icons over a title, on 4-pixel columns") {
        using bmds::SwitcherLayout;
        SwitcherLayout l;
        // One window: the box at its least width, one cell centred.
        l.layout(1, 1280);
        CHECK(l.width() == SwitcherLayout::MinWidth);
        CHECK(l.columns() == 1);
        CHECK(l.height() == SwitcherLayout::Margin + SwitcherLayout::Cell + SwitcherLayout::Gap +
                                    SwitcherLayout::TitleHeight + SwitcherLayout::Margin);
        CHECK(l.cellRect(0).x % 4 == 0);
        CHECK(l.cellRect(0).x + l.cellRect(0).width / 2 >= 120); // about the middle
        CHECK(l.iconRect(0).width == 32);
        CHECK(l.iconRect(0).x % 4 == 0);
        CHECK(l.cellRect(1).isEmpty());
        // Ten: eight a row, then two; the title under the second row.
        l.layout(10, 1280);
        CHECK(l.columns() == 8);
        CHECK(l.width() == 8 * SwitcherLayout::Cell + 2 * SwitcherLayout::Margin);
        CHECK(l.cellRect(8).y == l.cellRect(0).y + SwitcherLayout::Cell);
        CHECK(l.cellRect(9).x == l.cellRect(1).x);
        CHECK(l.titleRect().y == l.cellRect(8).bottom() + SwitcherLayout::Gap);
        CHECK(l.titleRect().right() + SwitcherLayout::Margin == l.width());
        bool aligned = true;
        for (size_t i = 0; i < 10; ++i) aligned = aligned && l.iconRect(i).x % 4 == 0;
        CHECK(aligned);
        // The cell under a point.
        const bmds::Rect c = l.cellRect(9);
        CHECK(l.cellAt(c.x, c.y) == 9);
        CHECK(l.cellAt(c.right() - 1, c.bottom() - 1) == 9);
        CHECK(l.cellAt(c.right(), c.y) == -1); // no tenth-plus-one cell
        CHECK(l.cellAt(0, 0) == -1);           // the margin
        // A narrow screen: fewer columns, never wider than it.
        l.layout(10, 200);
        CHECK(l.width() <= 200);
        CHECK(l.columns() == (200 - 2 * SwitcherLayout::Margin) / SwitcherLayout::Cell);
}

TEST_CASE("LineLayout: hints, stretch, most, and too little room") {
        uint16_t pos[4], size[4];
        // | launcher | windows (stretch) | clock |
        const LineLayout::Item panel[] = {{60, UINT16_MAX, 0}, {0, UINT16_MAX, 1}, {130, UINT16_MAX, 0}};
        LineLayout::arrange(Span<const LineLayout::Item>(panel), 1280, 4, 4, Span<uint16_t>(pos, 3),
                            Span<uint16_t>(size, 3));
        CHECK(pos[0] == 0);
        CHECK(size[0] == 60);
        CHECK(pos[1] == 64);
        CHECK(size[1] == 1280 - 60 - 132 - 8);
        CHECK(pos[2] == 64 + size[1] + 4);
        CHECK(size[2] == 132); // hints rounded up to the unit
        CHECK(pos[2] + size[2] == 1280);

        // Window buttons: equal shares, none past its most.
        const LineLayout::Item buttons[] = {{40, 200, 1}, {40, 200, 1}, {40, 200, 1}};
        LineLayout::arrange(Span<const LineLayout::Item>(buttons), 1000, 4, 4, Span<uint16_t>(pos, 3),
                            Span<uint16_t>(size, 3));
        for (int i = 0; i < 3; ++i) CHECK(size[i] == 200);
        LineLayout::arrange(Span<const LineLayout::Item>(buttons), 400, 4, 4, Span<uint16_t>(pos, 3),
                            Span<uint16_t>(size, 3));
        CHECK(size[0] + size[1] + size[2] == 392);
        for (int i = 0; i < 3; ++i) {
                CHECK(pos[i] % 4 == 0);
                CHECK(size[i] % 4 == 0);
                CHECK(size[i] >= 128);
        }
        // Stretch factors share in proportion.
        const LineLayout::Item weighted[] = {{0, UINT16_MAX, 1}, {0, UINT16_MAX, 3}};
        LineLayout::arrange(Span<const LineLayout::Item>(weighted), 400, 0, 4, Span<uint16_t>(pos, 2),
                            Span<uint16_t>(size, 2));
        CHECK(size[0] == 100);
        CHECK(size[1] == 300);
        // Too little room: the stretching items give first, then the last ones.
        LineLayout::arrange(Span<const LineLayout::Item>(panel), 180, 4, 4, Span<uint16_t>(pos, 3),
                            Span<uint16_t>(size, 3));
        CHECK(size[1] == 0);
        CHECK(pos[2] + size[2] <= 180);
        CHECK(size[0] == 60);
        // Every item inside the line, whatever the length.
        for (uint16_t length = 0; length < 300; ++length) {
                LineLayout::arrange(Span<const LineLayout::Item>(panel), length, 4, 4, Span<uint16_t>(pos, 3),
                                    Span<uint16_t>(size, 3));
                for (int i = 0; i < 3; ++i) CHECK(pos[i] + size[i] <= length);
        }
}

TEST_CASE("Bevel: the edges cover the rings exactly, the shaded side taking its corners") {
        using Style = bmds::Bevel::Style;
        using Role = bmds::WindowPalette::Role;
        const Style styles[] = {Style::Raised, Style::Sunken, Style::Window, Style::Field, Style::Status, Style::Thin};
        for (const Style style : styles) {
                const uint16_t depth = bmds::Bevel::width(style);
                CHECK((depth == 1 || depth == 2));
                for (uint16_t w = 0; w <= 9; ++w) {
                        for (uint16_t h = 0; h <= 9; ++h) {
                                const Rect        rect{10, 20, w, h};
                                bmds::Bevel::Edge edges[bmds::Bevel::MaxEdges];
                                const size_t      n = bmds::Bevel::edges(rect, style, edges);
                                std::vector<int>  count(size_t(w) * h, 0);
                                std::vector<Role> role(size_t(w) * h, Role::Face);
                                for (size_t i = 0; i < n; ++i) {
                                        const Rect &e = edges[i].rect;
                                        REQUIRE(!e.isEmpty());
                                        for (uint32_t y = e.y; y < e.bottom(); ++y)
                                                for (uint32_t x = e.x; x < e.right(); ++x) {
                                                        REQUIRE((x >= rect.x && x < rect.right() && y >= rect.y &&
                                                                 y < rect.bottom()));
                                                        ++count[(y - rect.y) * w + (x - rect.x)];
                                                        role[(y - rect.y) * w + (x - rect.x)] = edges[i].role;
                                                }
                                }
                                // Each pixel within `depth` of an edge once; the rest (the face) never.
                                for (int y = 0; y < h; ++y)
                                        for (int x = 0; x < w; ++x) {
                                                const int d = std::min(std::min(x, y), std::min(w - 1 - x, h - 1 - y));
                                                CHECK(count[size_t(y) * w + x] == (d < depth ? 1 : 0));
                                        }
                                const Rect in = bmds::Bevel::inside(rect, style);
                                if (w > 2 * depth && h > 2 * depth) {
                                        CHECK(in == Rect{uint16_t(10 + depth), uint16_t(20 + depth),
                                                         uint16_t(w - 2 * depth), uint16_t(h - 2 * depth)});
                                } else {
                                        CHECK(in.isEmpty());
                                }
                                if (w >= 3 && h >= 3) {
                                        // The outer ring: lit top-left, shaded bottom-right and both far corners.
                                        const Role lit = role[0];
                                        const Role shaded = role[size_t(h - 1) * w + (w - 1)];
                                        CHECK(lit != shaded);
                                        CHECK(role[w - 1] == shaded);             // top right
                                        CHECK(role[size_t(h - 1) * w] == shaded); // bottom left
                                        CHECK(role[1] == lit);                    // the top row
                                        CHECK(role[size_t(w)] == lit);            // the left column
                                }
                        }
                }
        }
        // A raised button's light comes from the top left; sunk, it is the other way round.
        bmds::Bevel::Edge up[bmds::Bevel::MaxEdges], down[bmds::Bevel::MaxEdges];
        bmds::Bevel::edges(Rect{0, 0, 16, 16}, Style::Raised, up);
        bmds::Bevel::edges(Rect{0, 0, 16, 16}, Style::Sunken, down);
        CHECK(up[2].role == Role::FaceHighlight);
        CHECK(up[0].role == Role::FaceDark);
        CHECK(down[2].role == Role::FaceDark);
        CHECK(down[0].role == Role::FaceHighlight);
}

TEST_CASE("cascadeRect: down a diagonal, the first free place, again at the top further right") {
        using bmds::cascadeRect;
        using bmds::CascadeStep;
        const Rect        area{0, 24, 1280, 696}; // under a panel
        std::vector<Rect> taken;
        auto              place = [&](uint16_t w, uint16_t h) {
                return cascadeRect(w, h, area, bmds::Span<const Rect>(taken.data(), taken.size()));
        };
        CHECK(place(808, 504) == Rect{16, 40, 808, 504});
        taken.push_back(place(808, 504));
        CHECK(place(808, 504) == Rect{40, 64, 808, 504});
        // A closed window's place is the next one's again.
        taken.push_back(Rect{64, 88, 808, 504});
        CHECK(place(808, 504) == Rect{40, 64, 808, 504});
        // Down the diagonal until it would leave the area, then from the top two steps right.
        taken.clear();
        const uint32_t down = (720 - 40 - 504) / CascadeStep + 1;
        for (uint32_t i = 0; i < down; ++i) {
                const Rect r = place(808, 504);
                CHECK(r == Rect{uint16_t(16 + i * CascadeStep), uint16_t(40 + i * CascadeStep), 808, 504});
                CHECK(r.bottom() <= area.bottom());
                taken.push_back(r);
        }
        CHECK(place(808, 504) == Rect{uint16_t(16 + 2 * CascadeStep), 40, 808, 504});
        // Too big for the steps: as high and as far left as it goes.
        CHECK(place(1280, 690) == Rect{0, 24, 1280, 690});
        // Every place taken: the first.
        taken.clear();
        for (uint32_t k = 0; k < 400; ++k) {
                const Rect r = place(400, 300);
                CHECK(r.x % 4 == 0);
                CHECK(r.right() <= area.right());
                CHECK(r.bottom() <= area.bottom());
                CHECK(r.y >= area.y);
                if (r == Rect{16, 40, 400, 300} && !taken.empty()) break;
                taken.push_back(r);
        }
        CHECK(taken.size() > 30);
        CHECK(place(400, 300) == Rect{16, 40, 400, 300});
}

namespace {

        std::vector<uint8_t> readFile(const std::string &path) {
                std::vector<uint8_t> out;
                FILE                *f = std::fopen(path.c_str(), "rb");
                if (f == nullptr) return out;
                uint8_t buf[4096];
                for (size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) out.insert(out.end(), buf, buf + n);
                std::fclose(f);
                return out;
        }

} // namespace

TEST_CASE("Png: every supported kind decodes to its pixels; the rest is refused") {
        const std::string dir = BMDS_PNG_DIR;
        const char       *good[] = {"rgba-stored", "rgba-fixed", "rgba-dynamic",      "rgb-key",
                                    "grey",        "grey-alpha", "palette-1",         "palette-2",
                                    "palette-4",   "palette-8",  "icon-folder-16x16", "icon-command_line-32x32"};
        for (const char *name : good) {
                INFO(std::string(name));
                const std::vector<uint8_t> file = readFile(dir + "/" + name + ".png");
                const std::vector<uint8_t> want = readFile(dir + "/" + name + ".argb");
                REQUIRE(!file.empty());
                // An LFS pointer instead of the image: 'git lfs pull'.
                REQUIRE(file[1] == 'P');
                REQUIRE(want.size() % 4 == 0);
                auto [size, serr] = bmds::Png::size(bmds::ByteSpan(file.data(), file.size()));
                REQUIRE(serr.isOk());
                CHECK(size_t(size.width) * size.height == want.size() / 4);
                std::vector<uint32_t> pixels(want.size() / 4);
                auto [got, err] =
                        bmds::Png::decode(bmds::ByteSpan(file.data(), file.size()),
                                          bmds::Span<uint32_t>(pixels.data(), pixels.size()), bmds::MemSpace::Psram);
                INFO(std::string(err.desc()));
                REQUIRE(err.isOk());
                CHECK(got.width == size.width);
                CHECK(std::memcmp(pixels.data(), want.data(), want.size()) == 0);
                // Too little room.
                std::vector<uint32_t> small(pixels.size() - 1);
                CHECK(bmds::Png::decode(bmds::ByteSpan(file.data(), file.size()),
                                        bmds::Span<uint32_t>(small.data(), small.size()), bmds::MemSpace::Psram)
                              .error() == Error::BufferTooSmall);
        }
        const std::pair<const char *, Error> bad[] = {{"bad-crc", Error::CorruptData},
                                                      {"bad-truncated", Error::CorruptData},
                                                      {"bad-interlaced", Error::NotSupported},
                                                      {"bad-16bit", Error::NotSupported}};
        for (const auto &[name, why] : bad) {
                INFO(std::string(name));
                const std::vector<uint8_t> file = readFile(dir + "/" + name + ".png");
                REQUIRE(!file.empty());
                std::vector<uint32_t> pixels(4096);
                CHECK(bmds::Png::decode(bmds::ByteSpan(file.data(), file.size()),
                                        bmds::Span<uint32_t>(pixels.data(), pixels.size()), bmds::MemSpace::Psram)
                              .error() == why);
        }
        const uint8_t text[] = "not a PNG at all, but long enough to have a header's room in it";
        CHECK(bmds::Png::size(bmds::ByteSpan(text, sizeof(text))).error() == Error::CorruptData);
        CHECK(bmds::Png::crc32(bmds::ByteSpan(reinterpret_cast<const uint8_t *>("123456789"), 9)) == 0xcbf43926u);
}

TEST_CASE("Inflate: zlib's checks, and what doesn't fit") {
        // zlib.compress(b"hello hello hello hello") (a back reference over itself).
        const uint8_t hello[] = {0x78, 0x9c, 0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x57,
                                 0xc8, 0x40, 0x27, 0x01, 0x68, 0x03, 0x08, 0xb1};
        uint8_t       out[64];
        auto [n, err] = bmds::Inflate::zlib(bmds::ByteSpan(hello, sizeof(hello)), bmds::WritableByteSpan(out, 64));
        REQUIRE(err.isOk());
        CHECK(std::string(reinterpret_cast<char *>(out), n) == "hello hello hello hello");
        CHECK(bmds::Inflate::zlib(bmds::ByteSpan(hello, sizeof(hello)), bmds::WritableByteSpan(out, 10)).error() ==
              Error::BufferTooSmall);
        uint8_t broken[sizeof(hello)];
        std::memcpy(broken, hello, sizeof(hello));
        broken[sizeof(hello) - 1] ^= 1; // the Adler-32
        CHECK(bmds::Inflate::zlib(bmds::ByteSpan(broken, sizeof(broken)), bmds::WritableByteSpan(out, 64)).error() ==
              Error::CorruptData);
        std::memcpy(broken, hello, sizeof(hello));
        broken[1] ^= 1; // the header's check
        CHECK(bmds::Inflate::zlib(bmds::ByteSpan(broken, sizeof(broken)), bmds::WritableByteSpan(out, 64)).error() ==
              Error::CorruptData);
        CHECK(bmds::Inflate::zlib(bmds::ByteSpan(hello, 8), bmds::WritableByteSpan(out, 64)).error() ==
              Error::CorruptData);
        const uint8_t reserved[] = {0x07}; // a last block of the reserved type
        CHECK(bmds::Inflate::raw(bmds::ByteSpan(reserved, 1), bmds::WritableByteSpan(out, 64)).error() ==
              Error::CorruptData);
        CHECK(bmds::Inflate::adler32(bmds::ByteSpan(reinterpret_cast<const uint8_t *>("Wikipedia"), 9)) == 0x11e60398u);
}

#ifdef BMDS_SDK_EXAMPLES
TEST_CASE("Program icons: an SDK program's notes, nearest the size asked for") {
        const std::vector<uint8_t> elf = readFile(std::string(BMDS_SDK_EXAMPLES) + "/hello.elf");
        REQUIRE(!elf.empty());
        bmds::ProgramImage::MemorySource source(bmds::ByteSpan(elf.data(), elf.size()));
        for (const auto &[want, got] : {std::pair<uint16_t, uint16_t>{16, 16}, {32, 32}, {24, 32}, {48, 32}, {8, 16}}) {
                bmds::ProgramImage::IconRef ref;
                REQUIRE(bmds::ProgramImage::findIcon(source, uint32_t(elf.size()), want, ref).isOk());
                CHECK(ref.width == got);
                CHECK(ref.height == got);
                CHECK(ref.format == BMDS_ICON_FORMAT_PNG);
                REQUIRE(ref.offset + ref.length <= elf.size());
                // It is the PNG the program was built with, and it decodes.
                const std::vector<uint8_t> png =
                        readFile(std::string(BMDS_PNG_DIR) + "/../../../sdk/examples/hello/hello-" +
                                 std::to_string(got) + ".png");
                REQUIRE(png.size() == ref.length);
                CHECK(std::memcmp(png.data(), elf.data() + ref.offset, ref.length) == 0);
                std::vector<uint32_t> pixels(size_t(got) * got);
                CHECK(bmds::Png::decode(bmds::ByteSpan(elf.data() + ref.offset, ref.length),
                                        bmds::Span<uint32_t>(pixels.data(), pixels.size()), bmds::MemSpace::Psram)
                              .isOk());
        }
        // A program without icons has none; neither has a file that isn't ELF.
        const std::vector<uint8_t>       cat = readFile(std::string(BMDS_SDK_EXAMPLES) + "/cat.elf");
        bmds::ProgramImage::MemorySource catSource(bmds::ByteSpan(cat.data(), cat.size()));
        bmds::ProgramImage::IconRef      ref;
        CHECK(bmds::ProgramImage::findIcon(catSource, uint32_t(cat.size()), 16, ref) == Error::NotFound);
        const uint8_t                    text[64] = "not a program";
        bmds::ProgramImage::MemorySource textSource(bmds::ByteSpan(text, sizeof(text)));
        CHECK(bmds::ProgramImage::findIcon(textSource, sizeof(text), 16, ref) == Error::NotFound);
}
#endif

namespace {

        std::string str(const bmds::LineEditModel &m) {
                // The text with the cursor as | and the anchor as ^ (when apart).
                std::string       out;
                const std::string t(m.text().data(), m.text().size());
                for (size_t i = 0; i <= t.size(); ++i) {
                        if (i == m.anchor() && m.hasSelection()) out += '^';
                        if (i == m.cursor()) out += '|';
                        if (i < t.size()) out += t[i];
                }
                return out;
        }

        std::vector<std::string> wrapped(const char *text, uint16_t columns, size_t max = 16) {
                StringView               lines[16];
                const size_t             n = bmds::TextWrap::wrap(text, columns, Span<StringView>(lines, max));
                std::vector<std::string> out;
                for (size_t i = 0; i < n; ++i) out.push_back(str(lines[i]));
                return out;
        }

} // namespace

TEST_CASE("LineEditModel: editing whole characters, a selection, the limit") {
        bmds::LineEditModel m;
        m.setText("caf\xc3\xa9");
        CHECK(str(m) == "caf\xc3\xa9|");
        m.left(false);
        CHECK(m.cursor() == 3); // before the é, not inside it
        CHECK(m.insert("\xe2\x82\xac"));
        CHECK(str(m) == "caf\xe2\x82\xac|\xc3\xa9");
        CHECK(m.backspace());
        CHECK(m.erase());
        CHECK(str(m) == "caf|");
        CHECK_FALSE(m.erase());
        m.home(false);
        CHECK_FALSE(m.backspace());
        // Shift+arrows select; an arrow without shift goes to the selection's side.
        m.right(true);
        m.right(true);
        CHECK(str(m) == "^ca|f");
        CHECK(str(bmds::LineEditModel(m)) == "^ca|f");
        CHECK(std::string(m.selected().data(), m.selected().size()) == "ca");
        m.left(false);
        CHECK(str(m) == "|caf");
        m.selectAll();
        CHECK(m.insert("new"));
        CHECK(str(m) == "new|");
        m.end(false);
        m.setCursor(1, true);
        CHECK(m.backspace());
        CHECK(str(m) == "n|");
        // Controls left out; the limit kept on a character.
        CHECK(m.insert("a\tb\nc"));
        CHECK(str(m) == "nabc|");
        std::string big(bmds::LineEditModel::MaxBytes - 5, 'x');
        m.setText(StringView(big.data(), big.size()));
        CHECK(m.insert("\xc3\xa9\xc3\xa9\xc3\xa9")); // two fit (4 bytes), the third doesn't
        CHECK(m.text().size() == bmds::LineEditModel::MaxBytes - 1);
        CHECK_FALSE(m.insert("\xc3\xa9"));
        // A lower limit stops typing there; the text already longer stays.
        m.setMaxBytes(4);
        CHECK(m.text().size() == bmds::LineEditModel::MaxBytes - 1);
        m.setText("ab");
        CHECK(m.insert("cdef"));
        CHECK(m.text() == "abcd");
        CHECK_FALSE(m.insert("g"));
        m.setMaxBytes(bmds::LineEditModel::MaxBytes);
        // A cursor put inside a character snaps back to its start.
        m.setText("\xc3\xa9z");
        m.setCursor(1, false);
        CHECK(m.cursor() == 0);
}

TEST_CASE("TextWrap: words in lines") {
        CHECK(wrapped("There is already an icon named Terminal here.", 20) ==
              std::vector<std::string>{"There is already an", "icon named Terminal", "here."});
        CHECK(wrapped("a\nb  c", 10) == std::vector<std::string>{"a", "b  c"});
        CHECK(wrapped("a\n\nb", 10) == std::vector<std::string>{"a", "", "b"}); // an empty line kept empty
        CHECK(wrapped("abcdefghij", 4) == std::vector<std::string>{"abcd", "efgh", "ij"});
        CHECK(wrapped("  lead", 10) == std::vector<std::string>{"lead"});
        CHECK(wrapped("\xe3\x81\x82\xe3\x81\x82\xe3\x81\x82", 4) ==
              std::vector<std::string>{"\xe3\x81\x82\xe3\x81\x82", "\xe3\x81\x82"});
        CHECK(wrapped("one two three", 3, 2) == std::vector<std::string>{"one", "two"});
        CHECK(wrapped("", 10).empty());
}

TEST_CASE("Mnemonic: marks taken out, the key found, typed keys matched") {
        using bmds::Mnemonic::None;
        char       out[16];
        const auto strip = [&](const char *text) {
                return bmds::Mnemonic::strip(text, Span<char>(out, sizeof(out)));
        };
        auto m = strip("&OK");
        CHECK(std::string(out) == "OK");
        CHECK(m.length == 2);
        CHECK(m.at == 0);
        CHECK(m.key == 'o');
        m = strip("Re&name");
        CHECK(std::string(out) == "Rename");
        CHECK(m.at == 2);
        CHECK(m.key == 'n');
        // && is a &; a & at the end dropped; the first mark is the one.
        m = strip("Tom && &Jerry&");
        CHECK(std::string(out) == "Tom & Jerry");
        CHECK(m.at == 6);
        CHECK(m.key == 'j');
        m = strip("&a&b");
        CHECK(std::string(out) == "ab");
        CHECK(m.at == 0);
        m = strip("plain");
        CHECK(m.at == None);
        CHECK(m.key == 0);
        // A marked character of several bytes; the text cut on a character to fit.
        m = strip("&\xc3\xa9t\xc3\xa9");
        CHECK(std::string(out) == "\xc3\xa9t\xc3\xa9");
        CHECK(m.key == 0xe9);
        char small[4];
        m = bmds::Mnemonic::strip("ab\xc3\xa9", Span<char>(small, sizeof(small)));
        CHECK(std::string(small) == "ab");
        CHECK(m.length == 2);
        // Keys: the character, folded; with Alt and no character, the key's letter or digit.
        bmds::KeyEvent e;
        e.usage = bmds::HidKey::A + ('o' - 'a');
        e.codepoint = 'O';
        CHECK(bmds::Mnemonic::keyOf(e) == 'o');
        e.codepoint = 0;
        CHECK(bmds::Mnemonic::keyOf(e) == 0);
        e.modifiers = bmds::KeyEvent::LeftAlt;
        CHECK(bmds::Mnemonic::keyOf(e) == 'o');
        e.usage = bmds::HidKey::Digit0;
        CHECK(bmds::Mnemonic::keyOf(e) == '0');
        e.usage = bmds::HidKey::Digit1 + 2;
        CHECK(bmds::Mnemonic::keyOf(e) == '3');
        e.modifiers = bmds::KeyEvent::LeftCtrl;
        e.codepoint = 'c';
        CHECK(bmds::Mnemonic::keyOf(e) == 0);
        e.modifiers = 0;
        e.action = bmds::KeyEvent::Action::Release;
        CHECK(bmds::Mnemonic::keyOf(e) == 0);
}

TEST_CASE("ScrollModel: the value kept in range, steps, and the bar's parts") {
        using bmds::ScrollModel;
        using Part = ScrollModel::Part;
        ScrollModel m;
        m.setRange(1000, 200);
        m.setLineStep(16);
        CHECK(m.maximum() == 800);
        CHECK(m.pageStep() == 184);
        CHECK(!m.setValue(-5)); // already 0
        CHECK(m.setValue(5000));
        CHECK(m.value() == 800);
        CHECK(m.stepLines(-1));
        CHECK(m.value() == 784);
        CHECK(m.stepPages(-10));
        CHECK(m.value() == 0);
        CHECK(!m.act(Part::BackArrow));
        CHECK(m.act(Part::ForwardPage));
        CHECK(m.value() == 184);
        m.setRange(300, 200); // shrunk: the value follows
        CHECK(m.value() == 100);
        m.setRange(100, 200); // all of it shows
        CHECK(m.value() == 0);
        CHECK(!m.isScrollable());

        // A 216-pixel bar, 16-pixel arrows: a 184-pixel track, the thumb a fifth of it.
        m.setRange(1000, 200);
        ScrollModel::Geometry g = m.geometry(216, 16);
        CHECK(g.arrow == 16);
        CHECK(g.trackStart == 16);
        CHECK(g.trackLength == 184);
        CHECK(g.thumbLength == 36);
        CHECK(g.thumbStart == 16);
        CHECK(ScrollModel::partAt(g, 216, 0) == Part::BackArrow);
        CHECK(ScrollModel::partAt(g, 216, 20) == Part::Thumb);
        CHECK(ScrollModel::partAt(g, 216, 60) == Part::ForwardPage);
        CHECK(ScrollModel::partAt(g, 216, 215) == Part::ForwardArrow);
        CHECK(ScrollModel::partAt(g, 216, 216) == Part::None);
        CHECK(m.setValue(800));
        g = m.geometry(216, 16);
        CHECK(g.thumbStart + g.thumbLength == 200); // at the track's end
        CHECK(ScrollModel::partAt(g, 216, 100) == Part::BackPage);
        // Dragging: each place along the track is a value, the ends the ends.
        CHECK(m.valueForThumb(g, 0) == 0);
        CHECK(m.valueForThumb(g, 16 + 74) == 400);
        CHECK(m.valueForThumb(g, 1000) == 800);
        for (int at = 16; at <= 16 + 148; ++at) {
                ScrollModel d = m;
                d.setValue(d.valueForThumb(g, at));
                const int back = d.geometry(216, 16).thumbStart;
                REQUIRE((back >= at - 1 && back <= at + 1)); // the thumb stays under the pointer
        }
        // A long range: the thumb at its least.
        m.setRange(1'000'000, 200);
        CHECK(m.geometry(216, 16).thumbLength == ScrollModel::MinThumb);
        // A bar too short for its arrows: half each, no thumb.
        g = m.geometry(20, 16);
        CHECK(g.arrow == 10);
        CHECK(g.thumbLength == 0);
        CHECK(ScrollModel::partAt(g, 20, 9) == Part::BackArrow);
        CHECK(ScrollModel::partAt(g, 20, 10) == Part::ForwardArrow);
        // Nothing to scroll: the track is no part.
        m.setRange(10, 200);
        g = m.geometry(216, 16);
        CHECK(g.thumbLength == 0);
        CHECK(ScrollModel::partAt(g, 216, 100) == Part::None);
}

TEST_CASE("Furniture: menu bar titles, splitter sides, progress blocks, a dialog's buttons") {
        using bmds::ButtonRow;
        using bmds::MenuBarLayout;
        using bmds::ProgressLayout;
        using bmds::SplitterLayout;
        const uint16_t texts[] = {32, 40, 8};
        Rect           titles[3];
        MenuBarLayout::place(texts, 2, 1, 20, titles);
        CHECK(titles[0] == Rect{4, 1, 48, 20});
        CHECK(titles[1] == Rect{52, 1, 56, 20});
        CHECK(titles[2] == Rect{108, 1, 24, 20});
        CHECK(MenuBarLayout::titleAt(titles, 51, 5) == 0);
        CHECK(MenuBarLayout::titleAt(titles, 52, 5) == 1);
        CHECK(MenuBarLayout::titleAt(titles, 200, 5) == -1);
        CHECK(MenuBarLayout::titleAt(titles, 60, 21) == -1);
        CHECK(MenuBarLayout::neighbour(0, -1, 3) == 2);
        CHECK(MenuBarLayout::neighbour(2, 1, 3) == 0);
        CHECK(MenuBarLayout::neighbour(1, 1, 3) == 2);

        // 404 pixels: 400 for the sides, each at least 100 here, on columns.
        CHECK(SplitterLayout::firstSize(150, 404, 100, 100, 4) == 148);
        CHECK(SplitterLayout::firstSize(10, 404, 100, 100, 4) == 100);
        CHECK(SplitterLayout::firstSize(390, 404, 100, 100, 4) == 300);
        CHECK(SplitterLayout::firstSize(-50, 404, 0, 0, 1) == 0);
        CHECK(SplitterLayout::firstSize(150, 150, 100, 100, 4) == 100); // too small: the first side's least
        CHECK(SplitterLayout::firstSize(150, 60, 100, 100, 4) == 56);   // and no more than there is
        CHECK(SplitterLayout::firstSize(150, 2, 100, 100, 4) == 0);

        CHECK(ProgressLayout::capacity(124) == 10);
        CHECK(ProgressLayout::blocks(0, 100, 124) == 0);
        CHECK(ProgressLayout::blocks(99, 100, 124) == 9);
        CHECK(ProgressLayout::blocks(100, 100, 124) == 10);
        CHECK(ProgressLayout::blocks(500, 100, 124) == 10);
        CHECK(ProgressLayout::blocks(5, 0, 124) == 0);
        CHECK(ProgressLayout::blocks(uint64_t(1) << 40, uint64_t(1) << 41, 124) == 5); // large sizes

        const uint16_t labels[] = {16, 100}; // "OK" and a long one
        CHECK(ButtonRow::buttonWidth(16) == ButtonRow::MinWidth);
        CHECK(ButtonRow::buttonWidth(100) == 116);
        CHECK(ButtonRow::width(labels) == 76 + 8 + 116);
        Rect buttons[2];
        ButtonRow::place(labels, 400, 50, 22, buttons);
        CHECK(buttons[0] == Rect{200, 50, 76, 22});
        CHECK(buttons[1] == Rect{284, 50, 116, 22});
}

TEST_CASE("IconGrid: many items, wanted cells kept, the rest down then across and past the grid") {
        using Cell = bmds::IconGrid::Cell;
        std::vector<Cell> wanted(1000), out(1000);
        wanted[999] = Cell{0, 0}; // the last wants the first cell
        wanted[5] = Cell{0, 0};   // and an earlier one too: the earlier wins
        REQUIRE(bmds::IconGrid::place(bmds::Span<const Cell>(wanted.data(), 1000), 10, 8,
                                      bmds::Span<Cell>(out.data(), 1000))
                        .isOk());
        CHECK(out[5] == Cell{0, 0});
        CHECK(out[0] == Cell{0, 1});
        CHECK(out[999].isValid());
        // Every item in a cell of its own.
        std::vector<int> seen;
        for (const Cell &c : out) seen.push_back(c.column * 8 + c.row);
        std::sort(seen.begin(), seen.end());
        CHECK(std::adjacent_find(seen.begin(), seen.end()) == seen.end());
        CHECK(seen.back() == 999); // 1000 items fill cells 0 to 999 exactly
}

TEST_CASE("ItemSelection: only, toggles, ranges from the anchor, all and none") {
        bmds::ItemSelection s;
        REQUIRE(s.reset(100).isOk());
        CHECK(s.current() == -1);
        s.selectOnly(10);
        CHECK(s.isSelected(10));
        CHECK(s.selectedCount() == 1);
        s.selectRange(14, false); // 10 to 14
        CHECK(s.selectedCount() == 5);
        CHECK(s.current() == 14);
        CHECK(s.anchor() == 10);
        s.selectRange(8, false); // from the same anchor, the other way: 8 to 10
        CHECK(s.selectedCount() == 3);
        CHECK(!s.isSelected(14));
        s.toggle(50); // Ctrl: added, the new anchor
        CHECK(s.selectedCount() == 4);
        CHECK(s.anchor() == 50);
        s.selectRange(52, true); // Ctrl+Shift: added to
        CHECK(s.selectedCount() == 6);
        CHECK(s.nextSelected(0) == 8);
        CHECK(s.nextSelected(11) == 50);
        CHECK(s.nextSelected(53) == -1);
        s.toggle(50);
        CHECK(!s.isSelected(50));
        s.selectAll();
        CHECK(s.selectedCount() == 100);
        CHECK(s.nextSelected(99) == 99);
        s.clear();
        CHECK(s.selectedCount() == 0);
        CHECK(s.current() == 50); // the current row stays
        s.setCurrent(70, false);
        CHECK(s.anchor() == 50);
        s.selectOnly(-1);
        CHECK(s.current() == -1);
        REQUIRE(s.reset(0).isOk());
        CHECK(s.nextSelected(0) == -1);
}

TEST_CASE("IconLayout and HeaderLayout: flowing cells, arrows, the grid's nearest, columns and edges") {
        using bmds::IconLayout;
        CHECK(IconLayout::columnsFor(500) == 6);
        CHECK(IconLayout::columnsFor(10) == 1);
        CHECK(IconLayout::flowX(7, 6) == 80);
        CHECK(IconLayout::flowY(7, 6) == 76);
        CHECK(IconLayout::flowHeight(13, 6) == 3 * 76);
        CHECK(IconLayout::flowHeight(2000, 6) == 334 * 76); // past 65535: 32-bit
        CHECK(IconLayout::flowRowAt(85, 80, 6, 13) == 7);
        CHECK(IconLayout::flowRowAt(85, 160, 6, 13) == -1); // the third line has one item
        CHECK(IconLayout::flowRowAt(5, 160, 6, 13) == 12);
        CHECK(IconLayout::flowRowAt(490, 0, 6, 13) == -1); // past the columns
        CHECK(IconLayout::flowNeighbour(5, 1, 0, 6, 13) == 6);
        CHECK(IconLayout::flowNeighbour(0, -1, 0, 6, 13) == 0);
        CHECK(IconLayout::flowNeighbour(1, 0, 1, 6, 13) == 7);
        CHECK(IconLayout::flowNeighbour(7, 0, 1, 6, 13) == 12); // the short last line's last item
        CHECK(IconLayout::flowNeighbour(12, 0, 1, 6, 13) == 12);
        CHECK(IconLayout::flowNeighbour(12, 0, -1, 6, 13) == 6);
        using Cell = bmds::IconGrid::Cell;
        const Cell cells[] = {{0, 0}, {0, 1}, {1, 0}, {3, 2}, {1, 4}};
        CHECK(IconLayout::gridNeighbour(cells, 0, 0, 1) == 1);
        CHECK(IconLayout::gridNeighbour(cells, 0, 1, 0) == 2);
        CHECK(IconLayout::gridNeighbour(cells, 2, 1, 0) == 3);
        CHECK(IconLayout::gridNeighbour(cells, 3, 1, 0) == 3); // none that way
        CHECK(IconLayout::gridNeighbour(cells, 1, 0, 1) == 4);

        using bmds::HeaderLayout;
        const uint16_t widths[] = {200, 80, 120};
        CHECK(HeaderLayout::total(widths) == 400);
        CHECK(HeaderLayout::columnAt(widths, 0) == 0);
        CHECK(HeaderLayout::columnAt(widths, 200) == 1);
        CHECK(HeaderLayout::columnAt(widths, 399) == 2);
        CHECK(HeaderLayout::columnAt(widths, 400) == -1);
        CHECK(HeaderLayout::edgeAt(widths, 198) == 0);
        CHECK(HeaderLayout::edgeAt(widths, 283) == 1);
        CHECK(HeaderLayout::edgeAt(widths, 250) == -1);
        CHECK(HeaderLayout::resized(100, 13) == 112);
        CHECK(HeaderLayout::resized(100, -200) == HeaderLayout::MinWidth);
}

TEST_CASE("TreeRows: expanding inserts children, collapsing removes descendants, paths and parents") {
        bmds::TreeRows t;
        REQUIRE(t.addRoot("/sd0/p0").isOk());
        REQUIRE(t.addRoot("/boot").isOk());
        const bmds::StringView top[] = {"docs", "music", "zz"};
        REQUIRE(t.setChildren(0, top).isOk());
        CHECK(t.count() == 5);
        CHECK(t.name(1) == "docs");
        CHECK(t.row(1).depth == 1);
        CHECK((t.row(0).flags & bmds::TreeRows::Expanded) != 0);
        CHECK(t.name(4) == "/boot");
        const bmds::StringView sub[] = {"a", "b"};
        REQUIRE(t.setChildren(2, sub).isOk()); // music
        CHECK(t.count() == 7);
        CHECK(t.name(3) == "a");
        CHECK(t.parent(4) == 2);
        CHECK(t.parent(2) == 0);
        CHECK(t.parent(0) == -1);
        CHECK(t.endOf(0) == 6);
        bmds::String path;
        REQUIRE(t.pathOf(4, path).isOk());
        CHECK(path.view() == "/sd0/p0/music/b");
        CHECK(t.find("/sd0/p0/music/b") == 4);
        CHECK(t.find("/sd0/p0/nothing") == -1);
        CHECK(t.find("/boot") == 6);
        // An empty folder: no children, no + box.
        REQUIRE(t.setChildren(1, bmds::Span<const bmds::StringView>()).isOk());
        CHECK((t.row(1).flags & bmds::TreeRows::HasChildren) == 0);
        t.collapse(0);
        CHECK(t.count() == 2);
        CHECK(t.name(1) == "/boot");
        CHECK((t.row(0).flags & bmds::TreeRows::Expanded) == 0);
        // Expanded and collapsed many times: the names compacted, still right.
        std::vector<std::string>      names(200);
        std::vector<bmds::StringView> views;
        for (int i = 0; i < 200; ++i) names[i] = "folder number " + std::to_string(i);
        for (const std::string &n : names) views.emplace_back(n.data(), n.size());
        for (int round = 0; round < 20; ++round) {
                REQUIRE(t.setChildren(1, bmds::Span<const bmds::StringView>(views.data(), views.size())).isOk());
                t.collapse(1);
        }
        REQUIRE(t.setChildren(1, bmds::Span<const bmds::StringView>(views.data(), views.size())).isOk());
        CHECK(t.name(0) == "/sd0/p0");
        CHECK(t.name(2) == "folder number 0");
        CHECK(t.name(201) == "folder number 199");
}

TEST_CASE("TreeRows: a folder read again keeps what is open below the children still there") {
        bmds::TreeRows t;
        REQUIRE(t.addRoot("/").isOk());
        const bmds::StringView top[] = {"bin", "home", "sd0"};
        REQUIRE(t.setChildren(0, top).isOk());
        const bmds::StringView home[] = {"Desktop", "docs"};
        REQUIRE(t.setChildren(2, home).isOk());
        const bmds::StringView desktop[] = {"x"};
        REQUIRE(t.setChildren(3, desktop).isOk()); // /home/Desktop
        const bmds::StringView sd[] = {"p0"};
        REQUIRE(t.setChildren(6, sd).isOk());
        auto names = [&t] {
                std::string out;
                for (size_t i = 0; i < t.count(); ++i)
                        out += std::string(t.row(i).depth, '.') + std::string(t.name(i).data(), t.name(i).size()) + " ";
                return out;
        };
        REQUIRE(names() == "/ .bin .home ..Desktop ...x ..docs .sd0 ..p0 ");
        // A folder made in /, one gone, the open ones kept with what is below them, in the new order.
        const bmds::StringView again[] = {"boot", "home", "new", "sd0"};
        REQUIRE(t.setChildren(0, again).isOk());
        CHECK(names() == "/ .boot .home ..Desktop ...x ..docs .new .sd0 ..p0 ");
        CHECK((t.row(2).flags & bmds::TreeRows::Expanded) != 0);
        CHECK((t.row(1).flags & bmds::TreeRows::Expanded) == 0);
        bmds::String path;
        REQUIRE(t.pathOf(4, path).isOk());
        CHECK(path.view() == "/home/Desktop/x");
        // Another order, and one gone with what was open below it.
        const bmds::StringView turned[] = {"sd0", "home"};
        REQUIRE(t.setChildren(0, turned).isOk());
        CHECK(names() == "/ .sd0 ..p0 .home ..Desktop ...x ..docs ");
        const bmds::StringView fewer[] = {"docs"};
        REQUIRE(t.setChildren(3, fewer).isOk());
        CHECK(names() == "/ .sd0 ..p0 .home ..docs ");
        // Many rounds: the names compacted, still right.
        for (int round = 0; round < 300; ++round) {
                const bmds::StringView some[] = {"Desktop", "docs", round % 2 ? "odd" : "even"};
                REQUIRE(t.setChildren(3, some).isOk());
                REQUIRE(t.setChildren(4, desktop).isOk());
        }
        CHECK(names() == "/ .sd0 ..p0 .home ..Desktop ...x ..docs ..odd "); // round 299
}

TEST_CASE("ListItemModel: names, icons by size, columns") {
        bmds::ListItemModel    m;
        const uint32_t         px = 0;
        const bmds::Icon       small{16, 16, &px}, large{32, 32, &px};
        const bmds::StringView cols[] = {"12 KB", "Text"};
        REQUIRE(m.add("notes.txt", &small, &large, cols).isOk());
        REQUIRE(m.add("bare", nullptr, &large).isOk());
        CHECK(m.count() == 2);
        CHECK(m.text(0) == "notes.txt");
        CHECK(m.icon(0, 16) == &small);
        CHECK(m.icon(0, 32) == &large);
        CHECK(m.icon(1, 16) == &large); // the other size when there is only one
        char buf[16];
        CHECK(m.columnText(0, 1, buf) == "12 KB");
        CHECK(m.columnText(1, 2, buf).isEmpty());
        const bmds::StringView       titles[] = {"Name", "Size", "Type"};
        const uint16_t               widths[] = {180, 80, 100};
        const bmds::ItemModel::Align aligns[] = {bmds::ItemModel::Align::Left, bmds::ItemModel::Align::Right};
        m.setColumns(titles, widths, aligns);
        CHECK(m.columns() == 3);
        CHECK(m.columnTitle(1) == "Size");
        CHECK(m.columnWidth(2) == 100);
        CHECK(m.columnAlign(1) == bmds::ItemModel::Align::Right);
        CHECK(m.columnAlign(2) == bmds::ItemModel::Align::Left);
}

TEST_CASE("ItemSelection: a Shift range by a grid's rectangle, not the rows' order") {
        using bmds::IconLayout;
        using Cell = bmds::IconGrid::Cell;
        // A full grid of 4 columns by 3 rows, the rows (by name) in no order of place.
        const Cell          cells[] = {{0, 0}, {3, 2}, {0, 1}, {2, 0}, {1, 1}, {0, 2},
                                       {3, 0}, {1, 0}, {2, 2}, {1, 2}, {2, 1}, {3, 1}};
        bmds::ItemSelection s;
        REQUIRE(s.reset(12).isOk());
        const auto byGrid = [](const void *arg, size_t row, size_t anchor, size_t to) {
                const Cell *c = static_cast<const Cell *>(arg);
                return IconLayout::inGridRange(c[row], c[anchor], c[to]);
        };
        s.selectOnly(0);                        // the top-left cell
        s.selectRange(2, false, byGrid, cells); // the one under it
        CHECK(s.selectedCount() == 2);
        CHECK(s.isSelected(0));
        CHECK(s.isSelected(2));
        CHECK(!s.isSelected(5));                 // the bottom-left: not in it
        s.selectRange(10, false, byGrid, cells); // to (2,1): columns 0-2, rows 0-1
        CHECK(s.selectedCount() == 6);
        for (const size_t r : {size_t(0), size_t(2), size_t(3), size_t(4), size_t(7), size_t(10)})
                CHECK(s.isSelected(r));
        s.selectRange(1, false, byGrid, cells); // to the far corner: all
        CHECK(s.selectedCount() == 12);
        CHECK(s.anchor() == 0);
        s.selectRange(0, false, byGrid, cells); // back to the anchor: it alone
        CHECK(s.selectedCount() == 1);
        // The rows' order (a flowing view's) is the other rule: rows 0 to 2.
        s.selectRange(2, false);
        CHECK(s.selectedCount() == 3);
        CHECK(s.isSelected(1));
}

TEST_CASE("SliderModel: a signed range, steps, and the handle's place both ways") {
        bmds::SliderModel m;
        m.setRange(0, -61); // backwards: swapped
        CHECK(m.minimum() == -61);
        CHECK(m.maximum() == 0);
        CHECK(m.value() == 0);
        CHECK_FALSE(m.setValue(5)); // kept at the maximum
        CHECK(m.setValue(-9));
        CHECK(m.value() == -9);
        m.setSteps(1, 6);
        CHECK(m.step(-2));
        CHECK(m.value() == -11);
        CHECK(m.stepPage(-10));
        CHECK(m.value() == -61);
        CHECK_FALSE(m.step(-1));
        // Horizontal: the minimum at the start. A 72-pixel slider, an 11-pixel handle: 61 to travel.
        CHECK(m.handleStart(72, 11) == 0);
        (void)m.setValue(0);
        CHECK(m.handleStart(72, 11) == 61);
        // Vertical (inverted): the maximum at the top.
        m.setInverted(true);
        CHECK(m.handleStart(72, 11) == 0);
        (void)m.setValue(-61);
        CHECK(m.handleStart(72, 11) == 61);
        // Back from a place: every value's handle maps back to it, and past the ends clamps.
        for (int v = -61; v <= 0; ++v) {
                (void)m.setValue(v);
                CHECK(m.valueAt(m.handleStart(133, 11), 133, 11) == v);
        }
        CHECK(m.valueAt(-50, 133, 11) == 0);
        CHECK(m.valueAt(500, 133, 11) == -61);
        // No room to travel, or no range: the minimum, at the start.
        CHECK(m.handleStart(10, 11) == 0);
        CHECK(m.valueAt(3, 10, 11) == -61);
}

TEST_CASE("TabRow: tabs side by side on columns, found by x, stepped around the ends") {
        const uint16_t texts[] = {64, 37, 0};
        uint16_t       lefts[3], widths[3];
        CHECK(bmds::TabRow::place(texts, 2, lefts, widths) == 2 + 80 + 56 + 16);
        CHECK(lefts[0] == 2);
        CHECK(widths[0] == 80);
        CHECK(lefts[1] == 82);
        CHECK(widths[1] == 56); // 37 + 16, up to a column
        CHECK(lefts[2] == 138);
        CHECK(widths[2] == 16);
        CHECK(bmds::TabRow::at(lefts, widths, 1) == -1);
        CHECK(bmds::TabRow::at(lefts, widths, 2) == 0);
        CHECK(bmds::TabRow::at(lefts, widths, 81) == 0);
        CHECK(bmds::TabRow::at(lefts, widths, 82) == 1);
        CHECK(bmds::TabRow::at(lefts, widths, 154) == -1);
        CHECK(bmds::TabRow::step(0, 3, 1) == 1);
        CHECK(bmds::TabRow::step(2, 3, 1) == 0);
        CHECK(bmds::TabRow::step(0, 3, -1) == 2);
        CHECK(bmds::TabRow::step(1, 3, -7) == 0);
        CHECK(bmds::TabRow::step(5, 0, 1) == 0);
}

TEST_CASE("FileNameList: one name as it is, several in quotes, written back") {
        bmds::List<bmds::StringView> names(bmds::MemSpace::Psram);
        REQUIRE(bmds::FileNameList::split("  my song.mp3 ", names).isOk());
        REQUIRE(names.size() == 1);
        CHECK(names[0] == "my song.mp3");
        REQUIRE(bmds::FileNameList::split(R"("a b.mp3" "c.ogg"  " " "d)", names).isOk());
        REQUIRE(names.size() == 3);
        CHECK(names[0] == "a b.mp3");
        CHECK(names[1] == "c.ogg");
        CHECK(names[2] == "d"); // unclosed: to the end
        REQUIRE(bmds::FileNameList::split("   ", names).isOk());
        CHECK(names.isEmpty());
        bmds::String out(bmds::MemSpace::Psram);
        REQUIRE(bmds::FileNameList::appendQuoted("a b.mp3", out).isOk());
        REQUIRE(bmds::FileNameList::appendQuoted("c.ogg", out).isOk());
        CHECK(out.view() == R"("a b.mp3" "c.ogg")");
        REQUIRE(bmds::FileNameList::split(out.view(), names).isOk());
        CHECK(names.size() == 2);
}
