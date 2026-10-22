/**
 * @file      languagesupport.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <bmds/codeedit.h>
#include <bmds/guiapplication.h>
#include <bmds/list.h>
#include <bmds/mutex.h>
#include <bmds/programlauncher.h>
#include <bmds/string.h>
#include <bmds/thread.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief What a language's tools add to the editor beyond colours (`devplan/editor.md` §9):
 *        checking, completion, help, running. AngelScript's is the first (`AngelSupport`); C
 *        and C++ will be another, once the system has a compiler.
 *
 * @par Thread Safety
 * The GUI loop's thread; an implementation's own threads post to it.
 */
class LanguageSupport {
        public:
                /** @brief A name it knows: the API's or the script's own. */
                struct Entry {
                                String kind{
                                        MemSpace::
                                                Psram}; ///< type, constructor, method, property, enum, constant, function, variable, class
                                String name{MemSpace::Psram}; ///< `Type.member`, `Ns::name` or `name`.
                                String decl{MemSpace::Psram};
                                String help{MemSpace::Psram};
                };

                /** @brief Where a run failed, from its report. */
                struct Failure {
                                String   file{MemSpace::Psram};
                                uint32_t line = 0; ///< From 1.
                                uint32_t column = 0;
                                String   message{MemSpace::Psram};
                                bool     exception = false; ///< At run time, not compiling.
                };

                /** @brief Told on the loop's thread. */
                class Listener {
                        public:
                                /** @brief The text checked as @p token: its diagnostics. */
                                virtual void checked(uint32_t token, Span<const CodeEdit::Diagnostic> diagnostics) = 0;
                                /** @brief The names it knows changed (the API read, a check's symbols). */
                                virtual void namesChanged() = 0;

                        protected:
                                ~Listener() = default;
                };

                virtual ~LanguageSupport() = default;

                /** @brief Returns the `Syntax` id it is for. */
                virtual const char *syntaxId() const = 0;
                /**
                 * @brief Checks @p text (the file @p path, maybe unsaved): its diagnostics told to
                 *        the listener with @p token. One at a time.
                 * @return `Ok`, `Busy` (one is being checked: ask again after), or why it can't.
                 */
                virtual Error check(uint32_t token, StringView path, StringView text) = 0;
                /** @brief Fills @p out with the names starting with @p prefix (members of @p owner's type if given). */
                virtual void complete(StringView owner, StringView prefix, List<const Entry *> &out) const = 0;
                /** @brief Fills @p out with what is known of @p name (a member of @p owner's type if given). */
                virtual void help(StringView owner, StringView name, String &out) const = 0;
                /** @brief Fills @p out with the type names to colour (sorted). */
                virtual void typeNames(List<String> &out) const = 0;
                /** @brief Fills @p argv with the command that runs @p path, its report on descriptor @p reportFd. */
                virtual Error runCommand(StringView path, int reportFd, List<String> &argv) const = 0;
                /** @brief Reads a run's report. @return True if it says where it failed. */
                virtual bool failure(StringView report, Failure &out) const = 0;
                /**
                 * @brief Fills @p argv with the command that runs @p path under the debugger
                 *        (`devplan/editor.md` §17): its report and events on @p reportFd, its
                 *        commands from @p commandFd. @return `NotSupported` if the language can't.
                 */
                virtual Error debugCommand(StringView path, int reportFd, int commandFd, List<String> &argv) const {
                        (void)path;
                        (void)reportFd;
                        (void)commandFd;
                        argv.clear();
                        return Error::NotSupported;
                }
};

/**
 * @brief AngelScript's tools: one `angel --serve` process while the editor needs it (checks
 *        and the API), runs through `angel --report-fd` (`devplan/editor.md` §9).
 */
class AngelSupport final : public LanguageSupport {
        public:
                /** @brief What it needs to start programs. */
                struct Context {
                                ProgramLauncher       *launcher = nullptr;
                                Span<const StringView> environment;
                                StringView             cwd;
                };

                AngelSupport(GuiApplication &app, const Context &context, Listener &listener);
                ~AngelSupport() override;

                const char *syntaxId() const override { return "angelscript"; }
                Error       check(uint32_t token, StringView path, StringView text) override;
                void        complete(StringView owner, StringView prefix, List<const Entry *> &out) const override;
                void        help(StringView owner, StringView name, String &out) const override;
                void        typeNames(List<String> &out) const override;
                Error       runCommand(StringView path, int reportFd, List<String> &argv) const override;
                bool        failure(StringView report, Failure &out) const override;
                Error debugCommand(StringView path, int reportFd, int commandFd, List<String> &argv) const override;

                /** @brief Returns angel's path (found on PATH), or empty. */
                StringView program() const { return _program.view(); }

        private:
                enum class Asked : uint8_t {
                        Api,
                        Check,
                };

                static void readerMain(void *arg);
                static void answerDue(void *arg);

                Error  startServer();
                void   stopServer();
                Error  send(StringView header, StringView body);
                void   take(StringView answer); // on the loop's thread
                void   readApi(StringView answer);
                void   readCheck(uint32_t token, StringView answer);
                String typeOf(StringView owner) const; // a variable's or a type's name

                GuiApplication   &_app;
                Context           _context;
                Listener         &_listener;
                String            _program{MemSpace::Psram};
                FileHandle::Ptr   _toServer;
                FileHandle::Ptr   _fromServer;
                int32_t           _pid = -1;
                Thread            _reader;
                bool              _readerStarted = false;
                Mutex             _lock; // the answers
                List<String>      _answers{MemSpace::Psram};
                std::atomic<bool> _posted{false};
                Asked             _asked[4] = {};
                uint32_t          _tokens[4] = {};
                size_t            _askedCount = 0;
                uint32_t          _failures = 0; // starts that failed: given up after a few
                List<Entry>       _api{MemSpace::Psram};
                List<Entry>       _symbols{MemSpace::Psram}; // the last successful check's
                String            _checkPath{MemSpace::Psram};
};

BMDS_NAMESPACE_END
