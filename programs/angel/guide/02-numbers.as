#!/rom/bin/angel
// Lesson 2: Numbers
//
// Computers are good at sums. This lesson is about numbers, and about variables: names that
// hold a value, so the program can keep it and use it later.

println(2 + 3);
println(10 - 4);
println(6 * 7);   // * means "times"
println(20 / 4);  // / means "divided by"

// A variable has a type, a name, and a value. int holds whole numbers.
int apples = 5;
int friends = 2;
println(apples * friends);

// Text and numbers can be joined with +: the number becomes text.
println("I have " + apples + " apples.");

// A variable can change. = puts a new value in it.
apples = apples + 3;
println("Now I have " + apples + " apples.");
apples += 1; // the same as apples = apples + 1
println("And now " + apples + ".");

// Whole numbers divide into whole numbers: 7 / 2 is 3, and % gives what's left over.
println("7 / 2 is " + (7 / 2) + " and 1 left over: 7 % 2 is " + (7 % 2));

// float holds numbers with a decimal point.
float price = 1.25;
float total = price * 3;
println("Three cost " + total);
println("Half of 7 is " + (7.0 / 2));

// Some ready-made maths: sqrt is the square root, PI is 3.14159...
println("The square root of 81 is " + sqrt(81.0));
float radius = 10;
println("A circle " + radius * 2 + " across goes " + 2 * PI * radius + " around.");
println("Rounded: " + round(2 * PI * radius));

// Try this:
//   1. Work out how many seconds there are in a day: 60 * 60 * 24.
//   2. Make a variable for your age, and print how old you'll be in 10 years.
//   3. Print 1 / 3 and then 1.0 / 3. Why are they different?
