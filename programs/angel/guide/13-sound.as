#!/rom/bin/angel
// Lesson 13: Sound
//
// The computer has a synthesizer: it can beep, play notes and tunes, and even talk.

println("A beep.");
beep();
sleep(500);

// tone(frequency, milliseconds): 440 is the A above middle C. Higher numbers, higher notes.
println("Going up...");
for (int f = 300; f <= 900; f += 100) tone(f, 120);
sleep(300);

// play() reads music written as letters, as QBasic's PLAY did: C D E F G A B are the notes,
// O sets the octave, L the length (4 a quarter note, 8 an eighth), T the speed, and P a rest.
println("A tune.");
play("T160 O4 L8 C C G G A A L4 G L8 F F E E D D L4 C");
sleep(300);

// noteOn and noteOff hold notes, as keys on a keyboard: here three at once, a chord.
// The numbers are MIDI notes: 60 is middle C, and each one up is a semitone higher.
println("A chord.");
instrument(0); // a piano (try 40 for a violin, 56 a trumpet)
noteOn(60);
noteOn(64);
noteOn(67);
sleep(1200);
allNotesOff();

// say() speaks.
println("And it talks.");
say("Hello! I can talk.");

// Try this:
//   1. Play the start of a tune you know with play().
//   2. Make a siren: tone() going up and down in a loop.
//   3. Ask for a sentence with input() and say() it.
