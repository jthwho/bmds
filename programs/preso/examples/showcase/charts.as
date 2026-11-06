// Charts from values and labels: a bar chart and a pie built, their values changed, then a line
// chart drawn.
float w = scene.width, h = scene.height;
SceneBarChart@ bars = SceneBarChart({12, 30, 22, 41, 18}, {"Mon", "Tue", "Wed", "Thu", "Fri"}, 420, 230)
                              .at(w * 0.28, h * 0.48);
scene.add(bars);
scene.play(bars.build(), 1);
scene.pause();
ScenePieChart@ pie = ScenePieChart({45, 25, 20, 10}, {"Code", "Docs", "Tests", "Talks"}, 110).at(w * 0.76, h * 0.46);
scene.add(pie);
scene.play(pie.build(), 1);
scene.pause();
scene.play(bars.setValue(1, 15), bars.setValue(4, 35), pie.setValue(3, 30), 1);
scene.pause();
scene.play(bars.fadeOut(), pie.fadeOut(), 0.5);
SceneLineChart@ trend = SceneLineChart({3, 7, 5, 11, 9, 14}, {"Jan", "Feb", "Mar", "Apr", "May", "Jun"}, 640, 260)
                                .at(w / 2, h * 0.48);
scene.add(trend);
scene.play(trend.build(), 1.5);
scene.pause();
scene.play(trend.setValue(2, 12), 0.8);
