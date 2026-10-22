#!/rom/bin/angel
// Lesson 8: Text
//
// Strings have functions of their own, written after a dot: name.length(). And the text screen
// can be coloured, and written anywhere on it.

string sentence = "the quick brown fox jumps over the lazy dog";
println(sentence.toUpper());
println("It has " + sentence.length() + " letters and spaces.");

// split cuts it into a list of words (lesson 7's arrays).
string[] words = sentence.split(" ");
println("It has " + words.length() + " words. The third is \"" + words[2] + "\".");

// find gives where a piece is (counting from 0), or -1.
println("\"fox\" starts at " + sentence.find("fox") + ".");

// substr takes a piece: from where, how long.
println("The first nine: " + sentence.substr(0, 9));

// replace swaps every piece for another.
println(sentence.replace("dog", "cat"));

// The longest word.
string longest = "";
foreach (string w : words)
        if (w.length() > longest.length()) longest = w;
println("The longest word is \"" + longest + "\".");

// The text screen: Console::color(text, background) sets the colours, Console::at(column, row)
// where the next text goes (from 0). Console::reset() puts things back.
Console::color(Console::Color::Yellow, Console::Color::Blue);
print(" Yellow on blue ");
Console::reset();
println();
for (int c = 9; c <= 14; c++) {
        Console::color(c);
        print("Colour " + c + "  ");
}
Console::reset();
println();

// Try this:
//   1. Ask for a sentence with input() and print it backwards. (Hint: a loop from the end, and
//      sentence.substr(i, 1) is the letter at i.)
//   2. Count the letter "o" in the sentence.
//   3. Print your name in every colour from 1 to 15.
