/**
 * @file      sysconf.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/configtypes.h>
#include <bmds/error.h>
#include <bmds/filehandle.h>
#include <bmds/hostname.h>
#include <bmds/result.h>
#include <bmds/span.h>
#include <bmds/string.h>
#include <bmds/stringview.h>
#include <bmds/waitlist.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The system configuration: a registry of typed parameters with
 *        defaults, changed for the running system and saved to `/boot` only
 *        when asked (`devplan/sysconf.md`).
 *
 * Each parameter has a current value and maybe a saved one. At boot the
 * current value is the saved one, else the default; `set()` and
 * `setDefault()` change only the current value; `save()` puts it in the
 * store (`/boot/sysconf`), or takes the key out of it when it is the
 * default. Values are checked against their declared type, limits and
 * check before they are taken.
 *
 * The built-in parameters are declared with `BMDS_SYSCONF` where they are
 * used; programs declare theirs with `declare()`. A `Watcher` is told the
 * parameters that change under a prefix.
 *
 * Each parameter has an `Id` for the rest of the boot (`devplan/sysconf.md`
 * §12.1): `find()` once, then use the id, which compares no strings. Ids
 * aren't kept across boots or written anywhere: the built-ins' order is the
 * linker's. The forms taking a key find it each time.
 *
 * @par Thread Safety
 * Thread-safe (one lock). `init()` once, at boot (the boot stage), or a
 * test's `initForHost()`.
 */
namespace SysConf {

        using Param = Config::Param;

        /** @brief Where the saved values are kept. */
        inline constexpr const char *StorePath = "/boot/sysconf";

        /** @brief Parameters programs may declare at run time (`declare()`). */
        inline constexpr size_t MaxRuntime = 64;

        /** @brief Parameters in all, built-in and declared at run time (a watcher's mask has a bit each). */
        inline constexpr size_t MaxEntries = 256;

        /** @brief A parameter for the rest of the boot (`find()`); never kept across boots. */
        enum class Id : uint16_t {
        };

        /** @brief No parameter. */
        inline constexpr Id NoId = Id(0xffff);

        /** @brief The largest store `init()` reads, in bytes. */
        inline constexpr size_t MaxStoreBytes = 64 * 1024;

        /** @brief True once the registry is set up (`init()`): before, every call fails. */
        bool isReady();

        /**
         * @brief Boot stage: takes the built-in declarations (`BMDS_SYSCONF`) and
         *        the saved values from `StorePath` (missing: none saved).
         * @return `Ok`, or `NoMem`; a store that can't be read is logged.
         */
        Error init();

        /**
         * @brief As `init()`, with @p builtins (static) as the built-in
         *        declarations and @p storePath as the store (host tests).
         *        Forgets everything from before.
         */
        Error initForHost(Span<const Param> builtins, StringView storePath);

        /** @brief What `info()` and `list()` report about a parameter. */
        struct Info {
                        const Param *param = nullptr;  ///< Its declaration.
                        String       value;            ///< Its current value.
                        String       saved;            ///< Its saved value ("" if none).
                        String       defaultValue;     ///< Its default, in canonical form.
                        bool         hasSaved = false; ///< True if it is in the store.
                        bool         changed = false;  ///< True if the current value isn't what a reboot would give.
                        bool         isDefault = false; ///< True if the current value is the default.
                        bool         runtime = false;   ///< True if a program declared it (`declare()`).

                        /** @brief True if the current value isn't what a reboot would give. */
                        bool isChanged() const { return changed; }

                        /** @brief "default", "saved" or "changed" (`sysconf list`'s word). */
                        const char *stateName() const;
        };

        /**
         * @brief Returns @p key's id: the same for the rest of the boot.
         * @return The id, or `NotExist`.
         */
        Result<Id> find(StringView key);

        /** @brief Returns @p id's key ("" if none); valid until reboot. */
        StringView keyOf(Id id);

        /** @brief Sets @p out to @p key's current value. @return `Ok`, `NotExist`, `NoMem`. */
        Error get(StringView key, String &out);

        /**
         * @brief Writes the system's host name into @p out: `system/hostname`, or
         *        `bmds-` and the serial number's last six while it is empty (`devplan/notifications.md` §2;
         *        target only).
         * @return Its length.
         */
        size_t hostname(char (&out)[HostName::MaxBytes + 1]);

        /**
         * @brief Sets @p out to the text template @p text (`TextTemplate`,
         *        `devplan/templates.md`) expanded with the system's values: `/home`,
         *        the host name, the serial number, the local time now (target only).
         * @return `Ok`, `Invalid` (not a template), `NoMem`.
         */
        Error expand(StringView text, String &out);

        /** @brief Sets @p out to @p id's current value. @return `Ok`, `NotExist`, `NoMem`. */
        Error get(Id id, String &out);

        /** @brief @p key's current value as a number (`Int`); `NotExist`, `Invalid` (another type). */
        Result<int32_t> getInt(StringView key);

        /** @brief @p key's current value (`Bool`). */
        Result<bool> getBool(StringView key);

        /** @brief @p key's current value as 0xRRGGBB (`Color`). */
        Result<uint32_t> getColor(StringView key);

        /** @brief @p id's current value as a number (`Int`); `NotExist`, `Invalid` (another type). */
        Result<int32_t> getInt(Id id);

        /** @brief @p id's current value (`Bool`). */
        Result<bool> getBool(Id id);

        /** @brief @p id's current value as 0xRRGGBB (`Color`). */
        Result<uint32_t> getColor(Id id);

        /**
         * @brief Sets @p key's current value to @p value (not saved), checked
         *        against its type, limits and check; watchers are told if it changed.
         * @return `Ok`, `NotExist` (no such key), `Invalid`, `OutOfRange`, the
         *         check's error, `NoMem`.
         * @blocking The check (a `stat()` for `home`).
         */
        Error set(StringView key, StringView value);

        /** @brief As `set(StringView, StringView)`, for @p id. @blocking The check. */
        Error set(Id id, StringView value);

        /** @brief Sets @p key's current value to its default (not saved). */
        Error setDefault(StringView key);

        /** @brief Sets @p id's current value to its default (not saved). */
        Error setDefault(Id id);

        /** @brief Sets @p key's current value to its saved one, else its default. */
        Error revert(StringView key);

        /** @brief Sets @p id's current value to its saved one, else its default. */
        Error revert(Id id);

        /**
         * @brief Saves @p key's current value (the default: taken out of the store)
         *        and rewrites the store.
         * @return `Ok`, `NotExist`, or the error writing the store (nothing saved then).
         * @blocking A write to `/boot`.
         */
        Error save(StringView key);

        /** @brief As `save(StringView)`, for @p id. @blocking A write to `/boot`. */
        Error save(Id id);

        /** @brief Saves every key whose current value differs from the store's. */
        Error saveAll();

        /** @brief Fills @p out about @p key. @return `Ok`, `NotExist`, `NoMem`. */
        Error info(StringView key, Info &out);

        /** @brief Fills @p out about @p id. @return `Ok`, `NotExist`, `NoMem`. */
        Error info(Id id, Info &out);

        /** @brief Called by `list()` for each parameter, in key order (the lock isn't held). */
        using ListFn = void (*)(void *arg, const Info &info);

        /**
         * @brief Calls @p fn for each parameter whose key is @p prefix or starts
         *        with it followed by `/` ("" for all), in key order.
         * @return `Ok` or `NoMem`.
         */
        Error list(StringView prefix, ListFn fn, void *arg);

        /**
         * @brief Returns the first parameter after @p after in key order (`NoId`:
         *        from the start) whose key is @p prefix or under it ("" for all):
         *        a listing one call at a time (the SDK's `sysconf_next`).
         * @return Its id, or `NotExist` at the end (or for an @p after not known).
         */
        Result<Id> next(StringView prefix, Id after);

        /**
         * @brief Declares a parameter at run time (a program's own). Its strings
         *        are copied. A saved value for it is applied now, and watchers
         *        whose prefix it is under watch it from now on.
         * @return Its id (the same one if it was declared already with this
         *         type), or `Exists` (declared with another type), `Invalid` (the
         *         key or the default), `Full` (`MaxRuntime`, `MaxEntries`), `NoMem`.
         */
        Result<Id> declare(const Param &param);

        /**
         * @brief Told which parameters change under a prefix.
         *
         * Owned by the listener (no allocation in the registry). The prefix is
         * matched once, into a bit mask over the parameters: when the watcher
         * starts, and for a key declared later, when it is declared. A change
         * is then a bit test, and puts the parameter's id in a ring of
         * `RingSize`; more than fit sets `overflowed()` ("anything may have
         * changed"). Each change then calls the notification, if any, and
         * notifies `waiters()`, after the value changed and without the
         * registry's lock (the listener may read the value at once).
         *
         * @par Thread Safety
         * `next()` and `overflowed()` from one task; `start()` and `stop()`
         * from any; the notification must not block, nor stop a watcher.
         */
        class Watcher {
                public:
                        /** @brief Called on the task that changed a value. */
                        using Notify = void (*)(void *arg);

                        /** @brief Ids kept between `next()` calls. */
                        static constexpr size_t RingSize = 16;

                        constexpr Watcher() = default;
                        ~Watcher();
                        Watcher(const Watcher &) = delete;
                        Watcher &operator=(const Watcher &) = delete;

                        /**
                         * @brief Watches @p prefix ("" for every key; else a key, or a key's
                         *        leading components: `wm/palette`).
                         * @return `Ok`, `InvalidState` (started), or `Invalid`.
                         */
                        Error start(StringView prefix, Notify notify, void *arg);

                        /** @brief Stops (waits for a notification running on another task). */
                        void stop();

                        /**
                         * @brief Takes the oldest changed parameter's id into @p id.
                         * @return True if there was one.
                         */
                        bool next(Id &id);

                        /** @brief True (once, then cleared) if more changed than the ring held. */
                        bool overflowed();

                        /** @brief True if `next()` or `overflowed()` has something. @nonblocking */
                        bool isPending();

                        /** @brief Released after each change it is told of. */
                        WaitList &waiters() { return _waiters; }

                        // The registry's (lock held).
                        bool     matches(StringView key) const;
                        bool     watches(size_t entry) const { return (_mask[entry / 32] >> (entry % 32)) & 1u; }
                        void     watch(size_t entry) { _mask[entry / 32] |= 1u << (entry % 32); }
                        void     push(uint16_t entry);
                        Watcher *nextWatcher = nullptr;
                        Notify   notify = nullptr;
                        void    *notifyArg = nullptr;

                private:
                        char          _prefix[Config::MaxKey + 1] = {};
                        uint32_t      _mask[MaxEntries / 32] = {}; // the entries under the prefix
                        uint16_t      _ring[RingSize] = {};
                        uint8_t       _head = 0;
                        uint8_t       _count = 0;
                        bool          _overflowed = false;
                        bool          _started = false;
                        WaitListOf<4> _waiters;
        };

        /**
         * @brief Opens a watch on @p prefix as a file (the SDK's `sysconf_watch`,
         *        `devplan/sysconf.md` §12.2): a read gives whole `uint32_t`
         *        records, the id of each parameter changed or `OverflowRecord`,
         *        and waits while there are none; it is readable for `io_poll`
         *        when there are. The watch ends with the last handle.
         * @return The handle (`FileHandle::Read`), or `Invalid` (the prefix),
         *         `InvalidState` (not set up), `NoMem`.
         */
        Result<FileHandle::Ptr> openWatch(StringView prefix);

        /** @brief A watch file's record after an overflow: read again what matters. */
        inline constexpr uint32_t OverflowRecord = 0xffffffffu;

} // namespace SysConf

BMDS_NAMESPACE_END

/**
 * @def BMDS_SYSCONF
 * @brief Declares a built-in parameter (`devplan/sysconf.md` §2): a
 *        `Config::Param` in rodata, found by `SysConf::init()`. Its key is
 *        an interface once shipped.
 * @param Id A unique identifier for the object.
 * @param ...   The `Config::Param`'s fields, in order.
 */
#define BMDS_SYSCONF(Id, ...)                                                                                          \
        [[gnu::used, gnu::section(".bmds_conf")]] constexpr ::bmds::Config::Param bmdsConf_##Id = {__VA_ARGS__};
