/**
 * @file      main.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * bmds-mesh3d: OBJ files and Bézier patches into a `.bm3d` model (devplan/3d.md §12d), on the
 * PC. Built with the host's compiler from these files and libbmds3d's math.c and mesh.c:
 *
 *     cc -O2 -I sdk/include -o bmds-mesh3d sdk/tools/mesh3d/[a-z]*.c sdk/3d/math.c sdk/3d/mesh.c -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <bmds/sys.h>
#include "mesh3d.h"

/* libbmds3d's memory calls, on the host: the only system calls mesh.c makes. */
static void *mem_alloc(uint32_t size, uint32_t align, uint32_t flags) {
        (void)flags;
        if (align < 16) align = 16;
        void *p = NULL;
        return posix_memalign(&p, align, size != 0 ? size : 1) == 0 ? p : NULL;
}

static void mem_free(void *ptr) {
        free(ptr);
        return;
}

static struct {
                uint32_t      magic, abi_version, count, reserved;
                bmds_sys_fn_t slot[BMDS_SYS_MEM_FREE + 1];
} table = {BMDS_SYS_MAGIC, BMDS_ABI_VERSION, BMDS_SYS_MEM_FREE + 1, 0, {0}};

const bmds_sys_t *bmds_sys = (const bmds_sys_t *)(const void *)&table;

static void usage(FILE *f) {
        fprintf(f, "usage: bmds-mesh3d [options] -o OUT.bm3d INPUT...\n"
                   "  INPUT: a Wavefront .obj (each o or g a mesh) or Bezier patches (.bpt)\n"
                   "  --detail N     patches: an N x N grid each (default 4)\n"
                   "  --crease DEG   smooth across faces within DEG degrees (default 60; 0: faceted)\n"
                   "  --merge        obj: one mesh an input, not one a group\n"
                   "  --name NAME    the next input's mesh name (default: the file's name)\n"
                   "  --up z         the input is Z-up: turned to Y-up\n"
                   "  --fit          centred, radius 1 (all meshes together)\n"
                   "  --scale S      positions times S (after --fit)\n"
                   "  --no-normals, --no-uvs, --no-edges   leave them out\n"
                   "  -v             print what was written\n");
        return;
}

static char *read_file(const char *path, size_t *length) {
        FILE *f = fopen(path, "rb");
        if (f == NULL) return NULL;
        char  *data = NULL;
        size_t size = 0, cap = 0;
        for (;;) {
                if (size == cap) {
                        cap = cap != 0 ? 2 * cap : 65536;
                        char *d = (char *)realloc(data, cap);
                        if (d == NULL) {
                                free(data);
                                fclose(f);
                                return NULL;
                        }
                        data = d;
                }
                const size_t n = fread(data + size, 1, cap - size, f);
                if (n == 0) break;
                size += n;
        }
        const int bad = ferror(f);
        fclose(f);
        if (bad) {
                free(data);
                return NULL;
        }
        *length = size;
        return data;
}

/* The file's name without its folder or extension. */
static char *stem(const char *path) {
        const char *base = strrchr(path, '/');
        base = base != NULL ? base + 1 : path;
        char *s = strdup(base);
        if (s == NULL) return NULL;
        char *dot = strrchr(s, '.');
        if (dot != NULL && dot != s) *dot = '\0';
        return s;
}

static int ends_with(const char *s, const char *suffix) {
        const size_t a = strlen(s), b = strlen(suffix);
        return a >= b && strcasecmp(s + a - b, suffix) == 0;
}

int main(int argc, char **argv) {
        table.slot[BMDS_SYS_MEM_ALLOC] = (bmds_sys_fn_t)mem_alloc;
        table.slot[BMDS_SYS_MEM_FREE] = (bmds_sys_fn_t)mem_free;

        m3d_options_t options;
        m3d_options_init(&options);
        m3d_scene_t scene = {NULL, 0};
        const char *output = NULL, *name = NULL;
        int         verbose = 0, inputs = 0;
        char        err[256] = "";

        for (int i = 1; i < argc; ++i) {
                const char *a = argv[i];
                const char *value = i + 1 < argc ? argv[i + 1] : NULL;
                if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
                        usage(stdout);
                        return 0;
                } else if (strcmp(a, "-o") == 0 && value != NULL) {
                        output = value, ++i;
                } else if (strcmp(a, "--detail") == 0 && value != NULL) {
                        options.detail = (uint32_t)strtoul(value, NULL, 10), ++i;
                } else if (strcmp(a, "--crease") == 0 && value != NULL) {
                        options.crease = strtof(value, NULL), ++i;
                } else if (strcmp(a, "--scale") == 0 && value != NULL) {
                        options.scale = strtof(value, NULL), ++i;
                } else if (strcmp(a, "--name") == 0 && value != NULL) {
                        name = value, ++i;
                } else if (strcmp(a, "--up") == 0 && value != NULL) {
                        if (strcmp(value, "z") != 0 && strcmp(value, "y") != 0) {
                                fprintf(stderr, "bmds-mesh3d: --up y or z\n");
                                return 2;
                        }
                        options.up_z = strcmp(value, "z") == 0, ++i;
                } else if (strcmp(a, "--fit") == 0) {
                        options.fit = 1;
                } else if (strcmp(a, "--merge") == 0) {
                        options.merge = 1;
                } else if (strcmp(a, "--no-normals") == 0) {
                        options.no_normals = 1;
                } else if (strcmp(a, "--no-uvs") == 0) {
                        options.no_uvs = 1;
                } else if (strcmp(a, "--no-edges") == 0) {
                        options.no_edges = 1;
                } else if (strcmp(a, "-v") == 0) {
                        verbose = 1;
                } else if (a[0] == '-') {
                        fprintf(stderr, "bmds-mesh3d: unknown option %s\n", a);
                        usage(stderr);
                        return 2;
                } else {
                        size_t length = 0;
                        char  *text = read_file(a, &length);
                        if (text == NULL) {
                                perror(a);
                                m3d_scene_free(&scene);
                                return 1;
                        }
                        char *own = name != NULL ? strdup(name) : stem(a);
                        name = NULL;
                        int r = -1;
                        if (own == NULL)
                                snprintf(err, sizeof(err), "out of memory");
                        else if (ends_with(a, ".bpt"))
                                r = m3d_read_bpt(text, length, own, &options, &scene, err, sizeof(err));
                        else if (ends_with(a, ".obj"))
                                r = m3d_read_obj(text, length, own, &options, &scene, err, sizeof(err));
                        else
                                snprintf(err, sizeof(err), "not .obj or .bpt");
                        free(own);
                        free(text);
                        if (r != 0) {
                                fprintf(stderr, "bmds-mesh3d: %s: %s\n", a, err);
                                m3d_scene_free(&scene);
                                return 1;
                        }
                        ++inputs;
                }
        }
        if (output == NULL || inputs == 0) {
                usage(stderr);
                m3d_scene_free(&scene);
                return 2;
        }

        uint8_t *file = NULL;
        size_t   size = 0;
        if (m3d_finish(&scene, &options, err, sizeof(err)) != 0 ||
            m3d_write(&scene, &options, &file, &size, err, sizeof(err)) != 0) {
                fprintf(stderr, "bmds-mesh3d: %s\n", err);
                m3d_scene_free(&scene);
                return 1;
        }
        FILE *f = fopen(output, "wb");
        int   bad = f == NULL || fwrite(file, 1, size, f) != size;
        if (f != NULL && fclose(f) != 0) bad = 1;
        if (bad) {
                perror(output);
                remove(output);
        } else if (verbose) {
                for (uint32_t i = 0; i < scene.count; ++i)
                        printf("%s: %u vertices, %u triangles%s%s\n", scene.meshes[i].name,
                               scene.meshes[i].vertex_count, scene.meshes[i].triangle_count,
                               scene.meshes[i].has_normals ? ", normals" : "", scene.meshes[i].has_uvs ? ", uvs" : "");
                printf("%s: %zu bytes\n", output, size);
        }
        free(file);
        m3d_scene_free(&scene);
        return bad ? 1 : 0;
}
