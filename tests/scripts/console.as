// The text screen's sequences (stdout isn't a terminal here, so they show as text).
Console::clear();
Console::at(4, 2);
Console::color(Console::Yellow, Console::Blue);
Console::color(Console::Red);
Console::cursor(false);
Console::reset();
Console::cursor(true);
println(Console::LightGreen);
println(int(Console::White));
println(Console::width() > 0);
