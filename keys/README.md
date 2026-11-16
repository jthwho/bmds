# Signing keys

Update packages (`.bmup`, devplan/update.md), packages (`.bmpk`) and package indexes (`.bmpi`,
devplan/packages.md) are signed with Ed25519. A key's **id** is the first 8 bytes of the BLAKE2s
of its public half, written as 16 hex digits (`pkg key list`, `signed-by=ID`).

## The kinds of key

| Kind | Public half | Private half held by | Can sign | Who uses it |
|---|---|---|---|---|
| **dev** | `dev.pub` | everyone: `dev.key` is in this repository on purpose | firmware updates and packages, on a dev-mode firmware only | BMDS's own releases; anyone trying things |
| **trusted** | `trusted.pub` (absent until there is one) | whoever builds the firmware | firmware updates and packages | someone locking their boards to what they sign |
| **package** | `packages.pub` | each key's publisher | packages and package indexes only, **never** a firmware update | repositories BMDS trusts out of the box: `bmds-apps` |
| **added** | `pkg/keys` on the board (`/cfg/pkg/keys`), by `pkg key add` or `pkg source add` | the repository's publisher | packages and package indexes only | anyone else's repository, trusted by the board's user |

A firmware accepts updates signed by the keys of its *acceptance mode*
(`CONFIG_BMDS_UPDATE_KEYS_*`), independent of debug or release builds:
- **Dev** (the default): the dev key or a trusted key.
- **Trusted**: a trusted key only. The build fails if there is none. Dev-signed packages are
  refused too; package and added keys still work for packages.

`pkg` names the kind wherever it names a signer: "signed by bmds-apps (a built-in package key)",
"(a built-in trusted key)", "(an added key)", "the dev key (anyone could have)".

**Published releases use the dev key** (the project's choice, `devplan/release.md` §2). BMDS is
meant to be built, changed and released by anyone, so there is no official firmware key: a dev
signature shows only that a package arrived whole and was made by the tools here, not who made it.

## Publishing your own packages

Make a key once; the tools never overwrite one:

    tools/mkpkg keygen ~/.config/bmds/myrepo.key --comment "Jane's apps"

It prints the key's id. Then:
- **The private half** (`myrepo.key`) stays outside every repository, backed up: nothing more can
  be signed with it once lost, and every board that trusts it would need your new key. For a CI
  release, put its contents in a secret (GitHub Actions: `gh secret set BMDS_KEY < myrepo.key`)
  and write it to a temporary file only for the signing step.
- **The public half** (`myrepo.pub`, a line `ed25519 HEX COMMENT`) is published as `key.pub`
  beside your `index.bmpi`, and its id goes in your README, so users can compare it with what
  `pkg source add` shows them before they trust it.
- **Sign** packages and the index with it: `mkpkg make --key`, `mkpkg index --key` (or
  `bmds_add_package()`'s `BMDS_KEY` in the SDK). Without `--key` the tools sign with the dev key
  and say so.

A user adds your repository with one line, which fetches `key.pub`, shows the key and asks:

    pkg source add https://github.com/jane/apps/releases/latest/download

The source is then pinned to your key (`signed-by=ID`): an index signed by any other key is
refused. `bmds-apps` (https://github.com/jthwho/bmds-apps) is a complete example
(devplan/thirdparty.md).

**Changing keys:** make the new one, sign with it, and publish both public halves for a while;
users add the new key (`pkg key add`) and the source names both ids (`signed-by=OLD,NEW`), until
the old one goes.

## Keys built into a firmware

**A trusted key is anybody's own.** Whoever builds a firmware can add their public key to
`trusted.pub`: their packages and updates then show as trusted on the boards running it, and in
the Dev mode dev-signed packages from anyone still install, shown as such. To lock your boards
to updates only you can sign, build them with `CONFIG_BMDS_UPDATE_KEYS_TRUSTED=y` as well.

    tools/mkupdate keygen ~/.config/bmds/trusted.key keys/trusted.pub --comment "Jane's key 1"

A later key (`keygen` writes PUB whole) goes to a file of its own and is appended to `trusted.pub`.

**A package key** goes in `packages.pub` instead (one `ed25519 HEX COMMENT` line each): its
repository is then trusted without asking, and it still can't sign a firmware update. Never put
a repository's key in `trusted.pub`: whoever holds it could then sign firmware for your boards.
The build refuses the dev key there.

`tools/mkupdate make` signs with `dev.key` unless given `--key`; `tools/mkupdate info` checks
against `dev.pub` and `trusted.pub` and names the one that signed (never `packages.pub`).
`tools/mkpkg info` checks against all three.

**Keys added on a board** (`pkg key add FILE`, a file in this format; `pkg key list`, `pkg key
remove ID`; devplan/packages.md §22): someone else's packages can be trusted without building a
firmware. Added keys count for packages only, and the dev key can't be added (so a Trusted
firmware stays one).
