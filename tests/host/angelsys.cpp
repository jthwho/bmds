/**
 * @file      angelsys.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The host angel's system call table (it runs `--api` and the script tests): the font calls
 * alone, the real font library behind them (fakefonts.cpp), so scripts' text draws as on the
 * board; every other call is missing.
 */

#include <bmds/sys.h>
#include "fakefonts.h"

namespace {

        struct Table {
                        uint32_t      magic, abi_version, count, reserved;
                        bmds_sys_fn_t slot[BMDS_SYS_FONT_CLOSE + 1];
        };

        Table makeTable() {
                Table t = {BMDS_SYS_MAGIC, BMDS_ABI_VERSION, BMDS_SYS_FONT_CLOSE + 1, 0, {}};
                FakeFonts::install(t.slot);
                return t;
        }

        Table table = makeTable();

} // namespace

extern "C" {
const bmds_sys_t *bmds_sys = reinterpret_cast<const bmds_sys_t *>(&table);
}
