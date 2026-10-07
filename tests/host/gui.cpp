/**
 * @file      gui.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The toolkit's pure parts (devplan/gui.md): configuration files, launcher
 * entries, menu geometry, line layout.
 */

#include <doctest.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>
#include <bmds/bevel.h>
#include <bmds/desktopentry.h>
#include <bmds/font.h>
#include <bmds/inireader.h>
#include <bmds/linelayout.h>
#include <bmds/menulayout.h>
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
