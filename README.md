# Mythic

Run Windows PC games on a non-jailbroken iPhone.

Mythic combines [Wine](https://www.winehq.org/) (ARM64EC),
[FEX-Emu](https://github.com/FEX-Emu/FEX) for x86-64 → ARM64 translation, and
[DXMT](https://github.com/3Shain/DXMT) for D3D11 → Metal, running as a single
Mach process on iOS with wineserver as a thread rather than a separate process.

## Status

Thumper and ULTRAKILL are playable. Marvel Cosmic Invasion has reached
gameplay, though a run has also ended in an unexplained termination and its
controls are not yet reliable. Others reach gameplay at low frame rates. This
is a research project, not a product: expect rough edges, per-title quirks and
breaking changes.

## Requirements

- A non-jailbroken iPhone. Development has been on an A15 (iPhone 13 Pro).
- JIT, which on iOS requires a debugger to attach —
  [StikDebug](https://github.com/0-Blu/StikJIT) is what this project uses.
- An Apple ID for signing. A free account works; its provisioning profiles
  expire after 7 days, so the app must be rebuilt and reinstalled weekly. The
  app's container survives reinstall, so prefixes and saves are preserved.

Because JIT requires debugger attach, this app cannot be distributed through the
App Store. It is installed by sideloading.

## Building

The build is split across several chains — the unix-side Wine libraries, the
ARM64EC PE modules, FEX, DXMT and the iOS app itself. `build/*/build.sh` covers
the native pieces; the app is built with `xcodebuild`.

```sh
git clone --recurse-submodules <this repo>
```

Note that `FEX`, `wine` and `research/dxmt` are submodules pointing at forks
containing the iOS work; upstream clones will not build here.

## License

**GPL-3.0-or-later** — see [`LICENSE`](LICENSE). Derivatives that are
distributed must remain open source.

Each upstream keeps its own license (Wine and GnuTLS LGPL-2.1-or-later, GMP and
Nettle LGPL-3.0-or-later, FEX-Emu and DXMT MIT); those texts are in
[`LICENSES/`](LICENSES).

[`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md) has the per-component
breakdown and records two unresolved issues you should read before
redistributing: the Microsoft Visual C++ runtime DLLs, which this project does
not distribute and which you must supply yourself (see
[`tools/fetch-vcruntime.md`](tools/fetch-vcruntime.md)), and the
corresponding-source obligation for the statically linked GMP/Nettle/GnuTLS
binaries.

## A note on upstream contributions

The forks here contain substantial AI-assisted work. FEX-Emu's contribution
policy states that AI must not be used to generate code for contributions to
that project, so **do not submit AI-generated changes from this fork upstream**.
The MIT license permits the fork itself; the policy governs contributions back.
Check each upstream's contribution policy before proposing changes to it.
