# Changelog

## [0.1.0-alpha.6](https://github.com/gitstore-dev/cli/compare/v0.1.0-alpha.5...v0.1.0-alpha.6) (2026-11-12)


### Bug Fixes

* **macos:** explain unavailable local runtime without XDG setup ([51b4cfb](https://github.com/gitstore-dev/cli/commit/51b4cfb48881dc42da5a91f5fe3da7650e4e12e9))

## [0.1.0-alpha.5](https://github.com/gitstore-dev/cli/compare/v0.1.0-alpha.4...v0.1.0-alpha.5) (2026-11-12)


### Bug Fixes

* **release:** propose Homebrew updates through protected branches ([8ed1aa1](https://github.com/gitstore-dev/cli/commit/8ed1aa1d5cc4772d69d57fde2e875adebe153af4))

## [0.1.0-alpha.4](https://github.com/gitstore-dev/cli/compare/v0.1.0-alpha.3...v0.1.0-alpha.4) (2026-11-10)


### Bug Fixes

* **ci:** run release automation on Node 24 ([4a6be76](https://github.com/gitstore-dev/cli/commit/4a6be7677f2d1a7dd15167571b6bdec3f89b67f2))

## [0.1.0-alpha.3](https://github.com/gitstore-dev/cli/compare/v0.1.0-alpha.2...v0.1.0-alpha.3) (2026-11-10)


### Bug Fixes

* enter fixture home only after switching users ([46ac874](https://github.com/gitstore-dev/cli/commit/46ac874ded4211b07dcbc325106f1d30a12d4a30))
* repair isolated Linux qualification fixtures ([9bb589a](https://github.com/gitstore-dev/cli/commit/9bb589afc90c0de15affae4e20a4a1eb03a39f9f))

## [0.1.0-alpha.2](https://github.com/gitstore-dev/cli/compare/v0.1.0-alpha.1...v0.1.0-alpha.2) (2026-11-10)


### Bug Fixes

* install declared Rust components before release builds ([45c22cf](https://github.com/gitstore-dev/cli/commit/45c22cf1ba5a08b58ef33a2b208b236fcbe2c8d4))

## [0.1.0-alpha.1](https://github.com/gitstore-dev/cli/compare/v0.1.0-alpha.0...v0.1.0-alpha.1) (2026-11-09)


### Features

* implement alpha.9 capabilities and prepare alpha releases ([abc36a2](https://github.com/gitstore-dev/cli/commit/abc36a2c39103185c53adf2a6b47e92ae907b2ca))
* prepare isolated hosted qualification for all release package channels ([507c5de](https://github.com/gitstore-dev/cli/commit/507c5de9a672f185a3f38803bbcd6cfd534fcf09))


### Bug Fixes

* create image cache downloads with private permissions ([2ccb848](https://github.com/gitstore-dev/cli/commit/2ccb8488a9fee763609c044d73a96753072e262d))
* detect formula trust capability in shallow Homebrew sources ([ce5a81d](https://github.com/gitstore-dev/cli/commit/ce5a81d66cb463a87e3c6d873091a184cbdd3252))
* embed image compression libraries in clean native builds ([1abf647](https://github.com/gitstore-dev/cli/commit/1abf6477945ca00dea2158d70f5b11c4ac6c3703))
* pin private unpacked image roots before execution binding ([108765e](https://github.com/gitstore-dev/cli/commit/108765eeb11f0d3b89ca44cf2d9e29953cf25be1))
* preserve idle watches and hash-lock privileged release dependencies ([2435ddf](https://github.com/gitstore-dev/cli/commit/2435ddf719729a06a6cf7749c7ff4d08b498e683))
* preserve packaging interpreter in complete candidate checks ([b438870](https://github.com/gitstore-dev/cli/commit/b438870146f7bd6cabc6fa7331b95f3fd027003a))
* require the configured publication approval gate before promotion ([6cf2f13](https://github.com/gitstore-dev/cli/commit/6cf2f13f9226f4ccca92335d8431f2ced148b16d))
* retry transient native source download failures ([0be7fee](https://github.com/gitstore-dev/cli/commit/0be7fee1d978e51bf353c27369fb03794fbb1e19))
* retry transient pinned libslirp source fetches ([4c37f94](https://github.com/gitstore-dev/cli/commit/4c37f943e5462706c086be7625e24108ae64a4d7))
* stage system review packages across fixture ownership boundaries ([20c2846](https://github.com/gitstore-dev/cli/commit/20c2846e79089214fb1576a5c16afc02f558d116))
* use an exact supported attestation identity and await Fedora boot ([730a3da](https://github.com/gitstore-dev/cli/commit/730a3da13ff07fe97c8407444a5722748410e5e8))

## Changelog

GitStore and this CLI are early-alpha software. Upstream schemas will continue
changing incompatibly. Upgrade compatibility, data preservation and recovery are
not guaranteed. Use, upgrade, migration or removal may cause data loss. Keep
independent backups and do not use prereleases for irreplaceable data.

No binary release has been qualified or published. Release Please will add
versioned entries from reviewed changes; this file is not publication evidence.
