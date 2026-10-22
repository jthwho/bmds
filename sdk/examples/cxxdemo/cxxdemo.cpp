/**
 * @file      cxxdemo.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * C++ in a BMDS program (sdk/README.md, "C++"): classes with virtual functions, new and
 * delete, a template, a static constructor and destructor, a static local. Prints what it
 * did; the exit status is 0 if all of it worked.
 */

#include <cstdio>
#include <cstring>
#include <new>
#include <utility>

namespace {

        int constructed = 0;

        // Made before main, destroyed at exit.
        struct Greeter {
                        Greeter() { constructed = 1; }
                        ~Greeter() { std::printf("cxxdemo: static destructor ran\n"); }
        };
        Greeter greeter;

        class Shape {
                public:
                        virtual ~Shape() = default;
                        virtual const char *name() const = 0;
                        virtual int         area() const = 0;
        };

        class Square : public Shape {
                public:
                        explicit Square(int side) : _side(side) {}
                        const char *name() const override { return "square"; }
                        int         area() const override { return _side * _side; }

                private:
                        int _side;
        };

        class Rectangle : public Shape {
                public:
                        Rectangle(int width, int height) : _width(width), _height(height) {}
                        const char *name() const override { return "rectangle"; }
                        int         area() const override { return _width * _height; }

                private:
                        int _width;
                        int _height;
        };

        template <typename T> T largest(const T *values, int count) {
                T best = values[0];
                for (int i = 1; i < count; ++i)
                        if (values[i] > best) best = values[i];
                return best;
        }

        int counter() {
                static int calls = 0; // a static local: a guard
                return ++calls;
        }

} // namespace

int main() {
        int failures = 0;
        if (constructed != 1) {
                std::printf("cxxdemo: the static constructor didn't run\n");
                ++failures;
        }
        Shape *shapes[] = {new Square(3), new Rectangle(2, 5)};
        int    areas[2];
        for (int i = 0; i < 2; ++i) {
                areas[i] = shapes[i]->area();
                std::printf("cxxdemo: a %s of area %d\n", shapes[i]->name(), areas[i]);
        }
        if (largest(areas, 2) != 10) ++failures;
        for (Shape *shape : shapes) delete shape;
        int *many = new (std::nothrow) int[1000];
        if (many == nullptr) ++failures;
        delete[] many;
        int a = 1, b = 2;
        std::swap(a, b);
        if (a != 2 || b != 1) ++failures;
        counter();
        if (counter() != 2) ++failures;
        std::printf("cxxdemo: %s\n", failures == 0 ? "all worked" : "something failed");
        return failures == 0 ? 0 : 1;
}
