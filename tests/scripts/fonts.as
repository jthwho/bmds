// The system's fonts (on the host, unscii and the IBM Plex files): opening, sharing, metrics,
// measuring and drawing.
Font sans("IBM Plex Sans 14 bold");
println(sans.name + " | " + sans.family + " | " + sans.style + " | " + sans.size);
println(sans.bold + " " + sans.italic + " " + sans.bitmap + " " + sans.monospace);
println((sans.ascent > 0) + " " + (sans.descent > 0) + " " + (sans.lineHeight >= sans.ascent + sans.descent));
Font@ same = Font("IBM Plex Sans", 14, Bold);
println(same is sans); // one font, shared
Font mono("IBM Plex Mono", 20);
println(mono.name + " " + mono.monospace + " " + (mono.width("iiii") == mono.width("MMMM")));
Font ui("ui");
println(ui.name + " " + ui.bitmap + " " + ui.lineHeight + " " + ui.width("abc"));
Font big("unscii 32");
println(big.width("abc") + " " + big.lineHeight);
println((sans.width("Hello, world") > sans.width("Hello")) + " " + sans.width(""));
println(sans.width("ab\nabcdef") == sans.width("abcdef")); // the widest line
int fits = sans.fit("Hello, world", sans.width("Hello"));
println(fits + " " + sans.fit("Hello", 1000) + " " + sans.fit("Hello", 0) + " " + sans.fit("one\ntwo", 1000));
Font nearest("Nonesuch 12");
println(nearest.family);
string[]@ families = fontFamilies();
for (uint i = 0; i < families.length(); i++) println(families[i]);

Image img(120, 40);
img.clear(Color::Navy);
int w = img.text(2, 2, "Hi", Color::White, sans);
println((w == sans.width("Hi")) + " " + (img.pixel(0, 0) == Color::Navy));
bool inked = false;
for (int y = 2; y < 2 + sans.lineHeight; y++)
        for (int x = 2; x < 2 + w; x++)
                if (img.pixel(x, y) != Color::Navy) inked = true;
println(inked);
// On a box, sharp: only the two colours.
img.clear(Color::Navy);
w = img.text(10, 10, 42, Color::Black, Color::Yellow, mono, Sharp);
bool twoColours = true;
for (int y = 10; y < 10 + mono.lineHeight; y++)
        for (int x = 10; x < 10 + w; x++)
                if (img.pixel(x, y) != Color::Black && img.pixel(x, y) != Color::Yellow) twoColours = false;
println(twoColours + " " + (img.pixel(10 + w - 1, 10) == Color::Yellow) + " " + (img.pixel(9, 10) == Color::Navy));
// Smoothed: colours in between.
img.clear(Color::Black);
img.text(0, 0, "Oo", Color::White, sans, Smooth);
bool between = false;
for (int y = 0; y < sans.lineHeight; y++)
        for (int x = 0; x < 30; x++)
                if (img.pixel(x, y) != Color::Black && img.pixel(x, y) != Color::White) between = true;
println(between);
Font@ none;
println(img.text(0, 0, "abc", Color::White, none) == textWidth("abc")); // null: unscii

try { Font huge("IBM Plex Sans", 300); } catch { println(getExceptionInfo()); }
try { Font odd("IBM Plex Sans", 14, 8); } catch { println(getExceptionInfo()); }
try { Font empty(""); } catch { println(getExceptionInfo()); }
