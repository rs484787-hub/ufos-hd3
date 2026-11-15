# UFOs HD

Standalone Windows build of **U.F.O.s / Gnap** (Artech, 1997) based on the
[ScummVM](https://www.scummvm.org) Gnap engine, with the goal of adding a
high-resolution asset layer for a personal visual remaster.

* `SCUMMVM_VERSION` – the ScummVM release this build is based on.
* `src/` – files that are copied over the ScummVM source tree (currently the Gnap engine).
* `package/` – files added to the Windows package (autorun config, folders, instructions).
* `.github/workflows/build-windows.yml` – builds `UFOs.exe` on every push and publishes it
  as the **latest** release.

No game data is stored in this repository. You need your own copy of the game.

ScummVM is licensed under the GNU GPL v3; this project inherits that license.
