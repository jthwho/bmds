// play()'s music language checked, and live notes (on the host nothing sounds); what they refuse.
play("T120 O3 L8 CDEFGAB>C");
play("MB c#4 d-8. e.. N60 N0 P4 R2 ML MS MN MF I0 V64 < > o0 c o6 b ; t255 l64 c");
play("");
tone(440, 100);
beep();
say("hello");
stopMusic();
musicVolume(0.5);
string[] bad = {"H", "O9", "L0", "C65", "P0", "MX", "N85", "N", "T20", "T256", "I128", "V200", "CDE!"};
for (uint i = 0; i < bad.length(); i++) {
        try { play(bad[i]); } catch { println(getExceptionInfo()); }
}
try { Sound s("x.wav"); } catch { println(getExceptionInfo()); }
try { playMusic("x.mid"); } catch { println(getExceptionInfo()); }
noteOn(60);
noteOn(64, 127);
noteOff(60);
instrument(19);
allNotesOff();
try { noteOn(128); } catch { println(getExceptionInfo()); }
try { noteOff(-1); } catch { println(getExceptionInfo()); }
try { instrument(128); } catch { println(getExceptionInfo()); }
