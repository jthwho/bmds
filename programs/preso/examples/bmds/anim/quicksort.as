// Quicksort (Lomuto's partition), each comparison and swap shown as it happens.
float[] v = {7, 3, 9, 1, 6, 2, 8, 4, 5};
SceneBars@ bars = SceneBars(v, scene.width - 40, scene.height - 20).at(scene.width / 2, scene.height / 2);
scene.add(bars);
scene.pause();
sort(0, int(v.length()) - 1);

void swap(int i, int j) {
        if (i == j) return;
        float t = v[i];
        v[i] = v[j];
        v[j] = t;
        scene.play(bars.swap(i, j), 0.25);
}

void sort(int lo, int hi) {
        if (lo >= hi) return;
        scene.play(bars.highlight(hi), 0.15);
        int i = lo;
        for (int j = lo; j < hi; j++) {
                if (v[j] < v[hi]) {
                        swap(i, j);
                        i++;
                }
        }
        swap(i, hi);
        scene.play(bars.unhighlight(i), 0.15);
        sort(lo, i - 1);
        sort(i + 1, hi);
}
