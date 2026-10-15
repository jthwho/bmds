/**
 * @file      image.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libbmdsgame's image loading (devplan/games.md §13): a picture decoded as BGRA32 by the system,
 * converted in place into the canvas's format, the buffer then cut to it. A file is read into
 * memory once (asked about and decoded from there).
 */

#include <string.h>
#include "game_internal.h"

static int load(const void *data, uint32_t length, const bmds_game_load_t *how, bmds_game_image_t *image) {
        memset(image, 0, sizeof(*image));
        bmds_game_converter_t converter;
        if (bmds_game_converter_init(&converter, how) != 0) return -BMDS_EINVAL;
        bmds_image_decode_t d;
        memset(&d, 0, sizeof(d));
        d.size = sizeof(d);
        d.format = BMDS_PIXEL_BGRA32;
        d.flags = BMDS_IMAGE_ORIENT;
        d.data = data;
        d.length = length;
        bmds_image_info_t info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        int32_t err = bmds_image_info(-1, data, length, &d, &info);
        if (err < 0) return err;
        const uint32_t width = info.decode_width, height = info.decode_height;
        if (width == 0 || height == 0 || width > 0x4000u || height > 0x4000u) return -BMDS_ERANGE;
        d.out_size = width * height * 4;
        d.out = bmds_mem_alloc(d.out_size, BMDS_MEM_ALIGN, how->mem_flags);
        if (d.out == NULL) return -BMDS_ENOMEM;
        err = bmds_image_decode(-1, &d, NULL);
        if (err < 0) {
                bmds_mem_free(d.out);
                return err;
        }
        const uint32_t stride = width * converter.bpp;
        uint8_t       *pixels = (uint8_t *)d.out;
        uint32_t       transparent = 0;
        for (uint32_t y = 0; y < height; ++y)
                transparent += bmds_game_convert(&converter, pixels + (size_t)y * width * 4,
                                                 pixels + (size_t)y * stride, width);
        void *cut = bmds_mem_realloc(pixels, stride * height, how->mem_flags);
        if (cut != NULL) pixels = (uint8_t *)cut;
        image->pixels = pixels;
        image->stride = stride;
        image->width = (int32_t)width;
        image->height = (int32_t)height;
        image->format = how->format;
        image->flags = transparent != 0 ? BMDS_GAME_IMAGE_KEYED : 0;
        image->key = converter.key;
        return 0;
}

int bmds_game_image_load(const char *path, const bmds_game_load_t *how, bmds_game_image_t *image) {
        if (path == NULL || image == NULL) return -BMDS_EINVAL;
        memset(image, 0, sizeof(*image));
        const int32_t fd = bmds_open(path, BMDS_O_READ);
        if (fd < 0) return fd;
        bmds_stat_t st;
        memset(&st, 0, sizeof(st));
        st.size = sizeof(st);
        int32_t  err = bmds_fstat(fd, &st);
        uint8_t *file = NULL;
        if (err == 0 && (st.file_size == 0 || st.file_size > 0x4000000u)) err = -BMDS_ERANGE;
        if (err == 0 && (file = (uint8_t *)bmds_mem_alloc((uint32_t)st.file_size, 0, 0)) == NULL) err = -BMDS_ENOMEM;
        uint32_t got = 0;
        while (err == 0 && got < st.file_size) {
                const int32_t n = bmds_read(fd, file + got, (uint32_t)st.file_size - got);
                if (n <= 0)
                        err = n < 0 ? n : -BMDS_EIO;
                else
                        got += (uint32_t)n;
        }
        bmds_close(fd);
        if (err == 0) err = load(file, got, how, image);
        bmds_mem_free(file);
        return err;
}

int bmds_game_image_load_memory(const void *data, uint32_t length, const bmds_game_load_t *how,
                                bmds_game_image_t *image) {
        if (data == NULL || image == NULL) return -BMDS_EINVAL;
        return load(data, length, how, image);
}

void bmds_game_image_free(bmds_game_image_t *image) {
        if (image == NULL) return;
        bmds_mem_free((void *)image->pixels);
        memset(image, 0, sizeof(*image));
        return;
}
