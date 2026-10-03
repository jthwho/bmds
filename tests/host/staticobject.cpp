/**
 * @file      staticobject.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <doctest.h>

#include <bmds/staticobject.h>

using bmds::StaticObject;

namespace {

        struct Widget {
                        Widget(int a, int b) : sum(a + b) {}
                        int sum;
        };

        // Constant-initialized: no global constructor runs for this.
        constinit StaticObject<Widget> widget;

} // namespace

TEST_CASE("StaticObject") {
        CHECK_FALSE(widget.isCreated());
        Widget &w = widget.create(2, 3);
        CHECK(widget.isCreated());
        CHECK(w.sum == 5);
        CHECK(widget->sum == 5);
        CHECK(&*widget == &w);
}
