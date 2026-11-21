/**
 * @file      i2cmulti.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * i2c-tools as one program with a link for each (as Dropbear's): libi2c and musl once instead of
 * five times. Each tool's main() is built as TOOL_main; the name it is started as picks it
 * (`i2cmulti TOOL ...` too).
 */

#include <stdio.h>
#include <string.h>

int i2cdetect_main(int argc, char **argv);
int i2cget_main(int argc, char **argv);
int i2cset_main(int argc, char **argv);
int i2cdump_main(int argc, char **argv);
int i2ctransfer_main(int argc, char **argv);

static const struct {
        const char *name;
        int (*run)(int argc, char **argv);
} tools[] = {
        {"i2cdetect", i2cdetect_main}, {"i2cget", i2cget_main},           {"i2cset", i2cset_main},
        {"i2cdump", i2cdump_main},     {"i2ctransfer", i2ctransfer_main},
};

static const char *baseName(const char *path) {
        const char *slash = strrchr(path, '/');
        return slash != NULL ? slash + 1 : path;
}

int main(int argc, char **argv) {
        for (int pass = 0; pass < 2 && argc > 0; pass++) {
                const char *name = baseName(argv[0]);
                for (size_t i = 0; i < sizeof(tools) / sizeof(tools[0]); i++)
                        if (strcmp(name, tools[i].name) == 0) return tools[i].run(argc, argv);
                argc--; /* `i2cmulti TOOL ...`: the tool's name is the next */
                argv++;
        }
        fprintf(stderr, "i2cmulti: run as one of i2cdetect, i2cget, i2cset, i2cdump, i2ctransfer\n");
        return 1;
}
