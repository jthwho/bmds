/**
 * @file      packages.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/fetch.h>
#include <bmds/list.h>
#include <bmds/pkgfile.h>
#include <bmds/pkgindex.h>
#include <bmds/result.h>
#include <bmds/span.h>
#include <bmds/string.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Installed packages (devplan/packages.md §7): installing a `.bmpk` into the root,
 *        removing one, and the database that records what each put where. Over the VFS alone,
 *        so the host tests run it on a tmpfs.
 *
 * The database is a directory (`pkg/db`) of files per installed package: `NAME.status` (manifest
 * field lines: the version, what it is, who signed it, whether the user asked for it),
 * `NAME.manifest` (the package's manifest as installed: every file with its hash),
 * `NAME.description` and `NAME.icons` (when it has them), and `NAME.pending` while it is being
 * installed or removed. Each is written whole and renamed into place.
 *
 * Installing checks everything before writing anything (the signature against the keys, the
 * architecture and ABI, every section's hash, the manifest, what is installed it needs, files
 * another package owns), writes each file beside its place (`PATH.pkgnew`, its hash checked) and
 * renames them all into place, then removes what the old version had that the new doesn't. A
 * power cut leaves `NAME.pending`, naming the package's file: `recover()` installs it again (or
 * finishes a removal). A file in the way that no package owns is taken over if it holds the same
 * bytes (or a link the same target), else refused unless forced.
 *
 * @par Thread Safety
 * One operation at a time across the system: a second gets `Busy`. Not callable from an ISR.
 */
namespace Packages {

        /** @brief What `pkg/dev` says of packages signed by the dev key (devplan/packages.md §6). */
        enum class DevPolicy : uint8_t {
                Allow,  ///< Installed as any other.
                Ask,    ///< The command asks about them (`-y` answers).
                Refuse, ///< Refused wherever one is read.
        };

        /** @brief Where things are and what this system is. */
        struct Config {
                        StringView                       root;              ///< Where packages install (`pkg/root`).
                        StringView                       db;                ///< The database (`pkg/db`).
                        Span<const SignedContainer::Key> keys;              ///< The keys packages may be signed with.
                        bool (*isDevKey)(size_t index) = nullptr;           ///< Whether `keys[index]` is the dev key.
                        const char *(*keyComment)(size_t index) = nullptr;  ///< `keys[index]`'s comment, or null.
                        const SignedContainer::Key *devKey = nullptr;       ///< The dev key, accepted or not.
                        StringView                  keysDir;                ///< Added keys (`pkg/keys`).
                        DevPolicy                   dev = DevPolicy::Allow; ///< `pkg/dev`.
                        StringView                  arch;                   ///< This system's (`esp32p4`).
                        uint32_t                    abi = 0;                ///< `BMDS_ABI_VERSION`.
                        StringView                  cache;                  ///< Lists read by `update()` (`pkg/cache`).
                        StringView                  sources;                ///< `;`-separated (`pkg/sources`).
                        int64_t (*now)() = nullptr;    ///< Seconds since 1970 (indexes' dates), or null.
                        FetchFunction fetch = nullptr; ///< Fetches a URL (network sources); null: none.
        };

        /** @brief What signed a package. */
        enum class Signer : uint8_t {
                Trusted, ///< A trusted key built in (`keys/trusted.pub`).
                Dev,     ///< The dev key: anyone could have.
                Added,   ///< A key added on this board (`pkg key add`).
        };

        /** @brief What a package is: from its file, or from the database. Views into `text`. */
        struct Info {
                        String     text; ///< The fields' storage (the status file, or the manifest's fields).
                        StringView name;
                        StringView version;
                        StringView arch;
                        StringView title;
                        StringView summary;
                        StringView category;
                        StringView license;
                        StringView homepage;
                        StringView depends;
                        uint32_t   abi = 0;
                        uint64_t   installedBytes = 0;
                        uint64_t   fileBytes = 0; ///< The `.bmpk`'s size (from a file).
                        int64_t    madeAt = 0;    ///< Seconds since 1970.
                        Signer     signer = Signer::Dev;
                        uint8_t    keyId[SignedContainer::KeyIdBytes] = {};
                        bool       automatic = false; ///< Installed as a dependency, not asked for.
                        uint32_t   files = 0;
                        uint32_t   links = 0;
                        uint32_t   directories = 0;
        };

        /** @brief What `installFile()` and `remove()` are doing, for progress. */
        enum class Step : uint8_t {
                Checking,  ///< Reading the package and its hashes.
                Writing,   ///< Writing its files.
                Replacing, ///< Moving them into place.
                Removing,  ///< Removing files.
                Fetching,  ///< Fetching it from its source (`fetchPlanned()`).
        };

        /** @brief Who hears about progress and what was decided along the way. */
        struct Listener {
                        void (*progress)(void *arg, StringView package, Step step, uint64_t done,
                                         uint64_t total) = nullptr;
                        void (*note)(void *arg, StringView package, StringView text) = nullptr;
                        void *arg = nullptr;
        };

        /** @brief One package of a plan (`plan()`): its file, what it is, and why it comes. */
        struct Planned {
                        String  path;              ///< The `.bmpk`, absolute.
                        Info    info;              ///< From `peekFile()`.
                        bool    automatic = false; ///< A dependency, not asked for.
                        bool    upgrade = false;   ///< Asked for by `PlanMode::Upgrade`: it keeps its mark.
                        bool    remote = false;    ///< `path` is a URL: `fetchPlanned()` before installing.
                        uint8_t hash[PkgIndex::HashBytes] = {}; ///< A remote one's whole-file hash, as listed.
        };

        /** @brief How to install. */
        struct InstallOptions {
                        bool automatic = false; ///< As a dependency (kept as before on an upgrade).
                        bool overwrite = false; ///< Take over unowned files that differ.
                        bool dryRun = false;    ///< Check everything, write nothing.
                        /// The plan's packages before this one: a dry run counts them as installed.
                        Span<const Planned> earlier;
        };

        /**
         * @brief Finishes what a power cut interrupted (`NAME.pending`): installs the named file
         *        again, or finishes a removal. Called by every operation first.
         * @return `Ok` (nothing pending, or all of it finished); else the first failure, the
         *         package named in @p why.
         */
        Error recover(const Config &config, const Listener &listener, String &why);

        /**
         * @brief Reads the package at @p path into @p out and checks it whole: its head and
         *        signature, every section's hash, its manifest; and that it suits this system.
         *        Its manifest and description to @p manifest and @p description when not null.
         * @return `Ok`; `CorruptData` with what is wrong in @p why (not a package, damaged, not
         *         signed by a key, for another architecture or a newer ABI); or the file's error.
         */
        Error inspectFile(StringView path, const Config &config, Info &out, String *manifest, String *description,
                          String &why);

        /**
         * @brief Reads what planning needs of the package at @p path into @p out: its head and
         *        signature, that it suits this system, and its manifest (its hash checked). Not
         *        the data: `installFile()` checks that.
         * @return As `inspectFile()`.
         */
        Error peekFile(StringView path, const Config &config, Info &out, String &why);

        /** @brief What to plan for: a package's file, or a name from the lists. */
        struct Request {
                        StringView text;
                        bool       isFile = false;
        };

        /** @brief What `plan()` plans. */
        enum class PlanMode : uint8_t {
                Install, ///< The requests and what they need.
                Upgrade, ///< Every installed package with a newer one in the lists (no requests).
        };

        /**
         * @brief Plans into @p out the packages to install, each after its dependencies
         *        (`PkgSolver`): for @p requests (files, and names: the newest in the lists, unless
         *        installed in that version already, which @p listener hears), or with
         *        `PlanMode::Upgrade` for every installed package that the lists have newer (each
         *        keeping its mark). Candidates: the files given, packages in their folders
         *        (`NAME_*.bmpk`, looked for when one is needed), and the lists' (`update()`).
         *        A list's package is peeked before it is planned: it must be there and be what
         *        the list says.
         * @return `Ok` (@p out may be empty: nothing to do); `NotFound` (a name in no list, or a
         *         dependency found nowhere: @p why says which, the chain and where it looked);
         *         `InvalidState` (a cycle, a package given twice, a plan that breaks an installed
         *         package, a list out of date); a file's error from `peekFile()`.
         */
        Error plan(Span<const Request> requests, PlanMode mode, const Config &config, const Listener &listener,
                   List<Planned> &out, String &why);

        /**
         * @brief Fetches remote package @p planned into the cache (`CACHE/packages`): a `.part`
         *        resumed by its size, the whole file's hash checked against its list, renamed, then
         *        peeked (it must be what the list says). @p planned then names the file, with who
         *        signed it. One fetched before and still good isn't fetched again.
         * @return `Ok`; the fetch's error (`.part` kept to resume); `CorruptData` (the hash, or not
         *         what the list says); `NotSupported` (no `Config::fetch`).
         * @blocking
         */
        Error fetchPlanned(Planned &planned, const Config &config, const Listener &listener, String &why);

        /**
         * @brief Installs (or upgrades, or reinstalls) the package at @p path; what it is to
         *        @p out (with a dry run too).
         * @return `Ok`; `CorruptData` or `InvalidState` with the reason in @p why (a check failed,
         *         a dependency missing, a file another package owns or an unowned one in the
         *         way); `Busy` (another operation, or a file in use: @p why names it); or the
         *         filesystem's error.
         */
        Error installFile(StringView path, const Config &config, const InstallOptions &options,
                          const Listener &listener, Info &out, String &why);

        /**
         * @brief Removes the installed package @p name: its files, links and emptied directories,
         *        then its records.
         * @return `Ok`; `NotFound` (not installed); `InvalidState` if an installed package other
         *         than those in @p alsoRemoving depends on it (@p why names them); or an error.
         */
        Error remove(StringView name, Span<const StringView> alsoRemoving, const Config &config,
                     const Listener &listener, String &why);

        /**
         * @brief Checks what `remove()` would, writing nothing: for a plan or a dry run.
         * @return As `remove()`: `Ok`, `NotFound`, `InvalidState` (@p why names what needs it),
         *         `Busy`, or an error.
         */
        Error checkRemove(StringView name, Span<const StringView> alsoRemoving, const Config &config, String &why);

        /**
         * @brief Marks installed package @p name automatic (a dependency `autoremove` may take)
         *        or not (asked for).
         * @return `Ok`; `NotFound`; `Busy`; or the filesystem's error.
         */
        Error setAutomatic(StringView name, bool automatic, const Config &config, String &why);

        /**
         * @brief The automatic installed packages nothing staying needs once @p removing are
         *        gone (`PkgSolver::unneeded()`), sorted, into @p out.
         * @return `Ok`, or an error reading the records.
         */
        Error unneeded(Span<const StringView> removing, const Config &config, List<String> &out);

        /**
         * @brief Checks a `pkg/sources` value: `;`-separated sources, each `file:` and an absolute,
         *        normalized folder, or an `http:` or `https:` URL (the folder of its `index.bmpi`),
         *        then optionally `signed-by=ID[,ID...]` (keys' 16-digit ids, at most 8).
         * @return `Ok`, or `Invalid` with the reason in @p why.
         */
        Error checkSources(StringView sources, String &why);

        /** @brief What `update()` did for one source. */
        struct SourceResult {
                        String   source;
                        uint32_t packages = 0;        ///< In its list.
                        bool     indexedHere = false; ///< The folder's index was missing or out of date.
                        Error    error;
                        String   why; ///< When `error` isn't `Ok`.
        };

        /**
         * @brief Reads every source's index into the cache (`CACHE/lists`), one result each into
         *        @p out, and removes the lists of sources no longer configured. A `file:` folder
         *        whose `index.bmpi` doesn't list exactly its packages is indexed here instead; a
         *        network source's is fetched and must be signed (by a key it names, else by any in
         *        the keyring).
         * @return `Ok`; else the first source's error (the others still updated), or `Busy`.
         */
        Error update(const Config &config, const Listener &listener, List<SourceResult> &out);

        /**
         * @brief Writes `DIR/index.bmpi` for the packages in @p dir: unsigned, their stanzas and
         *        icons. A file that isn't a package that suits is left out, and @p listener hears.
         * @return `Ok` with the packages listed in @p count; or the folder's error.
         */
        Error indexFolder(StringView dir, const Config &config, const Listener &listener, uint32_t &count, String &why);

        /** @brief A package the lists have. */
        struct Available {
                        Info    info;              ///< From its stanza (who signed it isn't known until it is read).
                        String  path;              ///< Its file.
                        String  source;            ///< The source it is from.
                        String  description;       ///< Its one line, if any.
                        bool    checkHash = false; ///< Its source names keys: the file must hash as listed.
                        bool    remote = false;    ///< `path` is a URL (a network source's).
                        uint8_t hash[PkgIndex::HashBytes] = {}; ///< The whole file's, as listed.
        };

        /**
         * @brief Every package in the lists of the configured sources that suits this system,
         *        into @p out (a source not yet updated has none).
         * @return `Ok`; `CorruptData` (a list that doesn't read: @p why names it).
         */
        Error available(const Config &config, List<Available> &out, String &why);

        /** @brief As `icon()`, for package @p name from the lists. @return `NotFound` if none has an icon for it. */
        Error availableIcon(StringView name, uint16_t side, const Config &config, List<uint8_t> &out);

        /** @brief Empties the cache. @return `Ok`, `Busy`, or the filesystem's error. */
        Error clean(const Config &config, String &why);

        /** @brief A key of the keyring. */
        struct KeyEntry {
                        SignedContainer::Key key;
                        uint8_t              id[SignedContainer::KeyIdBytes] = {};
                        Signer               kind = Signer::Trusted;
                        String               comment;
        };

        /**
         * @brief The keyring into @p out: the built-in keys, then those added (`keysDir`); an
         *        added file that isn't a key, the dev key, or a key already listed is skipped.
         * @return `Ok`, or `NoMem`.
         */
        Error keyring(const Config &config, List<KeyEntry> &out);

        /**
         * @brief Adds the first key line of the key file @p path to the keyring (`keysDir/ID.pub`);
         *        it to @p added.
         * @return `Ok`; `CorruptData` (no key line); `Exists` (in the keyring already);
         *         `PermissionDenied` (the dev key); or the filesystem's error. @p why says which.
         */
        Error addKey(StringView path, const Config &config, KeyEntry &added, String &why);

        /**
         * @brief Removes the added key whose id is or starts with @p id (at least 4 hex digits);
         *        it to @p removed.
         * @return `Ok`; `NotFound`; `Invalid` (too short, or more than one matches);
         *         `PermissionDenied` (a built-in key); or the filesystem's error.
         */
        Error removeKey(StringView id, const Config &config, KeyEntry &removed, String &why);

        /**
         * @brief Who signed @p info, in words: its key's name to @p name ("the dev key", an added
         *        key's comment, "key ID") and what it is to @p what ("a built-in trusted key",
         *        "an added key", "anyone could have", "no longer in the keyring").
         */
        void describeSigner(const Config &config, const Info &info, String &name, String &what);

        /** @brief The installed packages' names, sorted. */
        Result<List<String>> installed(const Config &config);

        /** @brief Reads installed package @p name's record into @p out. @return `NotFound` if it isn't installed. */
        Error readInstalled(StringView name, const Config &config, Info &out);

        /**
         * @brief Reads installed package @p name's manifest (or description) into @p out.
         * @return `NotFound` if it isn't installed (or has no description).
         */
        Error readManifest(StringView name, const Config &config, String &out);
        Error readDescription(StringView name, const Config &config, String &out); ///< See `readManifest()`.

        /** @brief What `verify()` found about one entry. */
        enum class Problem : uint8_t {
                Missing, ///< It isn't there.
                Changed, ///< Its bytes (or a link's target) differ.
                NotLink, ///< A link that is something else now.
        };

        /**
         * @brief Checks installed package @p name's files against their hashes and its links
         *        against their targets, telling @p found of each that doesn't match.
         * @return `Ok` (whatever was found); `NotFound`; or an error reading the records.
         */
        Error verify(StringView name, const Config &config, void (*found)(void *arg, StringView path, Problem problem),
                     void *arg);

        /**
         * @brief Reads installed package @p name's icon nearest @p side (that side, else the next
         *        bigger, else the biggest) into @p out, a PNG.
         * @return `NotFound` if it isn't installed or has no icon.
         */
        Error icon(StringView name, uint16_t side, const Config &config, List<uint8_t> &out);

} // namespace Packages

BMDS_NAMESPACE_END
