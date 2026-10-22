#!/rom/bin/angel
// Lesson 3: Asking
//
// A program can ask a question and use the answer. input() waits for you to type a line in the
// Output and press Enter, and gives back what you typed, as a string.

string name = input("What's your name? ");
if (name == "") name = "friend"; // nothing typed: a name anyway
println("Hello, " + name + "!");

// What you type is text. parseInt turns it into a number, so we can do sums with it.
string answer = input("How old are you? ");
int age = int(parseInt(answer));
println("Next year you'll be " + (age + 1) + ".");

// if runs the lines in its { } only when what's in its brackets is true.
// else runs the other lines when it isn't.
if (age >= 13) {
        println("You're a teenager, or older.");
} else {
        println("You're not a teenager yet.");
}

// Comparisons: == equal, != not equal, < less, > more, <= less or equal, >= more or equal.
// && means "and", || means "or".
string pet = input("Do you have a pet (yes or no)? ");
if (pet == "yes" || pet == "y") {
        println("Lucky you!");
} else if (pet == "no" || pet == "n") {
        println("Maybe one day.");
} else {
        println("I'll take that as a maybe.");
}

// Try this:
//   1. Ask for a favourite number and print double it.
//   2. Say something special if the name typed is yours.
//   3. Ask two numbers and say which is bigger, or that they're the same.
