/**
 * @file      fontlibrary.h
 * @copyright Jason Howard. All rights reserved.
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/font.h>
#include <bmds/fontname.h>
#include <bmds/memory.h>
#include <bmds/mutex.h>
#include <bmds/result.h>
#include <bmds/span.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

class BitmapFont;
class FontFace;
class GlyphCache;

/**
 * @brief How a `FontLibrary` reaches font files: the VFS on the board, the
 *        host's files in tests.
 * @par Thread Safety
 * Called with the library's lock held, from the task that scans or opens.
 */
class FontFiles {
        public:
                /** @brief Called with each file's full path. */
                using Visit = void (*)(void *arg, StringView path);

                /** @brief Calls @p visit for each file in folder @p dir (`NotExist`: no folder). */
                virtual Error list(StringView dir, Visit visit, void *arg) = 0;

                /**
                 * @brief Returns @p path's bytes where they lie (a ROM image), valid for good.
                 * @return The bytes, or `NotSupported` (read it with `load()`).
                 */
                virtual Result<ByteSpan> map(StringView path) = 0;

                /**
                 * @brief Reads @p path whole into memory from `Memory::alloc(Psram)`, which
                 *        the library frees.
                 * @return The bytes; `NotExist`, `NoMem`, `IoError`, `OutOfRange` (too large).
                 */
                virtual Result<Span<uint8_t>> load(StringView path) = 0;

        protected:
                ~FontFiles() = default;
};

/**
 * @brief The system's fonts (`devplan/fonts.md` §4a): its faces, the roles, the
 *        fallback faces, and the glyph cache they draw through.
 *
 * Faces are a table appended to and never reordered, so a face's index is its
 * id in the cache. The built-in bitmap fonts are added first; TrueType files
 * are found in folders by `scan()`. A file a ROM image holds is read in place;
 * another is read for its names at the scan, then loaded into PSRAM when a
 * font first uses it and kept while unused until memory runs short (the
 * library is a `Memory::Reclaimer`: unused glyphs go first, then unused
 * faces).
 *
 * Opening a name: a role is replaced by the name it is given (its size and
 * style overridden by the name's own); the family's faces are matched by
 * italic, then by the weight nearest 400 or 700; a bold it lacks is made up,
 * an italic never. A family it lacks is the `ui` role's, else unscii. A name
 * without a size takes `DefaultSize` (a bitmap face: its own height).
 *
 * @par Thread Safety
 * Any task: one mutex over the table, the roles and loading.
 */
class FontLibrary final : public Memory::Reclaimer {
        public:
                /** @brief The most faces. */
                static constexpr size_t MaxFaces = 64;

                /** @brief The fallback faces at most. */
                static constexpr size_t MaxFallbacks = 4;

                /** @brief The size of a name that gives none. */
                static constexpr uint16_t DefaultSize = 13;

                /** @brief The longest file path kept. */
                static constexpr size_t MaxPath = 127;

                /** @brief One face, as listed. */
                struct FaceInfo {
                                StringView family;
                                StringView style;
                                StringView path; ///< "" for a built-in bitmap font.
                                uint16_t   weight = 400;
                                uint16_t   glyphs = 0;     ///< 0 for a file not opened since the scan.
                                uint16_t   bitmapSize = 0; ///< A bitmap face's height; 0 for TrueType.
                                bool       italic = false;
                                bool       fixedPitch = false;
                                bool       present = true; ///< False once its file has gone.
                                bool       loaded = false; ///< Its bytes in memory now.
                                uint32_t   users = 0;      ///< `Font`s using it.
                };

                /**
                 * @brief A library drawing through @p cache, finding files through @p files
                 *        (null: the bitmap fonts alone). Both outlive it.
                 */
                FontLibrary(GlyphCache &cache, FontFiles *files);

                /** @brief Frees every face (no `Font` may be left). */
                ~FontLibrary();

                FontLibrary(const FontLibrary &) = delete;
                FontLibrary &operator=(const FontLibrary &) = delete;

                /**
                 * @brief Adds bitmap font @p font as family @p family, its baseline
                 *        @p ascent rows below its top.
                 * @return `Ok`, `Full`, `NoMem`.
                 */
                Error addBitmap(const BitmapFont &font, StringView family, uint8_t ascent);

                /**
                 * @brief Finds the TrueType files (`.ttf`, `.otf`) in @p folders (absolute
                 *        paths separated by `;`): new ones are added; faces whose files
                 *        have gone are marked so and no longer matched.
                 * @return `Ok` (a folder that doesn't exist is skipped; a file that isn't
                 *         a font we read too), `Full` (more faces than `MaxFaces`: the rest
                 *         left out).
                 */
                Error scan(StringView folders);

                /** @brief Returns the number of faces. */
                size_t faceCount() const;

                /** @brief Returns face @p index (less than `faceCount()`). */
                FaceInfo face(size_t index) const;

                /**
                 * @brief Gives role @p role the name @p name (which may not be a role).
                 * @return `Ok`, `Invalid`.
                 */
                Error setRole(FontName::Role role, const FontName &name);

                /** @brief Returns the name role @p role is given. */
                FontName role(FontName::Role role) const;

                /**
                 * @brief Sets the families tried for a missing glyph, in order (separated
                 *        by `;` or `,`; at most `MaxFallbacks`), before unscii.
                 */
                void setFallback(StringView families);

                /**
                 * @brief Opens @p name (above).
                 * @return The font, `NotExist` (nothing to fall back on), `NoMem`,
                 *         `CorruptData` (its file changed since the scan), `IoError`.
                 */
                Result<Font> open(const FontName &name);

                /** @brief Opens the name @p text (`FontName::parse()`). */
                Result<Font> open(StringView text);

                /** @brief Returns the cache it draws through. */
                GlyphCache &cache() const { return _cache; }

                size_t reclaim(MemSpace space, size_t bytes) override;
                Usage  usage() const override;

        private:
                friend class Font;

                struct Record;

                int          findFamily(StringView family, uint8_t style, bool &madeUpBold) const; // locked
                Error        load(Record &r);                                                      // locked
                Result<Font> openLocked(const FontName &name, int depth);
                void         addFile(StringView path); // locked
                void         retain(uint16_t face) const;
                void         drop(uint16_t face);

                // Lock-free reads for fonts in use (a record never moves; its face stays while used).
                const Record       *record(uint16_t face) const { return _faces[face]; }
                uint16_t            unscii() const { return _unscii; }
                Span<const int16_t> fallbacks() const { return Span<const int16_t>(_fallbacks, _fallbackCount); }

                mutable Mutex         _mutex;
                GlyphCache           &_cache;
                FontFiles            *_files;
                Record               *_faces[MaxFaces] = {};
                std::atomic<uint32_t> _count{0};
                FontName              _roles[FontName::RoleCount + 1];
                int16_t               _fallbacks[MaxFallbacks] = {};
                uint8_t               _fallbackCount = 0;
                char                  _fallbackNames[MaxFallbacks][FontName::MaxFamily + 1] = {};
                uint8_t               _fallbackNameCount = 0;
                uint16_t              _unscii = 0xffff;
                bool                  _full = false; // a scan found more faces than fit
                uint32_t              _reclaims = 0;
};

BMDS_NAMESPACE_END
