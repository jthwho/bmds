/**
 * @file      settingstable.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Settings' pages as data (devplan/controlpanel.md §5): every system setting's place, label
 * and words. A new setting gets its row here (`settings -u` lists the keys no row places).
 */

#include <cstring>

#include "settingstable.h"

BMDS_NAMESPACE_BEGIN

namespace SettingsTable {

        namespace {

                using E = Editor;

                constexpr Row section(const char *title) {
                        Row r;
                        r.label = title;
                        return r;
                }

                // Conditions several rows share.
                constexpr const char *Static = "net/*/enabled=true,net/*/ipv4/mode=static";
                constexpr const char *PtpMaster = "net/ptp/enabled=true,net/ptp/slave-only=false";
                constexpr const char *Ssh = "net/ssh/server=true";

                constexpr Row GeneralRows[] = {
                        section("Name"),
                        {.key = "system/hostname", .label = "Name"},
                        section("Password"),
                        {.key = "system/password",
                         .label = "Password",
                         .help = "Logging in over SSH, and anything else that asks, needs it (none: no password "
                                 "logins)",
                         .editor = E::Password},
                        section("Home"),
                        {.key = "home", .label = "Home folder", .editor = E::Folder},
                };

                // A color's row: its label, no help (the label says it; the preview shows it).
                constexpr Row color(const char *key, const char *label) {
                        Row r;
                        r.key = key;
                        r.label = label;
                        r.help = "";
                        return r;
                }

                constexpr Row AppearanceRows[] = {
                        section("Desktop"),
                        color("wm/palette/desktop", "Desktop"),
                        color("wm/palette/desktop-text", "Icon names"),
                        section("Windows"),
                        color("wm/palette/active-frame", "Active frame"),
                        color("wm/palette/active-title", "Active title"),
                        color("wm/palette/inactive-frame", "Inactive frame"),
                        color("wm/palette/inactive-title", "Inactive title"),
                        color("wm/palette/close-box", "Close button"),
                        color("wm/palette/close-mark", "Close mark"),
                        color("wm/palette/button", "Title buttons"),
                        color("wm/palette/button-mark", "Their marks"),
                        color("wm/palette/client", "Empty window"),
                        color("wm/palette/outline", "Moving outline"),
                        section("Controls"),
                        color("wm/palette/face", "Face"),
                        color("wm/palette/face-text", "Text"),
                        color("wm/palette/disabled-text", "Disabled text"),
                        color("wm/palette/face-highlight", "Fields, lit edge"),
                        color("wm/palette/face-light", "Lit edge, inner"),
                        color("wm/palette/face-shadow", "Shaded edge, inner"),
                        color("wm/palette/face-dark", "Shaded edge"),
                        color("wm/palette/highlight", "Selection"),
                        color("wm/palette/highlight-text", "Selected text"),
                        section("Tooltips"),
                        color("wm/palette/tooltip", "Tooltip"),
                        color("wm/palette/tooltip-text", "Tooltip text"),
                        section("Pointer"),
                        color("wm/palette/cursor-fill", "Pointer"),
                        color("wm/palette/cursor-edge", "Pointer edge"),
                        {.key = "wm/palette/*", .help = ""}, // a role added later
                        section("Icons"),
                        {.key = "panel/icon-theme", .label = "Icon theme"},
                };

                constexpr Row DesktopRows[] = {
                        section("Panel"),
                        {.key = "panel/edge",
                         .label = "Position",
                         .choices = "top=Top of the screen|bottom=Bottom of the screen"},
                        {.key = "panel/items",
                         .label = "Items",
                         .help = "In order: launcher, windows, tray, clock, spacer",
                         .editor = E::List},
                        {.key = "panel/windows/max-width",
                         .label = "Window buttons",
                         .unit = "pixels at most",
                         .when = "panel/items~windows"},
                        {.key = "panel/spacer/width",
                         .label = "Spacer",
                         .unit = "pixels (0: what is left)",
                         .when = "panel/items~spacer"},
                        section("Tray"),
                        {.key = "panel/tray/cpu", .label = "CPU meter"},
                        {.key = "panel/tray/cpu-graph",
                         .label = "CPU graph",
                         .choices = "cores=A band for each core|combined=All cores together",
                         .when = "panel/tray/cpu=true"},
                        {.key = "panel/tray/volume", .label = "Volume control"},
                        {.key = "panel/tray/network", .label = "Network"},
                        {.key = "panel/tray/notifications", .label = "Notifications"},
                        section("Apps menu"),
                        {.key = "panel/launcher/label", .label = "Label", .when = "panel/items~launcher"},
                        {.key = "panel/launcher/icon", .label = "Icon", .when = "panel/items~launcher"},
                        {.key = "panel/launcher/applications", .label = "Entries from"},
                        {.key = "panel/launcher/submenus", .label = "Group past", .unit = "entries (0: never)"},
                        section("Desktop"),
                        {.key = "panel/desktop/menu", .label = "Desktop menu"},
                        section("Games"),
                        {.key = "wm/controls", .label = "Controls command"},
                };

                constexpr Row DisplayRows[] = {
                        section("Mode"),
                        {.key = "display/mode", .label = "Mode", .editor = E::Mode, .flags = AtStart},
                        {.key = "display/buffering",
                         .label = "Buffering",
                         .choices = "double=Double (no tearing)|single=Single (half the memory)",
                         .flags = AtStart},
                        section("Full screen"),
                        {.key = "display/fullscreen-modes", .label = "Monitor's own modes"},
                        section("Screenshots"),
                        {.key = "screenshot/path", .label = "Saved as"},
                        section("Advanced"),
                        {.key = "display/lanembps", .label = "DSI lane rate", .unit = "Mbps", .flags = AtStart},
                };

                constexpr Row SoundRows[] = {
                        section("Output"),
                        {.key = "audio/default", .label = "Device"},
                        {.key = "audio/*/volume", .label = "Volume", .unit = "dB", .editor = E::Slider},
                        {.key = "audio/*/mute", .label = "Muted"},
                        section("Alerts"),
                        {.key = "audio/bell", .label = "Terminal bell", .choices = "sound=Sound|off=Off"},
                        {.key = "audio/feedback", .label = "Feedback sounds"},
                        section("Synthesizer"),
                        {.key = "synth/font", .label = "SoundFont", .editor = E::File},
                        {.key = "synth/gain", .label = "Gain", .unit = "dB"},
                        {.key = "synth/keep-fonts", .label = "Keep fonts loaded"},
                        section("MIDI"),
                        {.key = "midi/default", .label = "Default port"},
                        {.key = "midi/connect", .label = "Connections", .editor = E::List, .separator = ','},
                        section("Speech"),
                        {.key = "speech/voice", .label = "Voice"},
                        {.key = "speech/path", .label = "Voices from", .editor = E::List, .separator = ':'},
                        section("Advanced"),
                        {.key = "audio/*/rate", .label = "Rate", .unit = "Hz"},
                        {.key = "audio/*/period", .label = "Period", .unit = "frames"},
                };

                constexpr Row NotificationRows[] = {
                        {.key = "notify/balloons", .label = "Balloons"},
                        {.key = "notify/timeout", .label = "Shown for", .unit = "ms", .when = "notify/balloons=true"},
                        {.key = "notify/sound", .label = "Sound"},
                        {.key = "notify/quiet", .label = "Do Not Disturb"},
                };

                constexpr Row TimeRows[] = {
                        section("Time zone"),
                        {.key = "clock/zone", .label = "Time zone", .editor = E::Zone},
                        section("Network time"),
                        {.key = "net/ntp/enabled", .label = "Set automatically"},
                        {.key = "net/ntp/servers",
                         .label = "Servers",
                         .editor = E::List,
                         .when = "net/ntp/enabled=true"},
                        section("Panel clock"),
                        {.key = "panel/clock/format", .label = "Format", .when = "panel/items~clock"},
                        {.key = "panel/clock/tooltip", .label = "Tooltip", .when = "panel/items~clock"},
                        {.key = "panel/clock/zone",
                         .label = "Time zone",
                         .editor = E::Zone,
                         .flags = Empty,
                         .when = "panel/items~clock"},
                        section("Precision time (PTP)"),
                        {.key = "net/ptp/enabled", .label = "PTP"},
                        {.key = "net/ptp/interface", .label = "Interface", .when = "net/ptp/enabled=true"},
                        {.key = "net/ptp/domain", .label = "Domain", .when = "net/ptp/enabled=true"},
                        {.key = "net/ptp/profile",
                         .label = "Profile",
                         .choices = "media=Media (AES67)|default=Default (1588)",
                         .when = "net/ptp/enabled=true"},
                        {.key = "net/ptp/transport",
                         .label = "Transport",
                         .choices = "udp=UDP/IPv4|l2=Ethernet (layer 2)",
                         .when = "net/ptp/enabled=true"},
                        {.key = "net/ptp/priority1", .label = "Priority 1", .when = PtpMaster},
                        {.key = "net/ptp/priority2", .label = "Priority 2", .when = PtpMaster},
                        {.key = "net/ptp/slave-only", .label = "Never master", .when = "net/ptp/enabled=true"},
                        {.key = "net/ptp/sync-interval",
                         .label = "Sync interval",
                         .choices = "auto=Automatic",
                         .when = PtpMaster},
                        {.key = "net/ptp/announce-interval",
                         .label = "Announce interval",
                         .choices = "auto=Automatic",
                         .when = PtpMaster},
                        {.key = "net/ptp/delay-interval",
                         .label = "Delay interval",
                         .choices = "auto=Automatic",
                         .when = PtpMaster},
                        section("Advanced"),
                        {.key = "clock/tzdata", .label = "Zone database", .editor = E::File},
                        {.key = "clock/leapseconds", .label = "Leap seconds", .editor = E::File},
                };

                constexpr Row NetworkRows[] = {
                        section("Ethernet"),
                        {.key = "net/*/enabled", .label = "Use %s", .flags = Confirm},
                        {.key = "net/*/ipv4/mode",
                         .label = "IPv4 (%s)",
                         .choices = "dhcp=Automatic (DHCP)|static=Manual|off=Off",
                         .flags = Confirm,
                         .when = "net/*/enabled=true"},
                        {.key = "net/*/ipv4/address", .label = "Address", .flags = Confirm, .when = Static},
                        {.key = "net/*/ipv4/gateway", .label = "Router", .flags = Confirm, .when = Static},
                        {.key = "net/*/mtu",
                         .label = "MTU",
                         .unit = "bytes",
                         .flags = Confirm,
                         .when = "net/*/enabled=true"},
                        {.key = "net/*/mac", .label = "MAC address", .flags = Confirm, .when = "net/*/enabled=true"},
                        section("DNS"),
                        {.key = "net/dns/servers", .label = "Servers", .editor = E::List},
                        {.key = "net/dns/search", .label = "Search domains", .editor = E::List},
                        section("Hosts"),
                        {.key = "net/hosts", .label = "Hosts", .editor = E::List},
                        section("Local name"),
                        {.key = "net/mdns/enabled", .label = "Answer .local"},
                };

                constexpr Row SharingRows[] = {
                        section("SSH"),
                        {.key = "net/ssh/server", .label = "SSH server", .flags = Confirm},
                        {.key = "net/ssh/port", .label = "Port", .flags = Confirm, .when = Ssh},
                        {.key = "net/ssh/password-login", .label = "Password logins", .when = Ssh},
                        {.key = "net/ssh/host-keys", .label = "Host keys", .editor = E::Folder, .when = Ssh},
                        {.key = "net/ssh/authorized-keys-dir", .label = "Authorized keys", .when = Ssh},
                };

                constexpr Row InputRows[] = {
                        section("Gamepads"),
                        {.key = "input/gamepads", .label = "Your mappings", .editor = E::File},
                };

                constexpr Row TerminalRows[] = {
                        section("Colors"),
                        color("terminal/colors/text", "Text"),
                        color("terminal/colors/background", "Background"),
                        color("terminal/colors/cursor", "Cursor"),
                        section("The 16 colors"),
                        color("terminal/colors/color0", "Black"),
                        color("terminal/colors/color1", "Red"),
                        color("terminal/colors/color2", "Green"),
                        color("terminal/colors/color3", "Yellow"),
                        color("terminal/colors/color4", "Blue"),
                        color("terminal/colors/color5", "Magenta"),
                        color("terminal/colors/color6", "Cyan"),
                        color("terminal/colors/color7", "White"),
                        color("terminal/colors/color8", "Bright black"),
                        color("terminal/colors/color9", "Bright red"),
                        color("terminal/colors/color10", "Bright green"),
                        color("terminal/colors/color11", "Bright yellow"),
                        color("terminal/colors/color12", "Bright blue"),
                        color("terminal/colors/color13", "Bright magenta"),
                        color("terminal/colors/color14", "Bright cyan"),
                        color("terminal/colors/color15", "Bright white"),
                        {.key = "terminal/colors/*", .help = ""},
                        section("Clipboard"),
                        {.key = "terminal/clipboard/allow-write", .label = "Programs may copy"},
                        {.key = "terminal/clipboard/allow-read", .label = "Programs may paste"},
                        section("Shell"),
                        {.key = "shell/path", .label = "Command path", .editor = E::List, .separator = ':'},
                };

                constexpr Row FilesRows[] = {
                        section("Folders"),
                        {.key = "files/default-view",
                         .label = "New folders as",
                         .choices = "icons=Icons|details=Details"},
                        {.key = "files/show-hidden", .label = "Hidden files"},
                        section("Applications"),
                        {.key = "apps/path", .label = "Applications from", .editor = E::List},
                };

                constexpr Row SystemRows[] = {
                        section("Logging"),
                        {.key = "log/level",
                         .label = "Level",
                         .choices = "info=Information|warn=Warnings|err=Errors|debug=Debugging"},
                        {.key = "log/debug", .label = "Debug modules", .editor = E::List},
                        {.key = "log/dir", .label = "Log files in", .editor = E::Folder},
                        {.key = "log/file-bytes", .label = "File size", .unit = "bytes", .when = "log/dir!="},
                        {.key = "log/files", .label = "Files kept", .when = "log/dir!="},
                        section("Memory"),
                        {.key = "tmp/size", .label = "/tmp at most", .unit = "bytes", .flags = AtBoot},
                        {.key = "clipboard/limit", .label = "Clipboard at most", .unit = "bytes"},
                        {.key = "image/decode-max", .label = "Pictures at most", .unit = "megapixels"},
                        section("Monitoring"),
                        {.key = "sysmon/enable", .label = "CPU time", .flags = AtBoot},
                        {.key = "sysmon/interval", .label = "Sampled every", .unit = "ms"},
                        {.key = "sysmon/limits", .label = "Rules"},
                        section("Updates"),
                        {.key = "update/dir", .label = "Packages in", .editor = E::Folder},
                };

                constexpr Page Pages[] = {
                        {"general", "General", "preferences-system", Span<const Row>(GeneralRows)},
                        {"appearance", "Appearance", "preferences-desktop-theme", Span<const Row>(AppearanceRows)},
                        {"desktop", "Desktop & Panel", "preferences-desktop-panel", Span<const Row>(DesktopRows)},
                        {"display", "Display", "preferences-desktop-display", Span<const Row>(DisplayRows)},
                        {"sound", "Sound", "audio-volume-high", Span<const Row>(SoundRows)},
                        {"notifications", "Notifications", "preferences-desktop-notification",
                         Span<const Row>(NotificationRows)},
                        {"time", "Date & Time", "preferences-system-time", Span<const Row>(TimeRows)},
                        {"network", "Network", "network-wired", Span<const Row>(NetworkRows)},
                        {"sharing", "Sharing", "preferences-system-network", Span<const Row>(SharingRows)},
                        {"input", "Keyboard & Gamepads", "input-gaming", Span<const Row>(InputRows)},
                        {"terminal", "Terminal", "utilities-terminal", Span<const Row>(TerminalRows)},
                        {"files", "Files", "folder", Span<const Row>(FilesRows)},
                        {"system", "System", "applications-system", Span<const Row>(SystemRows)},
                        {"all", "All Settings", "preferences-other", Span<const Row>()},
                };

                char lower(char c) {
                        return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
                }

                // @p text into @p out, cut to fit (a NUL after it). @return Its length.
                size_t put(StringView text, Span<char> out) {
                        if (out.isEmpty()) return 0;
                        const size_t n = text.size() < out.size() - 1 ? text.size() : out.size() - 1;
                        std::memcpy(out.data(), text.data(), n);
                        out[n] = '\0';
                        return n;
                }

        } // namespace

        Span<const Page> pages() {
                return Span<const Page>(Pages);
        }

        int findPage(StringView id) {
                for (size_t i = 0; i < sizeof(Pages) / sizeof(Pages[0]); ++i)
                        if (id == StringView(Pages[i].id)) return int(i);
                return -1;
        }

        bool matches(StringView pattern, StringView key, StringView &star) {
                star = StringView();
                size_t p = 0, k = 0;
                for (;;) {
                        const size_t     pe = pattern.mid(p).indexOf('/'), ke = key.mid(k).indexOf('/');
                        const StringView pc = pattern.mid(p, pe), kc = key.mid(k, ke);
                        if (pc == StringView("*")) {
                                if (kc.isEmpty()) return false;
                                if (star.isEmpty()) star = kc;
                        } else if (pc != kc) {
                                return false;
                        }
                        if (pe == StringView::NPos || ke == StringView::NPos) return pe == ke;
                        p += pe + 1;
                        k += ke + 1;
                }
        }

        Place place(StringView key) {
                // A row naming the key wins over a pattern matching it (net/mdns/enabled over
                // net/ * /enabled); then the first in the pages' order.
                Place at;
                for (int pass = 0; pass < 2; ++pass) {
                        for (size_t i = 0; i < sizeof(Pages) / sizeof(Pages[0]); ++i) {
                                const Row *heading = nullptr;
                                for (const Row &r : Pages[i].rows) {
                                        if (r.key == nullptr) {
                                                heading = &r;
                                                continue;
                                        }
                                        const bool pattern = StringView(r.key).indexOf('*') != StringView::NPos;
                                        if (pattern != (pass == 1)) continue;
                                        StringView star;
                                        if (!matches(StringView(r.key), key, star)) continue;
                                        at.page = int(i);
                                        at.row = &r;
                                        at.section = heading;
                                        at.star = star;
                                        return at;
                                }
                        }
                }
                return at;
        }

        size_t labelOf(StringView word, Span<char> out) {
                const size_t n = put(word, out);
                for (size_t i = 0; i < n; ++i)
                        if (out[i] == '-') out[i] = ' ';
                if (n > 0 && out[0] >= 'a' && out[0] <= 'z') out[0] = char(out[0] - 'a' + 'A');
                return n;
        }

        size_t label(const Row &row, StringView key, Span<char> out) {
                if (row.label == nullptr) {
                        const size_t slash = key.lastIndexOf('/');
                        return labelOf(slash == StringView::NPos ? key : key.mid(slash + 1), out);
                }
                StringView star;
                if (row.key != nullptr) (void)matches(StringView(row.key), key, star);
                const StringView l(row.label);
                const size_t     at = l.indexOf(StringView("%s"));
                if (at == StringView::NPos || out.isEmpty()) return put(l, out);
                size_t n = put(l.left(at), out);
                n += put(star, Span<char>(out.data() + n, out.size() - n));
                n += put(l.mid(at + 2), Span<char>(out.data() + n, out.size() - n));
                return n;
        }

        size_t choiceLabel(const char *choices, StringView word, Span<char> out) {
                StringView rest(choices != nullptr ? choices : "");
                while (!rest.isEmpty()) {
                        const size_t     bar = rest.indexOf('|');
                        const StringView pair = rest.left(bar);
                        const size_t     eq = pair.indexOf('=');
                        if (eq != StringView::NPos && pair.left(eq) == word) return put(pair.mid(eq + 1), out);
                        if (bar == StringView::NPos) break;
                        rest = rest.mid(bar + 1);
                }
                // Capitalized only: a word's dash may be its sign (-3).
                const size_t n = put(word, out);
                if (n > 0 && out[0] >= 'a' && out[0] <= 'z') out[0] = char(out[0] - 'a' + 'A');
                return n;
        }

        bool applies(const char *when, StringView star, ValueOf valueOf, void *arg) {
                StringView rest(when != nullptr ? when : "");
                while (!rest.isEmpty()) {
                        const size_t     comma = rest.indexOf(',');
                        const StringView cond = rest.left(comma);
                        rest = comma == StringView::NPos ? StringView() : rest.mid(comma + 1);
                        // KEY, the operator, the value(s).
                        size_t op = 0;
                        while (op < cond.size() && cond[op] != '=' && cond[op] != '!' && cond[op] != '~') ++op;
                        if (op == cond.size()) return false;
                        const char   kind = cond[op];
                        const size_t valueAt = kind == '!' ? op + 2 : op + 1;
                        if (kind == '!' && (op + 1 >= cond.size() || cond[op + 1] != '=')) return false;
                        const StringView pattern = cond.left(op), wanted = cond.mid(valueAt);
                        char             key[96];
                        size_t           n = 0;
                        for (size_t i = 0; i < pattern.size() && n + star.size() < sizeof(key) - 1; ++i) {
                                if (pattern[i] == '*') {
                                        std::memcpy(key + n, star.data(), star.size());
                                        n += star.size();
                                } else {
                                        key[n++] = pattern[i];
                                }
                        }
                        char value[256];
                        if (!valueOf(arg, StringView(key, n), value, sizeof(value))) return false;
                        const StringView v(value);
                        bool             holds = false;
                        if (kind == '~') {
                                for (StringView items = v; !items.isEmpty();) {
                                        const size_t semi = items.indexOf(';');
                                        holds = holds || items.left(semi) == wanted;
                                        items = semi == StringView::NPos ? StringView() : items.mid(semi + 1);
                                }
                        } else {
                                for (StringView choices = wanted;;) {
                                        const size_t bar = choices.indexOf('|');
                                        holds = holds || choices.left(bar) == v;
                                        if (bar == StringView::NPos) break;
                                        choices = choices.mid(bar + 1);
                                }
                                if (kind == '!') holds = !holds;
                        }
                        if (!holds) return false;
                }
                return true;
        }

        bool containsFolded(StringView text, StringView needle) {
                if (needle.isEmpty()) return true;
                for (size_t i = 0; i + needle.size() <= text.size(); ++i) {
                        size_t j = 0;
                        while (j < needle.size() && lower(text[i + j]) == lower(needle[j])) ++j;
                        if (j == needle.size()) return true;
                }
                return false;
        }

} // namespace SettingsTable

BMDS_NAMESPACE_END
