#!/rom/bin/angel
// Chat with a friend on another computer. One of you waits:
//     angel chat.as
// and it shows the address to give your friend, who joins with it:
//     angel chat.as 192.168.1.20
// Type a line and press Enter to send it; Escape (or Ctrl+C) leaves.

const int port = 5000;
string name = input("Your name? ");
if (name == "") name = "someone";

Socket@ friend;
if (args.length() < 2) {
        ServerSocket waiting(port);
        string here = localAddress();
        println("Waiting for a friend: tell them to run  angel chat.as " + (here == "" ? "<this computer's address>" : here));
        @friend = waiting.accept();
        waiting.close(); // just the one friend
} else {
        println("Calling " + args[1] + "...");
        @friend = Socket(args[1], port);
}
friend.writeLine(name);
string friendName = friend.readLine();
println("You're talking to " + friendName + ". Escape leaves.");

string typed = "";
Console::color(Console::Color::LightGreen);
print("> ");
while (true) {
        // What they sent: shown above the line being typed.
        if (friend.ready(50)) {
                if (friend.eof()) {
                        print("\r\x1b[K");
                        println(friendName + " left.");
                        break;
                }
                string line = friend.readLine();
                print("\r\x1b[K");
                Console::color(Console::Color::Yellow);
                println(friendName + ": " + line);
                Console::color(Console::Color::LightGreen);
                print("> " + typed);
        }
        // What you type.
        string key = Console::keyNow();
        if (key == "") continue;
        if (key == "Escape" || key == "Ctrl+C") {
                println("");
                break;
        } else if (key == "Enter") {
                if (typed != "") friend.writeLine(typed);
                print("\r\x1b[K> ");
                if (typed != "") {
                        Console::color(Console::Color::White);
                        print("\r" + name + ": " + typed + "\n");
                        Console::color(Console::Color::LightGreen);
                        print("> ");
                }
                typed = "";
        } else if (key == "Backspace") {
                if (typed.length() > 0) {
                        // The last character, all its bytes (an é is two).
                        uint at = typed.length() - 1;
                        while (at > 0 && (typed[at] & 0xc0) == 0x80) at--;
                        typed.erase(at, typed.length() - at);
                        print("\x08 \x08"); // back, blank it, back
                }
        } else if (key.length() == 1 || ord(key) >= 0x80) {
                typed += key;
                print(key);
        }
}
Console::reset();
friend.close();
