/**
 * @file      folders.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Folders watched and files' types (devplan/guiprograms.md §14, step 6b): `FolderWatch` (a
 * folder's changes called back while `app.run()` runs: the system's `fs_watch`, Linux's inotify
 * on the host) and `fileType()` (the system's `file_type`). A watch with a callback is kept (a
 * reference) until it stops, as a watched setting is.
 */

#include <errno.h>
#include <new>
#include <poll.h>
#include <string.h>
#include <unistd.h>
#include "angel.h"

#ifdef ANGEL_BMDS
#include <bmds/sys.h>
#else
#include <sys/inotify.h>
#endif

namespace angel {

        namespace {

                // What changed (FolderChange): the system's bits.
                constexpr uint32_t Entries = 1, Contents = 2, Mounts = 4;
                constexpr size_t   MaxKept = 16;

                class FolderWatch {
                        public:
                                String             path;
                                LoopSource         watch{-1, nullptr, nullptr, LoopHold::Run};
                                asIScriptFunction *onChanged = nullptr;
                                bool               listed = false; // a loop source (it has a callback)

                                void addRef() { ++_refs; }
                                void release() {
                                        if (--_refs == 0) delete this;
                                        return;
                                }

                                // Its callback let go of, out of the loop (the watch stays open).
                                void quiet() {
                                        if (listed) removeLoopSource(&watch);
                                        listed = false;
                                        if (onChanged != nullptr) onChanged->Release();
                                        onChanged = nullptr;
                                        return;
                                }
                                // Ended for good.
                                void stop() {
                                        quiet();
                                        if (watch.fd >= 0) close(watch.fd);
                                        watch.fd = -1;
                                        return;
                                }

                        private:
                                ~FolderWatch() { stop(); }
                                int _refs = 1;
                };

                FolderWatch *kept[MaxKept]; // a reference each: those with a callback
                size_t       keptCount = 0;

                FolderWatch *self(asIScriptGeneric *gen) {
                        return static_cast<FolderWatch *>(gen->GetObject());
                }
                const String &argString(asIScriptGeneric *gen, int i) {
                        return *static_cast<const String *>(gen->GetArgAddress(asUINT(i)));
                }

                // The changes waiting on @p fd (FolderChange bits), 0 if none; never waits.
                uint32_t takeChanges(int fd) {
                        struct pollfd p = {fd, POLLIN, 0};
                        if (poll(&p, 1, 0) <= 0) return 0;
#ifdef ANGEL_BMDS
                        uint32_t bits = 0;
                        return read(fd, &bits, sizeof(bits)) == ssize_t(sizeof(bits)) ? bits : 0;
#else
                        // inotify's events as the system's bits: a file made is told when it is
                        // closed after writing, as the system tells it.
                        alignas(struct inotify_event) char buffer[4096];
                        uint32_t                           bits = 0;
                        ssize_t                            n;
                        while ((n = read(fd, buffer, sizeof(buffer))) > 0) {
                                for (ssize_t at = 0; at < n;) {
                                        const auto    *e = reinterpret_cast<const struct inotify_event *>(buffer + at);
                                        const uint32_t m = e->mask;
                                        if ((m & (IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF |
                                                  IN_MOVE_SELF)) != 0 ||
                                            ((m & IN_CREATE) != 0 && (m & IN_ISDIR) != 0))
                                                bits |= Entries;
                                        if ((m & (IN_CLOSE_WRITE | IN_ATTRIB)) != 0) bits |= Contents;
                                        if ((m & IN_UNMOUNT) != 0) bits |= Mounts;
                                        at += ssize_t(sizeof(struct inotify_event) + e->len);
                                }
                        }
                        return bits;
#endif
                }

                // The loop: the folder changed.
                void folderReady(LoopSource *source) {
                        auto          *w = static_cast<FolderWatch *>(source->context);
                        const uint32_t bits = takeChanges(source->fd);
                        if (bits == 0 || w->onChanged == nullptr) return;
                        const CallArg      arg{nullptr, 0, int32_t(bits)};
                        asIScriptFunction *fn = w->onChanged;
                        w->addRef();
                        fn->AddRef();
                        callScript(fn, &arg, 1);
                        fn->Release();
                        w->release();
                }

                void unkeep(FolderWatch *w) {
                        for (size_t i = 0; i < keptCount; ++i) {
                                if (kept[i] != w) continue;
                                kept[i] = kept[--keptCount];
                                w->quiet();
                                w->release();
                                return;
                        }
                        w->quiet();
                        return;
                }

                void watchMake(asIScriptGeneric *gen) {
                        const String &path = argString(gen, 0);
#ifdef ANGEL_BMDS
                        const int32_t fd = bmds_fs_watch(path.cStr(), BMDS_O_CLOEXEC);
                        const int     error = fd < 0 ? -fd : 0;
#else
                        int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
                        int error = fd < 0 ? errno : 0;
                        if (fd >= 0 &&
                            inotify_add_watch(fd, path.cStr(),
                                              IN_ONLYDIR | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO |
                                                      IN_DELETE_SELF | IN_MOVE_SELF | IN_CLOSE_WRITE | IN_ATTRIB) < 0) {
                                error = errno;
                                close(fd);
                                fd = -1;
                        }
#endif
                        if (fd < 0) {
                                raise("Can't watch '%s': %s", path.cStr(), strerror(error));
                                gen->SetReturnAddress(nullptr);
                                return;
                        }
                        FolderWatch *w = new FolderWatch();
                        w->path = path;
                        w->watch.fd = fd;
                        w->watch.ready = folderReady;
                        w->watch.context = w;
                        gen->SetReturnAddress(w);
                }

                void watchAddRef(asIScriptGeneric *gen) {
                        self(gen)->addRef();
                }
                void watchRelease(asIScriptGeneric *gen) {
                        self(gen)->release();
                }
                void watchPath(asIScriptGeneric *gen) {
                        new (gen->GetAddressOfReturnLocation()) String(self(gen)->path);
                }
                void watchWatching(asIScriptGeneric *gen) {
                        gen->SetReturnByte(self(gen)->watch.fd >= 0);
                }

                void watchOnChanged(asIScriptGeneric *gen) {
                        FolderWatch       *w = self(gen);
                        asIScriptFunction *fn = static_cast<asIScriptFunction *>(gen->GetArgObject(0));
                        if (fn == nullptr) {
                                unkeep(w);
                                return;
                        }
                        if (w->watch.fd < 0) {
                                fn->Release();
                                raise("The watch on '%s' has stopped", w->path.cStr());
                                return;
                        }
                        if (w->onChanged != nullptr) {
                                w->onChanged->Release();
                                w->onChanged = fn;
                                return;
                        }
                        if (keptCount == MaxKept) {
                                fn->Release();
                                raise("Too many folders watched (%u)", unsigned(MaxKept));
                                return;
                        }
                        w->onChanged = fn;
                        if (!addLoopSource(&w->watch)) {
                                w->quiet();
                                return;
                        }
                        w->listed = true;
                        w->addRef();
                        kept[keptCount++] = w;
                }

                void watchStop(asIScriptGeneric *gen) {
                        FolderWatch *w = self(gen);
                        unkeep(w);
                        w->stop();
                }

                // fileType(path): "" when it can't be told.
                void fileType(asIScriptGeneric *gen) {
                        char type[128] = "";
#ifdef ANGEL_BMDS
                        if (bmds_file_type(argString(gen, 0).cStr(), 0, type, sizeof(type)) < 0) type[0] = '\0';
#else
                        (void)argString;
#endif
                        new (gen->GetAddressOfReturnLocation()) String(type);
                }

        } // namespace

        void registerFolders(asIScriptEngine *engine) {
                check(engine->RegisterEnum("FolderChange"), "FolderChange");
                check(engine->RegisterEnumValue("FolderChange", "Entries", int(Entries)), "Entries");
                check(engine->RegisterEnumValue("FolderChange", "Contents", int(Contents)), "Contents");
                check(engine->RegisterEnumValue("FolderChange", "Mounts", int(Mounts)), "Mounts");
                check(engine->RegisterObjectType("FolderWatch", 0, asOBJ_REF), "FolderWatch");
                behaviour(engine, "FolderWatch", asBEHAVE_FACTORY, "FolderWatch@ f(const string &in path)", watchMake,
                          "Watches a folder for changes (onChanged); raises if it isn't one.");
                behaviour(engine, "FolderWatch", asBEHAVE_ADDREF, "void f()", watchAddRef, nullptr);
                behaviour(engine, "FolderWatch", asBEHAVE_RELEASE, "void f()", watchRelease, nullptr);
                method(engine, "FolderWatch", "string get_path() const property", watchPath, "The folder watched.");
                method(engine, "FolderWatch", "bool get_watching() const property", watchWatching,
                       "It watches (not stopped).");
                method(engine, "FolderWatch", "void onChanged(IntCallback@ callback)", watchOnChanged,
                       "Calls back with what changed (FolderChange bits: Entries, Contents, Mounts) while "
                       "app.run() runs; null stops calling.");
                method(engine, "FolderWatch", "void stop()", watchStop, "Stops watching.");
                def(engine, "string fileType(const string &in path)", fileType,
                    "What a file is, as a MIME type (text/plain, image/png, inode/directory); \"\" if it can't be "
                    "told.");
                return;
        }

        void releaseFolders() {
                while (keptCount != 0) unkeep(kept[keptCount - 1]);
                return;
        }

} // namespace angel
