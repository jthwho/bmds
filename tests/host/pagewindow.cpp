/**
 * @file      pagewindow.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <doctest.h>

#include <cstdlib>
#include <cstring>
#include <vector>
#include <bmds/pagewindow.h>

using bmds::PageWindow;
namespace PageRuns = bmds::PageRuns;

namespace {

        // A heap of whole pages in a row, some in use: runs come from its free stretches, as
        // TLSF's aligned blocks would (a page lost in front of each for its header).
        class FakeHeap final : public PageRuns::Source {
                public:
                        explicit FakeHeap(uint32_t pages) : used(pages, false), base(64 * 1024 * size_t(pages)) {}

                        // Page @p p taken by someone else (the heap cut up).
                        void occupy(uint32_t p) { used[p] = true; }

                        size_t largestFree() override {
                                uint32_t best = 0;
                                uint32_t run = 0;
                                for (bool u : used) {
                                        run = u ? 0 : run + 1;
                                        if (run > best) best = run;
                                }
                                return size_t(best) * Page;
                        }

                        uint8_t *allocRun(uint32_t pages) override {
                                ++calls;
                                // Its header in the page before, its line in the page after: k + 2.
                                uint32_t run = 0;
                                for (uint32_t p = 0; p < used.size(); ++p) {
                                        run = used[p] ? 0 : run + 1;
                                        if (run == pages + 2) {
                                                const uint32_t first = p + 1 - pages - 1;
                                                for (uint32_t q = first - 1; q <= p; ++q) used[q] = true;
                                                runs.push_back({first, pages});
                                                return base.data() + size_t(first) * Page;
                                        }
                                }
                                return nullptr;
                        }

                        void freeRun(uint8_t *at) override {
                                const uint32_t first = uint32_t((at - base.data()) / Page);
                                for (size_t i = 0; i < runs.size(); ++i) {
                                        if (runs[i].first != first) continue;
                                        for (uint32_t q = first - 1; q <= first + runs[i].pages; ++q) used[q] = false;
                                        runs.erase(runs.begin() + long(i));
                                        return;
                                }
                                FAIL("a run freed twice, or never given");
                        }

                        struct Given {
                                        uint32_t first;
                                        uint32_t pages;
                        };

                        static constexpr size_t Page = 64 * 1024;
                        std::vector<bool>       used;
                        std::vector<Given>      runs;
                        std::vector<uint8_t>    base;
                        int                     calls = 0;
        };

        uint32_t total(const PageRuns::Run *runs, size_t n) {
                uint32_t pages = 0;
                for (size_t i = 0; i < n; ++i) pages += runs[i].pages;
                return pages;
        }

} // namespace

TEST_CASE("PageWindow") {
        PageWindow w;
        w.reset(64);

        SUBCASE("first fit from the bottom, last fit from the top") {
                CHECK(w.take(4) == 0);
                CHECK(w.take(4) == 4);
                CHECK(w.take(8, true) == 56);
                CHECK(w.take(1, true) == 55);
                CHECK(w.usedPages() == 17);
                CHECK(w.longestFree() == 64 - 17);
        }

        SUBCASE("a freed range is reused, and the longest range follows") {
                const uint32_t a = w.take(10);
                const uint32_t b = w.take(10);
                const uint32_t c = w.take(10);
                CHECK(w.longestFree() == 34);
                w.give(b, 10);
                CHECK(w.longestFree() == 34);
                CHECK(w.take(10) == b);
                w.give(a, 10);
                w.give(b, 10);
                w.give(c, 10);
                CHECK(w.usedPages() == 0);
                CHECK(w.longestFree() == 64);
        }

        SUBCASE("too long a range is refused") {
                CHECK(w.take(65) == PageWindow::None);
                CHECK(w.take(0) == PageWindow::None);
                (void)w.take(30);
                (void)w.take(4, true);
                CHECK(w.take(31) == PageWindow::None);
                CHECK(w.take(30) == 30);
        }

        SUBCASE("growing in place") {
                const uint32_t a = w.take(4);
                CHECK(w.takeAt(a + 4, 4));
                const uint32_t b = w.take(4);
                CHECK(b == 8);
                CHECK_FALSE(w.takeAt(a + 7, 2)); // b's first page
                CHECK_FALSE(w.takeAt(62, 4));    // past the end
                CHECK(w.usedPages() == 12);
        }

        SUBCASE("odd sizes and the whole window") {
                PageWindow odd;
                odd.reset(37);
                CHECK(odd.longestFree() == 37);
                CHECK(odd.take(37) == 0);
                CHECK(odd.longestFree() == 0);
                odd.give(5, 3);
                CHECK(odd.longestFree() == 3);
                CHECK(odd.take(3, true) == 5);
        }
}

TEST_CASE("PageRuns::gather") {
        PageRuns::Run runs[PageRuns::MaxRuns];

        SUBCASE("one run from an empty heap") {
                FakeHeap     heap(64);
                const size_t n = PageRuns::gather(heap, 20, FakeHeap::Page, bmds::Span<PageRuns::Run>(runs));
                CHECK(n == 1);
                CHECK(runs[0].pages == 20);
                PageRuns::release(heap, bmds::Span<const PageRuns::Run>(runs, n));
                CHECK(heap.runs.empty());
        }

        SUBCASE("a cut-up heap gives its stretches, the largest first") {
                FakeHeap heap(64);
                // Free stretches of 15, 9 and 6 pages between pages in use.
                heap.occupy(15);
                heap.occupy(25);
                heap.occupy(32);
                for (uint32_t p = 33; p < 64; ++p) heap.occupy(p);
                const size_t n = PageRuns::gather(heap, 20, FakeHeap::Page, bmds::Span<PageRuns::Run>(runs));
                REQUIRE(n >= 2);
                CHECK(total(runs, n) == 20);
                CHECK(runs[0].pages >= runs[1].pages);
                PageRuns::release(heap, bmds::Span<const PageRuns::Run>(runs, n));
                CHECK(heap.runs.empty());
        }

        SUBCASE("not enough: nothing kept") {
                FakeHeap     heap(16);
                const size_t n = PageRuns::gather(heap, 20, FakeHeap::Page, bmds::Span<PageRuns::Run>(runs));
                CHECK(n == 0);
                CHECK(heap.runs.empty());
        }

        SUBCASE("too many runs: nothing kept") {
                FakeHeap heap(200);
                // Stretches of 2 free pages (one usable run of 0: none) and of 3 (a run of 1).
                for (uint32_t p = 0; p < 200; p += 4) heap.occupy(p);
                PageRuns::Run few[4];
                const size_t  n = PageRuns::gather(heap, 10, FakeHeap::Page, bmds::Span<PageRuns::Run>(few));
                CHECK(n == 0);
                CHECK(heap.runs.empty());
        }

        SUBCASE("a soak: gathered and released at random, the heap whole at the end") {
                FakeHeap heap(256);
                for (uint32_t p = 7; p < 256; p += 23) heap.occupy(p);
                const std::vector<bool>                 start = heap.used;
                std::vector<std::vector<PageRuns::Run>> held;
                std::srand(1);
                for (int i = 0; i < 2000; ++i) {
                        if (held.empty() || std::rand() % 3 != 0) {
                                const uint32_t want = 1 + uint32_t(std::rand() % 24);
                                const size_t   n =
                                        PageRuns::gather(heap, want, FakeHeap::Page, bmds::Span<PageRuns::Run>(runs));
                                if (n != 0) {
                                        CHECK(total(runs, n) == want);
                                        held.emplace_back(runs, runs + n);
                                }
                        } else {
                                const size_t k = size_t(std::rand()) % held.size();
                                PageRuns::release(heap,
                                                  bmds::Span<const PageRuns::Run>(held[k].data(), held[k].size()));
                                held.erase(held.begin() + long(k));
                        }
                }
                for (auto &h : held) PageRuns::release(heap, bmds::Span<const PageRuns::Run>(h.data(), h.size()));
                CHECK(heap.runs.empty());
                CHECK(heap.used == start);
        }
}
