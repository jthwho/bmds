// BMDS's layers, from the hardware up: each Next adds one.
string[] names = {
        "ESP32-P4 hardware",
        "ESP-IDF: FreeRTOS, startup, HAL",
        "Drivers: SD, USB, HDMI, audio, Ethernet",
        "Kernel services: files, input, display, sound",
        "Shell, program loader, consoles",
        "System calls: a C table, append-only",
        "Programs from the SD card",
};
float h = region.height / names.length();
for (uint i = 0; i < names.length(); i++) {
        float y = region.height - h * (i + 0.5);
        Rect@ r = Rect(region.width - 8, h - 4).at(region.width / 2, y);
        r.color = theme.color("Script.Line");
        r.fill = i + 1 == names.length() ? theme.color("Script.Highlight") : theme.color("Script.Fill");
        r.fillOpacity = 1;
        Text@ t = Text(names[i]).at(region.width / 2, y);
        if (i > 0) pause();
        add(r);
        add(t);
        play(fadeIn(r), fadeIn(t), 0.3);
}
