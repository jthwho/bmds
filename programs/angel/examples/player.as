#!/rom/bin/angel
// A music player (audio and speech: devplan/guiprograms.md §14): a file chosen, played in the
// background (an AudioPlayer: WAV, MP3, Ogg Vorbis, FLAC, MIDI), paused, stopped, moved through
// with the slider; its tags shown, and its title read aloud by a voice. A file can be given:
// angel player.as song.mp3.

Window w("Player", 420, 180);
w.minWidth = 320;
w.margin = 8;
w.spacing = 6;

Box fileRow(w, "across");
Button openButton(fileRow, "&Open...");
Label fileName(fileRow, "No file");
fileName.stretch = 1;
Label tags(w, "");
Box timeRow(w, "across");
Slider seek(timeRow);
seek.stretch = 1;
seek.maximum = 1000;
Label clock(timeRow, "0:00 / 0:00");
Box buttons(w, "across");
Button playButton(buttons, "&Play");
Button stopButton(buttons, "&Stop");
Button readButton(buttons, "&Read the title");
Label volumeLabel(buttons, "Volume:");
Slider volume(buttons);
volume.stretch = 1;
volume.maximum = 100;
volume.value = 80;
Spacer gap(w);
gap.stretch = 1; // the status bar at the bottom
StatusBar status(w);
int field = 0;
field = status.addField(0);

AudioPlayer@ player = null;
Voice reader;

string minutes(float seconds) {
        if (seconds < 0) return "?";
        int s = int(seconds);
        string ss = "" + (s % 60);
        return (s / 60) + ":" + (ss.length() < 2 ? "0" + ss : ss);
}

void show() {
        if (player is null) return;
        float at = player.position, length = player.duration;
        clock.text = minutes(at) + " / " + minutes(length);
        if (length > 0) seek.value = int(at / length * 1000);
        playButton.text = player.playing ? "&Pause" : "&Play";
}

void load(const string &in path) {
        if (player !is null) player.stop();
        try {
                @player = AudioPlayer(path);
        } catch {
                @player = null;
                alert(getExceptionInfo());
                return;
        }
        fileName.text = path.substr(path.findLast("/") + 1);
        string title = player.tag("TITLE"), artist = player.tag("ARTIST");
        tags.text = title.length() > 0 ? title + (artist.length() > 0 ? " - " + artist : "") : "(no tags)";
        status.setField(field, player.codec + ", " + player.rate + " Hz, " + (player.channels == 2 ? "stereo" : "mono")
                        + (player.bitrate > 0 ? ", " + (player.bitrate / 1000) + " kb/s" : ""));
        player.volume = volume.value / 100.0;
        player.onFinished(function() {
                seek.value = 0;
                show();
        });
        player.play();
        show();
}

openButton.onClicked(function() {
        string path = chooseFile("Play a sound file");
        if (path.length() > 0) load(path);
});
playButton.onClicked(function() {
        if (player is null) return;
        if (player.playing)
                player.pause();
        else
                player.play();
        show();
});
stopButton.onClicked(function() {
        if (player is null) return;
        player.stop();
        show();
});
seek.onReleased(function(value) {
        if (player is null || player.duration <= 0) return;
        player.position = value / 1000.0 * player.duration;
        show();
});
volume.onChanged(function(value) {
        if (player !is null) player.volume = value / 100.0;
});
readButton.onClicked(function() {
        reader.stop();
        reader.speak(tags.text.length() > 0 && tags.text != "(no tags)" ? "Now playing: " + tags.text
                                                                         : "This file has no title.");
});

app.every(250, show);
w.show();
if (args.length() > 1) load(args[1]);
app.run();
