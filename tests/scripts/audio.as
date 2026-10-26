// Audio and speech (devplan/guiprograms.md §14, step 6c) on the host: nothing is heard, but a
// voice's texts are told said in the loop, in order; there are no voices, ports or players here.

Voice v;
println("voice '" + v.name + "' " + v.speed + " " + v.pitch + " " + v.volume);
v.speed = 150;
v.pitch = 80;
try {
        v.speed = 10;
} catch {
        println(getExceptionInfo());
}
string told = "";
v.onSaid(function(text) {
        told += "[" + text + "]";
        if (text == "three") app.quit();
});
v.speak("one");
v.speak("two");
v.speak("three");
app.after(3000, function() { app.quit(); });
app.run();
println("said: " + told + " speaking " + v.speaking);
v.say("waited"); // not told: waited for
println("after say: " + told);
v.onSaid(null);
println("voices " + voices().length() + ", ports " + midiPorts().length());
println("mixer " + mixer.volume + " " + mixer.muted);
try {
        mixer.volume = -10;
} catch {
        println(getExceptionInfo());
}
try {
        AudioPlayer p("music.mp3");
} catch {
        println(getExceptionInfo());
}
try {
        MidiPort m("wavetable");
} catch {
        println(getExceptionInfo());
}
