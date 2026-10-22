#!/rom/bin/angel
// A piano: click the keys (drag across them for a glissando), or play them on the keyboard:
// A S D F G H J K the white keys, W E T Y U the black ones; hold several for a chord. Z and X
// change the octave, 1-4 the instrument. A note sounds from noteOn() while its key is down to
// noteOff() when it comes up, as on a real keyboard.

Window w("Piano", 480, 220);

// The keys: the computer key that plays each, its place in the octave, and where it is.
class Key {
        string letter;
        int semitone; // from C
        int x, y, width, height;
        bool black;
        bool down = false;
        int note = -1; // what it's sounding now (-1: nothing)
        Key() {}
        Key(const string &in l, int s, int kx, bool b) {
                letter = l;
                semitone = s;
                x = kx;
                y = 40;
                black = b;
                width = b ? 32 : 52;
                height = b ? 90 : 150;
        }
        bool contains(int px, int py) const {
                return px >= x && px < x + width && py >= y && py < y + height;
        }
}

Key[] keys;
string[] whiteLetters = {"a", "s", "d", "f", "g", "h", "j", "k"};
int[] whiteSemitones = {0, 2, 4, 5, 7, 9, 11, 12};
for (uint i = 0; i < whiteLetters.length(); i++) keys.insertLast(Key(whiteLetters[i], whiteSemitones[i], 20 + int(i) * 55, false));
string[] blackLetters = {"w", "e", "", "t", "y", "u"};
int[] blackSemitones = {1, 3, 0, 6, 8, 10};
for (uint i = 0; i < blackLetters.length(); i++)
        if (blackLetters[i] != "") keys.insertLast(Key(blackLetters[i], blackSemitones[i], 20 + int(i) * 55 + 37, true));

int[] instruments = {0, 19, 40, 80}; // piano, organ, violin, square lead
string[] instrumentNames = {"piano", "organ", "violin", "square"};
int octave = 4, chosen = 0;    // octave 4: C is middle C (note 60)
int mouseKey = -1;              // the key the mouse holds down, or -1

// The key under a point: the black ones lie on top of the white.
int keyAt(int x, int y) {
        for (int pass = 0; pass < 2; pass++)
                for (uint i = 0; i < keys.length(); i++)
                        if (keys[i].black == (pass == 0) && keys[i].contains(x, y)) return int(i);
        return -1;
}

int keyFor(const string &in letter) {
        for (uint i = 0; i < keys.length(); i++)
                if (keys[i].letter == letter) return int(i);
        return -1;
}

void press(int k) {
        if (k < 0 || keys[k].down) return;
        keys[k].down = true;
        keys[k].note = 12 * (octave + 1) + keys[k].semitone;
        noteOn(keys[k].note);
}

void release(int k) {
        if (k < 0 || !keys[k].down) return;
        keys[k].down = false;
        noteOff(keys[k].note); // the note it started, even if the octave changed meanwhile
        keys[k].note = -1;
}

void draw() {
        w.clear(Color::DarkGray);
        for (int pass = 0; pass < 2; pass++) {
                for (uint i = 0; i < keys.length(); i++) {
                        Key@ k = keys[i];
                        if (k.black != (pass == 1)) continue;
                        uint color = k.black ? Color::Black : Color::White;
                        if (k.down) color = k.black ? Color::Orange : Color::Yellow;
                        w.fillRect(k.x, k.y, k.width, k.height, color);
                        w.text(k.x + k.width / 2 - 4, k.y + k.height - 26, k.letter.toUpper(), k.black ? Color::White : Color::Black);
                }
        }
        w.text(20, 12, "octave " + octave + " (Z X)   " + instrumentNames[chosen] + " (1-4)", Color::White);
        w.frame();
}

instrument(instruments[chosen]);
draw();
while (!w.closed()) {
        Event@ e = w.waitEvent();
        bool changed = true;
        if (e.type == KeyDown && !e.repeat) { // a held key repeats: the note is already sounding
                int k = keyFor(e.key);
                if (k >= 0) {
                        press(k);
                } else if (e.key == "z" && octave > 0) {
                        octave--;
                } else if (e.key == "x" && octave < 8) {
                        octave++;
                } else if (e.text >= "1" && e.text <= "4") {
                        chosen = ord(e.text) - ord("1");
                        instrument(instruments[chosen]);
                } else {
                        changed = false;
                }
        } else if (e.type == KeyUp && keyFor(e.key) >= 0 && keyFor(e.key) != mouseKey) {
                release(keyFor(e.key));
        } else if (e.type == MouseDown && e.button == 1) {
                mouseKey = keyAt(e.x, e.y);
                press(mouseKey);
        } else if (e.type == MouseMove && w.mouseDown(1) && keyAt(e.x, e.y) != mouseKey) {
                // Dragged onto another key: that one plays instead.
                release(mouseKey);
                mouseKey = keyAt(e.x, e.y);
                press(mouseKey);
        } else if (e.type == MouseUp && e.button == 1) {
                release(mouseKey);
                mouseKey = -1;
        } else {
                changed = false;
        }
        if (changed) draw(); // drawn again only when something shown may have changed
}
allNotesOff();
