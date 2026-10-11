/**
 * @file      imageview.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The image viewer's layout (devplan/images.md §4b): fit, zoom, pan, the part shown.
 */

#include <doctest.h>

#include "viewlayout.h"

using bmds::ViewLayout;

TEST_CASE("ViewLayout: fitted, centred, never enlarged, on 4-pixel columns") {
        ViewLayout::Input in;
        in.sourceWidth = 1920;
        in.sourceHeight = 1080;
        in.viewWidth = 1000;
        in.viewHeight = 700;
        ViewLayout l = ViewLayout::make(in);
        CHECK(l.width == 1000);
        CHECK(l.height == 563); // 1080 × 1000 / 1920, rounded
        CHECK(l.x == 0);
        CHECK(l.y == (700 - 563) / 2);
        CHECK(l.cropWidth == 1920);
        CHECK(l.cropHeight == 1080);
        CHECK(l.zoom == 520);
        // Tall: the height limits, the width rounded down to a column.
        in.sourceWidth = 1001;
        in.sourceHeight = 2000;
        l = ViewLayout::make(in);
        CHECK(l.width % 4 == 0);
        CHECK(l.height <= 700);
        CHECK(l.x % 4 == 0);
        CHECK(l.x + l.width <= 1000);
        // Small: as it is, centred.
        in.sourceWidth = 64;
        in.sourceHeight = 48;
        l = ViewLayout::make(in);
        CHECK(l.width == 64);
        CHECK(l.height == 48);
        CHECK(l.zoom == 1000);
        CHECK(l.x == (1000 - 64) / 2 / 4 * 4);
        // Nothing to lay out.
        in.sourceWidth = 0;
        CHECK(ViewLayout::make(in).width == 0);
}

TEST_CASE("ViewLayout: zoomed, panned in bounds, the part shown cropped") {
        ViewLayout::Input in;
        in.sourceWidth = 2000;
        in.sourceHeight = 1500;
        in.viewWidth = 1000;
        in.viewHeight = 700;
        in.fit = false;
        in.zoom = 1000; // actual size: the crop is the part shown, pixel for pixel
        in.panX = 503;  // in fours across
        in.panY = 9999; // past the bottom: kept in
        ViewLayout l = ViewLayout::make(in);
        CHECK(l.pans(in));
        CHECK(l.panX == 500);
        CHECK(l.panY == 1500 - 700);
        CHECK(l.shownWidth == 1000);
        CHECK(l.shownHeight == 700);
        CHECK(l.x == 0);
        CHECK(l.cropX == 500);
        CHECK(l.cropY == 800);
        CHECK(l.cropWidth == 1000);
        CHECK(l.cropHeight == 700);
        // 200%: half as much of the picture shows.
        in.zoom = 2000;
        in.panX = 0;
        in.panY = 0;
        l = ViewLayout::make(in);
        CHECK(l.width == 4000);
        CHECK(l.cropWidth == 500);
        CHECK(l.cropHeight == 350);
        // 25%: smaller than the view: centred, no pan.
        in.zoom = 250;
        in.panX = 300;
        l = ViewLayout::make(in);
        CHECK_FALSE(l.pans(in));
        CHECK(l.panX == 0);
        CHECK(l.width == 500);
        CHECK(l.x == 248);
        CHECK(l.cropWidth == 2000);
}

TEST_CASE("ViewLayout: zoom steps") {
        CHECK(ViewLayout::step(1000, true) == 1414);
        CHECK(ViewLayout::step(1000, false) == 707);
        CHECK(ViewLayout::step(520, true) == 707); // from a fit's zoom: the next step
        CHECK(ViewLayout::step(520, false) == 500);
        CHECK(ViewLayout::step(8000, true) == 8000);
        CHECK(ViewLayout::step(63, false) == 63);
}
