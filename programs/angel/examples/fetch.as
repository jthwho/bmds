#!/rom/bin/angel
// Fetches a page from the web and shows it, with what the server said about it:
//     angel fetch.as http://example.com/
// (Only http:// for now; https:// needs encryption the system can't offer scripts yet.)

string url = args.length() > 1 ? args[1] : "http://example.com/";
println("Fetching " + url + "...");
uint start = millis();
HttpResponse@ page = Http::request("GET", url);
uint took = millis() - start;

println("The server said: " + page.status + " " + page.reason);
if (page.url != url) println("(it sent us on to " + page.url + ")");
println("It's " + page.body.length() + " bytes of " + page.header("Content-Type") + ", in " + took + " ms.");
println("");

// HTML is text with <tags>: show the text, roughly. What's inside <style> and <script> isn't
// text, and the tags that start a paragraph, heading or line start a new line.
string html = page.body;
string text = "";
uint i = 0;
while (i < html.length()) {
        int open = html.find("<", i);
        if (open < 0) {
                text += html.substr(i);
                break;
        }
        text += html.substr(i, uint(open) - i);
        int close = html.find(">", uint(open));
        if (close < 0) break;
        string tag = html.substr(uint(open) + 1, uint(close - open) - 1).toLower();
        i = uint(close) + 1;
        if (tag.startsWith("style") || tag.startsWith("script")) {
                int end = html.toLower().find("</" + (tag.startsWith("style") ? "style" : "script"), i);
                if (end < 0) break;
                i = uint(end);
        } else if (tag.startsWith("p") || tag.startsWith("/p") || tag.startsWith("br") || tag.startsWith("h") ||
                   tag.startsWith("/h") || tag.startsWith("div") || tag.startsWith("li") || tag.startsWith("tr")) {
                text += "\n";
        }
}
text = text.replace("&amp;", "&").replace("&lt;", "<").replace("&gt;", ">").replace("&quot;", "\"").replace("&nbsp;", " ");

string[] lines = text.split("\n");
int shown = 0;
for (uint n = 0; n < lines.length() && shown < 20; n++) {
        string line = lines[n].trim();
        if (line == "") continue;
        println(line);
        shown++;
}
