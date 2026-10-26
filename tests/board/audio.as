// Audio and speech on the board (devplan/guiprograms.md §14, step 6c): a WAV the test makes (a
// second of 440 Hz) played, paused, sought, waited for, looped, its end called back; a Vorbis
// file's tags if the card has /sd0/p0/t.ogg; a voice speaking in the loop, each text told; the
// MIDI ports (a note to the wavetable synth; what programs play heard on `system`); the mixer
// read, changed and put back. Run with `angel tests/board/audio.as`; it prints "audio.as: ok".

int failures = 0;
void check(bool ok, const string &in what) {
        if (ok) return;
        println("audio.as: failed: " + what);
        failures++;
}
bool near(float a, float b, float within) {
        return a >= b - within && a <= b + within;
}

// ---- A WAV made here: 16 kHz, mono, 16-bit, 1 s of 440 Hz at a quarter of full scale
string le(uint v, int bytes) {
        string s = "";
        for (int i = 0; i < bytes; i++) {
                s += " ";
                s[i] = uint8((v >> (8 * i)) & 255);
        }
        return s;
}
const int RATE = 16000;
string samples = "";
for (int i = 0; i < RATE; i++) {
        int v = int(8192 * sin(2 * 3.14159265 * 440 * i / RATE));
        samples += le(uint(v) & 0xffff, 2);
}
string wav = "RIFF" + le(36 + samples.length(), 4) + "WAVEfmt " + le(16, 4) + le(1, 2) + le(1, 2) + le(RATE, 4)
             + le(RATE * 2, 4) + le(2, 2) + le(16, 2) + "data" + le(samples.length(), 4) + samples;
string path = "/tmp/angel-tone.wav";
writeFile(path, wav);

AudioPlayer p(path);
check(p.codec == "wav" && p.rate == RATE && p.channels == 1, "its format: " + p.codec + " " + p.rate);
check(near(p.duration, 1, 0.01), "its duration: " + p.duration);
check(!p.playing && p.position == 0, "not playing yet");
p.volume = 0.3;
p.play();
sleep(400);
check(p.playing, "playing");
float at = p.position;
check(at > 0.1 && at < 0.6, "its position playing: " + at);
p.pause();
at = p.position;
sleep(300);
check(!p.playing && near(p.position, at, 0.02), "paused: " + at + " then " + p.position);
p.play();
p.position = 0.5;
check(near(p.position, 0.5, 0.05), "sought: " + p.position);
p.wait();
check(!p.playing && near(p.position, 1, 0.01), "waited for: " + p.position);

// Its end called back in the loop.
bool finished = false;
p.onFinished(function() {
        finished = true;
        app.quit();
});
p.play(); // from the start again
int guard = app.after(4000, function() { app.quit(); });
app.run();
app.stop(guard); // not left to end a later run
check(finished, "its end called back");
p.onFinished(null);

// Looped: past its length it plays on; wait() would never end; stop() goes back to the start.
p.loop = true;
p.play();
sleep(1400);
check(p.playing && p.position < 1, "looped: " + p.position);
bool raised = false;
try {
        p.wait();
} catch {
        raised = true;
}
check(raised, "waiting for a loop raises");
p.stop();
check(!p.playing && p.position == 0, "stopped");
p.loop = false;

// Let go of while playing: still heard to its end (kept meanwhile).
AudioPlayer(path).play();

raised = false;
try {
        writeFile("/tmp/angel-not.wav", "not a sound");
        AudioPlayer bad("/tmp/angel-not.wav");
} catch {
        raised = true;
}
check(raised, "not a sound file: raises");
removeFile("/tmp/angel-not.wav");

if (fileExists("/sd0/p0/t.ogg")) {
        AudioPlayer ogg("/sd0/p0/t.ogg");
        string[] names = ogg.tagNames();
        string all = "";
        for (uint i = 0; i < names.length(); i++) all += " " + names[i] + "=" + ogg.tag(names[i]);
        println("t.ogg: " + ogg.codec + " " + ogg.rate + " Hz " + ogg.channels + " ch " + ogg.bitrate + " b/s "
                + formatFloat(ogg.duration, 1) + " s;" + all);
        check(ogg.codec == "vorbis" && ogg.duration > 0, "a Vorbis file");
        check(names.length() == 0 || ogg.tag(names[0].toLower()) == ogg.tag(names[0]), "tags in any case");
}

// ---- Speech
string[] vs = voices();
check(vs.find("en-US") >= 0, "the voices: " + vs.length());
raised = false;
try {
        Voice none("xx-XX");
} catch {
        raised = true;
}
check(raised, "a voice not there raises");
Voice v;
v.speed = 130;
string told = "";
v.onSaid(function(text) {
        told += "[" + text + "]";
        if (text == "Two.") app.quit();
});
v.speak("One.");
v.speak("Two.");
check(v.speaking, "speaking in the background");
guard = app.after(15000, function() { app.quit(); });
app.run();
app.stop(guard);
check(told == "[One.][Two.]", "each text told once heard: " + told);
v.onSaid(null);
v.say("Three.");
check(!v.speaking, "said, waited for");
v.speak("This is cut short, as stop is called at once.");
v.stop();
check(!v.speaking, "stopped");

// ---- MIDI
string[] ports = midiPorts();
check(ports.find("wavetable") >= 0 && ports.find("system") >= 0, "the ports: " + ports.length());
MidiPort synth("wavetable");
check(synth.name == "wavetable" && synth.number >= 0, "a port by name");
synth.program(1, 0);
synth.noteOn(1, 72, 80);
sleep(300);
synth.noteOff(1, 72);
MidiPort system1("system");
string heard = "";
system1.onMessage(function(status, data1, data2) {
        heard += " " + status + "," + data1 + "," + data2;
        if (status == 0x80 || (status == 0x90 && data2 == 0)) app.quit();
});
app.after(10, function() {
        noteOn(64, 90); // angel's own notes go out through `system`
        sleep(200);
        noteOff(64);
});
guard = app.after(3000, function() { app.quit(); });
app.run();
app.stop(guard);
check(heard.contains(",64,90"), "what programs play heard on system:" + heard);
system1.close();
raised = false;
try {
        MidiPort nowhere("no-such-port");
} catch {
        raised = true;
}
check(raised, "a port not there raises");
raised = false;
try {
        synth.noteOn(17, 60);
} catch {
        raised = true;
}
check(raised, "channel 17 raises");

// ---- The mixer: changed and put back (not saved)
int volume = mixer.volume;
mixer.volume = volume > -60 ? volume - 1 : volume + 1;
check(mixer.volume != volume, "the volume changed");
mixer.volume = volume;
check(mixer.volume == volume, "and put back: " + volume);

removeFile(path);
if (failures == 0) println("audio.as: ok");
