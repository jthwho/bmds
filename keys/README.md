# Signing keys

Update packages (`.bmup`, devplan/update.md), packages (`.bmpk`) and package indexes (`.bmpi`,
devplan/packages.md) are signed with Ed25519. Which keys a firmware accepts is its *acceptance mode*
(`CONFIG_BMDS_UPDATE_KEYS_*`), independent of debug or release builds:

- **Dev** (the default): packages signed with the dev key or a trusted key.
- **Trusted**: packages signed with a trusted key only. The build fails if there is none.

- `dev.key`: the dev key's private half, in the repository on purpose: anyone can sign a dev
  package.
- `dev.pub`: its public half.
- `trusted.pub`: the trusted keys' public halves, one per line (absent until there is one).

**Published releases use the dev key** (the project's choice, `devplan/release.md` §2). BMDS is
meant to be built, changed and released by anyone, so there is no official key: a dev signature
shows only that a package arrived whole and was made by the tools here, not who made it.

**A trusted key is anybody's own.** Whoever builds a firmware can add their public key to
`trusted.pub`: their packages then show as trusted on the boards running it, and in the Dev mode
dev-signed packages from anyone still install, shown as such. To lock your boards to packages only
you can sign, build them with `CONFIG_BMDS_UPDATE_KEYS_TRUSTED=y` as well.

A trusted key is made once, its private half outside the repository (kept safe and backed up:
nothing can be signed with it without it), and only its public half is added here:

    tools/mkupdate keygen ~/.config/bmds/trusted.key keys/trusted.pub --comment "Jane's key 1"

A later key (`keygen` writes PUB whole) goes to a file of its own and is appended to `trusted.pub`.

`tools/mkupdate make` signs with `dev.key` unless given `--key`; `tools/mkupdate info` checks
against the keys here and names the one that signed.

**Packages also take keys added on a board** (`pkg key add FILE`, a file in this format; `pkg key
list`, `pkg key remove ID`; devplan/packages.md §22): someone else's packages can be trusted
without building a firmware. Added keys count for packages only, never for firmware updates, and
the dev key can't be added (so a Trusted firmware stays one). The comments here are what `pkg`
calls each key ("signed by Jane's key 1 (a built-in trusted key)"). `mkpkg index --key` signs an
index for a source that names its keys (`signed-by=ID` in `pkg/sources`).
