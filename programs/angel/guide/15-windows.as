#!/rom/bin/angel
// Lesson 15: Windows and buttons
//
// The programs so far ran from top to bottom: print, ask, print again. A program with a window
// works another way. It builds the window, shows it, and then waits. When you click a button or
// type in a box, a function you gave it is called. That's called an event, and the function a
// callback.
//
// Here: a window with a box to type your name in, a button that says hello, and a counter.

// A Window: its title, then its width and height in pixels. margin and spacing are the gaps
// around its edge and between the things in it.
Window w("Hello window", 320, 160);
w.margin = 10;
w.spacing = 6;

// What goes in a window is made with the window (or a box in it) as its first value: that's
// where it goes. They're laid out from top to bottom in the order they're made.
Label greeting(w, "What's your name?");
LineEdit nameBox(w); // a box to type a line in

// A Box puts what's in it side by side ("across") instead of one under another.
Box buttons(w, "across");
Button helloButton(buttons, "Say &hello"); // & makes Alt+H press it
Button countButton(buttons, "Count");
Label countLabel(buttons, "0");

// The callbacks. This one is an ordinary function: it changes the label's text.
void sayHello() {
        string name = nameBox.text.trim();
        if (name.length() == 0) {
                greeting.text = "Hello, whoever you are!";
        } else {
                greeting.text = "Hello, " + name + "!";
        }
}

// onClicked gives the button its callback. Note there are no brackets after sayHello: we
// don't call it now, we hand it over to be called at each click.
helloButton.onClicked(sayHello);

// Pressing Enter in the box calls its onAccepted callback with the text in the box.
nameBox.onAccepted(function(text) { sayHello(); });

// A callback can also be written in place: function() { ... } is a function with no name. It
// can use the program's variables, such as count.
int count = 0;
countButton.onClicked(function() {
        count++;
        countLabel.text = "" + count;
});

// Show the window, put the cursor in the box, and wait for events. app.run() returns when the
// window is closed; the program then goes on (and here, ends).
w.show();
nameBox.setFocus();
app.run();
println("The window was closed. You counted to " + count + ".");

// Try this:
//   1. Add a "Reset" button that puts the count back to 0.
//   2. Make the hello button also clear the name box (nameBox.text = "").
//   3. Add a CheckBox(w, "Shout") and, when it's checked (its checked property), greet in
//      capitals with toUpper().
