/**
 * @file      stage.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Angel's stage (devplan/preso.md §8c): a window that plays a script's scene, as a deck's region
 * does. Angel runs a script as it always has; its first scene call (scene.add, play, wait, pause
 * or start), or a draw(Scene@, float) when it ends, hands it to the stage, which runs it again
 * from its start: beats by Space, back by Left, as a deck steps.
 */

#ifndef PRESO_STAGE_H
#define PRESO_STAGE_H

#include "../core/fonts.h"
#include "../core/images.h"
#include "../core/platform.h"
#include "../core/theme.h"
#include "runtime.h"

namespace preso {

        /**
         * @brief Angel's stage: the SceneHost of angel's engine, and the window a scene plays in.
         *
         * @par Thread Safety
         * Not thread-safe: angel's thread.
         */
        class Stage : public SceneHost, public App {
                public:
                        /** @brief The scene's size: a window that fits the board's screen with room round it. */
                        static constexpr int32_t Width = 960;
                        static constexpr int32_t Height = 540;
                        /** @brief How an error is said: angel's diagnostic(), @p message without its place. */
                        using Reporter = void (*)(const char *file, int line, const char *message);

                        /** @brief For the script at @p path (its files are named from its folder). */
                        Stage(Platform &platform, const char *path, Reporter reporter);
                        ~Stage() override;
                        Stage(const Stage &) = delete;
                        Stage &operator=(const Stage &) = delete;

                        /** @brief Becomes @p engine's host (before its script is built). */
                        void attach(asIScriptEngine *engine);
                        /** @brief Whether a scene call asked for the stage (angel's run was aborted for it). */
                        bool isWanted() const { return _wanted; }
                        /** @brief Whether @p module has a draw(Scene@, float): a stage when its statements end. */
                        static bool hasDraw(asIScriptModule *module);
                        /**
                         * @brief Plays @p module (built, its host this) from its start: in a window until it
                         *        is closed, or with no screen to its end, its beats taken at once.
                         * @return 0, or 1 if the script failed.
                         */
                        int play(asIScriptModule *module);
                        /** @brief Lets go of what play() made of the script (before its engine goes). */
                        void finish();

                        // ---- SceneHost
                        asIScriptEngine *engine() override { return _engine; }
                        Platform        &platform() override { return _platform; }
                        FontCache       &fonts() override { return _fonts; }
                        ImageCache      *images() override { return &_images; }
                        const Theme     &theme() const override { return Theme::paper(); }
                        void             size(int32_t *width, int32_t *height) const override;
                        bool         resolve(StringView path, char *out, size_t size, const char **why) const override;
                        bool         isSandboxed() const override { return false; }
                        void         report(const ScriptPlace &place, uint32_t line, const char *message) override;
                        int64_t      runawayNs() const override { return 0; }
                        bool         passesOutput() const override { return true; }
                        bool         takeInitException(char *out, size_t size, int *line) override;
                        AngelRegion *needRegion() override;
                        ScriptFont  *scriptFont(void *font) override;
                        void        *makeFont(const FontKey &key) override;
                        void         releaseFont(void *font) override;
                        void        *makeEvent(const ScriptInput &input) override;
                        void         releaseEvent(void *event) override;

                        // ---- App
                        void event(const Event &event) override;
                        void filesChanged() override {}
                        void frame(int64_t ns) override;

                private:
                        void playHeadless();
                        void redraw(const Rect *areas, uint32_t count);
                        void next();
                        void back();
                        void settle();
                        void input(const ScriptInput &in);

                        Platform        &_platform;
                        FontCache        _fonts;
                        ImageCache       _images;
                        Reporter         _report;
                        char             _path[512] = {};
                        char             _folder[512] = ".";
                        asIScriptEngine *_engine = nullptr;
                        AngelRegion     *_region = nullptr;
                        bool             _wanted = false;
                        Canvas           _canvas;
                        void            *_pixels = nullptr;
                        int64_t          _last = 0;       ///< The last frame's time.
                        uint32_t         _buttons = 0;    ///< The pointer's, as last told.
                        int32_t          _titleBeat = -1; ///< The beat the title says.
        };

} // namespace preso

#endif // PRESO_STAGE_H
