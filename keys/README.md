# Update signing keys

Update packages (`.bmup`, devplan/update.md) are signed with Ed25519. Which keys a firmware
accepts is its *acceptance mode* (`CONFIG_BMDS_UPDATE_KEYS_*`), independent of debug or release
builds:

- **Dev** (the default): packages signed with the dev key or a release key.
- **Release**: packages signed with a release key only. The build fails if there is none.

- `dev.key`: the dev key's private half, in the repository on purpose: anyone can sign a dev
  package.
- `dev.pub`: its public half.
- `release.pub`: the release keys' public halves, one per line (absent until there is one).

A release key is made once, its private half outside the repository (kept safe and backed up:
nothing can be released without it), and only its public half is added here:

    tools/mkupdate keygen ~/.config/bmds/release.key keys/release.pub --comment "release key 1"

A later key (`keygen` writes PUB whole) goes to a file of its own and is appended to `release.pub`.

`tools/mkupdate make` signs with `dev.key` unless given `--key`; `tools/mkupdate info` checks
against the keys here and names the one that signed.
