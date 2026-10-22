#!/rom/bin/angel
// Lesson 7: Lists
//
// An array holds many values of one type, in order: a shopping list, the scores in a game, the
// positions of the aliens. Its type is the values' type with [] after it.

string[] fruit = {"apple", "banana", "plum"};

// The first one is number 0, the second number 1, and so on. length() says how many.
println("The first fruit is " + fruit[0] + ".");
println("There are " + fruit.length() + " fruits.");

// insertLast adds one at the end.
fruit.insertLast("date");
println("Now there are " + fruit.length() + ": " + join(fruit, ", ") + ".");

// foreach goes through them all, one at a time.
foreach (string f : fruit) {
        println("I like " + f + "s.");
}

// A list of numbers: add them up, find the biggest.
int[] scores = {12, 45, 7, 30, 45, 18};
int total = 0;
int best = scores[0];
foreach (int s : scores) {
        total += s;
        if (s > best) best = s;
}
println("Total " + total + ", best " + best + ", average " + float(total) / scores.length() + ".");

// sortDesc puts them from biggest to smallest (sortAsc the other way).
scores.sortDesc();
println("A high score table:");
for (uint i = 0; i < 3; i++) println("  " + (i + 1) + ". " + scores[i]);

// find gives where a value is, or -1 if it isn't there.
println("plum is number " + fruit.find("plum") + "; kiwi is " + fruit.find("kiwi") + ".");

// removeAt takes one out.
fruit.removeAt(1);
println("Without the banana: " + join(fruit, ", ") + ".");

// Try this:
//   1. Make a list of your friends' names and print each with a greeting.
//   2. Ask for numbers with input() until an empty line, put them in an array, then print them
//      sorted.
//   3. Count how many of the scores are more than 20.
