/**
 * @file      conf.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The system configuration from a program (bmds/sys.h's sysconf_* calls,
 * devplan/sysconf.md §12):
 *   conf [PREFIX]        list KEY=VALUE under PREFIX (all without one)
 *   conf -w [PREFIX]     list, then print each change as KEY=VALUE until interrupted
 *   conf KEY=VALUE...    set each (for this boot: `sysconf save` keeps it)
 *   conf -d              declare this program's own example/greeting and show it
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <bmds/sys.h>

static char value[BMDS_SYSCONF_VALUE_MAX + 1];
static char key[BMDS_SYSCONF_KEY_MAX + 1];

static int show(int32_t id) {
        if (bmds_sysconf_get(id, BMDS_SYSCONF_FIELD_KEY, key, sizeof(key)) < 0) return 1;
        if (bmds_sysconf_get(id, BMDS_SYSCONF_FIELD_VALUE, value, sizeof(value)) < 0) return 1;
        printf("%s=%s\n", key, value);
        return 0;
}

static int list(const char *prefix) {
        int status = 0;
        for (int32_t id = bmds_sysconf_next(prefix, -1); id >= 0; id = bmds_sysconf_next(prefix, id))
                status |= show(id);
        return status;
}

static int watch(const char *prefix) {
        const int32_t fd = bmds_sysconf_watch(prefix, BMDS_O_CLOEXEC);
        if (fd < 0) {
                fprintf(stderr, "conf: %s: %s\n", prefix, strerror(-fd));
                return 1;
        }
        list(prefix);
        fflush(stdout);
        for (;;) {
                uint32_t      records[16];
                const ssize_t n = read(fd, records, sizeof(records)); // waits for a change
                if (n < 0) {
                        if (errno == EINTR) continue;
                        break;
                }
                for (size_t i = 0; i < (size_t)n / sizeof(uint32_t); ++i) {
                        if (records[i] == BMDS_SYSCONF_OVERFLOW) {
                                list(prefix); // more changed than were kept: all again
                        } else {
                                show((int32_t)records[i]);
                        }
                }
                fflush(stdout);
        }
        close(fd);
        return 0;
}

static int set(const char *assignment) {
        const char *equals = strchr(assignment, '=');
        if (equals == NULL || (size_t)(equals - assignment) > BMDS_SYSCONF_KEY_MAX) {
                fprintf(stderr, "conf: %s: not KEY=VALUE\n", assignment);
                return 1;
        }
        memcpy(key, assignment, (size_t)(equals - assignment));
        key[equals - assignment] = '\0';
        const int32_t id = bmds_sysconf_find(key);
        const int32_t err = id < 0 ? id : bmds_sysconf_set(id, BMDS_SYSCONF_OP_SET, equals + 1);
        if (err < 0) {
                fprintf(stderr, "conf: %s: %s\n", key, strerror(-err));
                return 1;
        }
        return 0;
}

static int declare(void) {
        bmds_sysconf_param_t param = {0};
        param.size = sizeof(param);
        param.type = BMDS_SYSCONF_TYPE_STRING;
        param.key = "example/greeting";
        param.default_value = "hello";
        param.help = "What the SDK's conf example says";
        param.max = 64;
        const int32_t id = bmds_sysconf_declare(&param);
        if (id < 0) {
                fprintf(stderr, "conf: example/greeting: %s\n", strerror(-id));
                return 1;
        }
        return show(id);
}

int main(int argc, char **argv) {
        if (!bmds_sys_has(BMDS_SYS_SYSCONF_WATCH)) {
                fprintf(stderr, "conf: this system has no system configuration calls\n");
                return 1;
        }
        if (argc == 2 && strcmp(argv[1], "-d") == 0) return declare();
        if (argc >= 2 && strcmp(argv[1], "-w") == 0) return argc <= 3 ? watch(argc == 3 ? argv[2] : "") : 2;
        if (argc >= 2 && strchr(argv[1], '=') != NULL) {
                int status = 0;
                for (int i = 1; i < argc; ++i) status |= set(argv[i]);
                return status;
        }
        if (argc > 2 || (argc == 2 && argv[1][0] == '-')) {
                fprintf(stderr, "usage: conf [-w] [PREFIX] | conf KEY=VALUE... | conf -d\n");
                return 2;
        }
        return list(argc == 2 ? argv[1] : "");
}
