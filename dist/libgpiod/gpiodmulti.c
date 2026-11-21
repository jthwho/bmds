/**
 * @file      gpiodmulti.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libgpiod's tools as one program with a link for each (as Dropbear's): the library and musl
 * once instead of six times. Each tool's main() is built as TOOL_main; the name it is started as
 * picks it (`gpiodmulti TOOL ...` too).
 */

#include <stdio.h>
#include <string.h>

int gpiodetect_main(int argc, char **argv);
int gpioinfo_main(int argc, char **argv);
int gpioget_main(int argc, char **argv);
int gpioset_main(int argc, char **argv);
int gpiomon_main(int argc, char **argv);
int gpionotify_main(int argc, char **argv);

static const struct {
        const char *name;
        int (*run)(int argc, char **argv);
} tools[] = {
        {"gpiodetect", gpiodetect_main}, {"gpioinfo", gpioinfo_main}, {"gpioget", gpioget_main},
        {"gpioset", gpioset_main},       {"gpiomon", gpiomon_main},   {"gpionotify", gpionotify_main},
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
                argc--; /* `gpiodmulti TOOL ...`: the tool's name is the next */
                argv++;
        }
        fprintf(stderr, "gpiodmulti: run as one of gpiodetect, gpioinfo, gpioget, gpioset, gpiomon, gpionotify\n");
        return 1;
}
