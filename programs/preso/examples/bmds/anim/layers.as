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
float h = scene.height / names.length();
for (uint i = 0; i < names.length(); i++) {
        float y = scene.height - h * (i + 0.5);
        SceneRect@ r = SceneRect(scene.width - 8, h - 4).at(scene.width / 2, y);
        r.color = scene.theme.color("Script.Line");
        r.fill = i + 1 == names.length() ? scene.theme.color("Script.Highlight") : scene.theme.color("Script.Fill");
        r.fillOpacity = 1;
        SceneText@ t = SceneText(names[i]).at(scene.width / 2, y);
        if (i > 0) scene.pause();
        scene.add(r);
        scene.add(t);
        scene.play(r.fadeIn(), t.fadeIn(), 0.3);
}
