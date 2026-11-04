/**
 * @file      platform.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include "platform.h"
#include "transitions.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

namespace preso {

        Error Platform::readFile(const char *path, uint32_t maxLength, char **data, uint32_t *length) {
                *data = nullptr;
                *length = 0;
                FILE *f = fopen(path, "rb");
                if (f == nullptr) return errno == ENOENT ? Error::NotFound : Error::Io;
                // Read in pieces rather than trusting a size asked first: a pipe or a growing
                // file has none, and the limit holds either way.
                char    *buffer = nullptr;
                uint32_t used = 0;
                uint32_t capacity = 0;
                Error    error = Error::Ok;
                for (;;) {
                        if (used + 4096 + 1 > capacity) {
                                uint32_t want = capacity == 0 ? 16384 : capacity * 2;
                                char    *grown = static_cast<char *>(realloc(buffer, want));
                                if (grown == nullptr) {
                                        error = Error::NoMemory;
                                        break;
                                }
                                buffer = grown;
                                capacity = want;
                        }
                        size_t n = fread(buffer + used, 1, capacity - used - 1, f);
                        used += uint32_t(n);
                        if (used > maxLength) {
                                error = Error::TooBig;
                                break;
                        }
                        if (n == 0) {
                                if (ferror(f)) error = Error::Io;
                                break;
                        }
                }
                fclose(f);
                if (error != Error::Ok) {
                        free(buffer);
                        return error;
                }
                buffer[used] = '\0';
                *data = buffer;
                *length = used;
                return Error::Ok;
        }

        Error Platform::imageSize(const char *path, int32_t *width, int32_t *height) {
                *width = *height = 0;
                FILE *f = fopen(path, "rb");
                if (f == nullptr) return errno == ENOENT ? Error::NotFound : Error::Io;
                fclose(f);
                return Error::NotSupported;
        }

        int64_t Platform::now() {
                struct timespec t;
                timespec_get(&t, TIME_UTC);
                return int64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
        }

        bool Platform::beginPieces(const Canvas &from, const Canvas &to) {
                _piecesFrom = &from;
                _piecesTo = &to;
                return true;
        }

        bool Platform::presentPieces(const Piece *pieces, uint32_t count) {
                Canvas screen;
                if (_piecesFrom == nullptr || !beginFrame(screen)) return false;
                applyPieces(screen, *_piecesFrom, *_piecesTo, pieces, count);
                endFrame();
                return true;
        }

        void Platform::endPieces() {
                _piecesFrom = _piecesTo = nullptr;
                return;
        }

        Action defaultAction(uint32_t usage, InputMode mode) {
                // As Controls' defaults (the BMDS platform's declaration), by HID usage.
                usage &= 0xFFFF;
                if (mode == InputMode::Script) {
                        switch (usage) {
                                case 0x29: return Action::Leave;
                                case 0x4E: return Action::Next;
                                case 0x4B: return Action::Previous;
                                default: return Action::None;
                        }
                }
                if (mode == InputMode::Overview) {
                        switch (usage) {
                                case 0x50: return Action::Left;
                                case 0x4F: return Action::Right;
                                case 0x52: return Action::Up;
                                case 0x51: return Action::Down;
                                case 0x28:
                                case 0x2C: return Action::Choose;
                                case 0x2B:
                                case 0x29: return Action::Close;
                                default: return Action::None;
                        }
                }
                switch (usage) {
                        case 0x4F: // Right
                        case 0x51: // Down
                        case 0x2C: // Space
                        case 0x4E: return Action::Next;
                        case 0x50: // Left
                        case 0x52: // Up
                        case 0x2A: // Backspace
                        case 0x4B: return Action::Previous;
                        case 0x4A: return Action::First;
                        case 0x4D: return Action::Last;
                        case 0x29: return Action::Quit;
                        case 0x05: // B
                        case 0x37: return Action::Black;
                        case 0x1A: return Action::White;
                        case 0x2B: return Action::Overview;
                        case 0x0C: return Action::Into;     // I
                        case 0x06: return Action::ClearInk; // C
                        default: return Action::None;
                }
        }

        bool Platform::fileExists(const char *path) {
                FILE *f = fopen(path, "rb");
                if (f == nullptr) return false;
                fclose(f);
                return true;
        }

        void *Platform::allocatePixels(uint32_t bytes) {
                return malloc(bytes);
        }

        void Platform::freePixels(void *pixels) {
                free(pixels);
                return;
        }

} // namespace preso
