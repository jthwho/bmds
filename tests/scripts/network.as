// TCP and UDP over loopback: a server and its client in one script.
ServerSocket server(0);
println(server.port() > 0);
println(server.ready());
Socket client("127.0.0.1", server.port());
println(server.ready(1000));
Socket@ s = server.accept();
println(s.address() + " " + (s.port() == 0 ? "no port" : "a port"));
client.writeLine("hello");
client.write("two\r\nthree ");
client.writeLine(42);
println(s.readLine());
println(s.readLine());
println(s.readLine());
println(s.ready());
client.write("abcdef");
println(s.ready(1000));
println(s.read(3));
println(s.read());
s.writeLine("back");
println(client.readLine());
s.setTimeout(50);
try {
        s.readLine();
} catch {
        println(getExceptionInfo());
}
client.writeLine("last");
client.close();
println(client.isOpen());
println(s.readAll());
println(s.eof());
println(s.read() == "");
s.close();
try {
        s.write("x");
} catch {
        println(getExceptionInfo());
}

// Datagrams.
UdpSocket a;
UdpSocket b;
println(b.ready());
a.sendTo("127.0.0.1", b.port(), "ping");
println(b.receive() + " " + b.sender() + " " + (b.senderPort() == a.port()));
b.sendTo(b.sender(), b.senderPort(), "pong");
println(a.receive());
a.setTimeout(50);
try {
        a.receive();
} catch {
        println(getExceptionInfo());
}

// What fails.
int closedPort = server.port();
server.close();
try {
        Socket refused("127.0.0.1", closedPort);
} catch {
        println(getExceptionInfo().startsWith("Can't connect to 127.0.0.1 port"));
}
try {
        ServerSocket bad(70000);
} catch {
        println(getExceptionInfo());
}
try {
        server.accept();
} catch {
        println(getExceptionInfo());
}
println(lookup("127.0.0.1"));
println(localAddress() != "127.0.0.1");
