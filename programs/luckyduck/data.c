/**
 * @file      data.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Lucky Duck's kept state (devplan/luckyduck.md §2, §7): the settings and the bank, each a file
 * of "name value" lines in the data folder, read once and written whole when something changes.
 */

#include <stdio.h>
#include <string.h>
#include <bmds/app.h>
#include "luckyduck.h"

#define ENTRIES 64

typedef struct {
                char    name[32];
                int64_t value;
} entry_t;

typedef struct {
                char     path[300]; /* "": not kept */
                entry_t  e[ENTRIES];
                uint32_t count;
                int      loaded;
} store_t;

static store_t settings_store, bank_store;

static void load(store_t *s, const char *file) {
        if (s->loaded) return;
        s->loaded = 1;
        char data[256];
        if (bmds_app_data_dir(data, sizeof(data)) != 0) return;
        const int n = snprintf(s->path, sizeof(s->path), "%s/%s", data, file);
        if (n < 0 || (size_t)n >= sizeof(s->path)) {
                s->path[0] = '\0';
                return;
        }
        FILE *f = fopen(s->path, "r");
        if (f == NULL) return;
        char      line[96], name[32];
        long long value;
        while (s->count < ENTRIES && fgets(line, sizeof(line), f) != NULL) {
                if (sscanf(line, "%31s %lld", name, &value) != 2) continue;
                snprintf(s->e[s->count].name, sizeof(s->e[0].name), "%s", name);
                s->e[s->count++].value = value;
        }
        fclose(f);
        return;
}

static void save(const store_t *s) {
        if (s->path[0] == '\0') return;
        FILE *f = fopen(s->path, "w");
        if (f == NULL) return;
        for (uint32_t i = 0; i < s->count; ++i) fprintf(f, "%s %lld\n", s->e[i].name, (long long)s->e[i].value);
        fclose(f);
        return;
}

static entry_t *find(store_t *s, const char *name) {
        for (uint32_t i = 0; i < s->count; ++i)
                if (strcmp(s->e[i].name, name) == 0) return &s->e[i];
        return NULL;
}

static int64_t get(store_t *s, const char *file, const char *name, int64_t fallback) {
        load(s, file);
        const entry_t *e = find(s, name);
        return e != NULL ? e->value : fallback;
}

static void put(store_t *s, const char *file, const char *name, int64_t value) {
        load(s, file);
        entry_t *e = find(s, name);
        if (e == NULL) {
                if (s->count == ENTRIES) return;
                e = &s->e[s->count++];
                snprintf(e->name, sizeof(e->name), "%s", name);
        } else if (e->value == value) {
                return;
        }
        e->value = value;
        save(s);
        return;
}

int64_t setting(const char *name, int64_t fallback) {
        return get(&settings_store, "settings", name, fallback);
}

void set_setting(const char *name, int64_t value) {
        put(&settings_store, "settings", name, value);
        return;
}

/* ---- The bank */

int64_t bank_balance(void) {
        return get(&bank_store, "bank", "balance", BANK_START);
}

void bank_add(int64_t delta) {
        if (delta == 0) return;
        const int64_t now = bank_balance() + delta;
        put(&bank_store, "bank", "balance", now);
        if (now > get(&bank_store, "bank", "highest", BANK_START)) put(&bank_store, "bank", "highest", now);
        return;
}

int bank_ensure(int64_t least) {
        if (bank_balance() >= least) return 0;
        put(&bank_store, "bank", "balance", BANK_START);
        put(&bank_store, "bank", "topups", get(&bank_store, "bank", "topups", 0) + 1);
        return 1;
}

void money(char *out, uint32_t size, int64_t dollars) {
        char      digits[32];
        const int negative = dollars < 0;
        snprintf(digits, sizeof(digits), "%lld", (long long)(negative ? -dollars : dollars));
        const int n = (int)strlen(digits);
        char      grouped[48];
        int       k = 0;
        for (int i = 0; i < n; ++i) {
                if (i > 0 && (n - i) % 3 == 0) grouped[k++] = ',';
                grouped[k++] = digits[i];
        }
        grouped[k] = '\0';
        snprintf(out, size, "%s$%s", negative ? "-" : "", grouped);
        return;
}
