# GTA San Anskateas

Skate 3 skating inside GTA San Andreas. Press **J** (or **L3 + R3** on a
controller) and CJ gets on a skateboard driven by the Skate 3 engine, riding
San Andreas' own streets and collision, with Skate 3's camera, controls,
tricks, grinds and bails.

**No game files are included.** The setup builds what the mod needs from your
own copies of both games.

## Download and install

1. **Get GTA San Andreas ready (one time).** The mod needs the classic PC game
   (not the Definitive Edition) at version 1.0 US with an ASI Loader. Run
   [GTA SA Open Downgrader](https://github.com/xxanqw/gtasa-open-downgrader/releases)
   on your copy: downgrade it, and install **ASI Loader & ModLoader** from its mod list.
2. **Get your Skate 3 files (one time).** Skate 3 for Xbox 360, extracted to a
   folder with `default.xex` and `data` in it: from your disc's ISO with
   [extract-xiso](https://github.com/XboxDev/extract-xiso)
   (`extract-xiso -x "Skate 3.iso"`), or a Games on Demand copy with Velocity.
3. **A controller.** An Xbox controller, or a PlayStation one through DS4Windows.
4. **Install.** Download the zip from **Releases**, unzip it, double-click
   `Setup.cmd`, and pick your GTA folder and your `default.xex`.
5. **Play.** In game, when "Skate 3 is ready" shows, press **J** (or **L3 + R3**).

The zip's `README.txt` has the same steps in more detail, plus troubleshooting.

## Repository layout

| Folder | What |
|---|---|
| `sa-plugin/` | The game side: a C++ ASI plugin (`SanAnskateas.asi`) built on plugin-sdk |
| `skate-ffi/` | A 32-bit Rust DLL (`skate_ffi.dll`) wrapping the Skate engine in a C API |
| `mashup/` | The Skate engine part (`skate/`) of [2010 Rust Rewrite Mashup](https://github.com/chasmlol/2010-rust-rewrite-mashup) (Apache-2.0) with its licence and notice, plus small local changes; the rest of that project isn't needed here |
| `release/` | The player setup (`Setup.cmd` / `Setup.ps1`), its README, and `make-dist.ps1`, which assembles the download |

## Building from source

**Just want to play? Skip this:** use the zip from the Releases page. This is
only for people who want to change the code and compile the mod themselves
(Windows only).

1. **Install two free tools** (one time):
   - [Visual Studio Build Tools 2022](https://visualstudio.microsoft.com/visual-cpp-build-tools/).
     In the installer, tick **Desktop development with C++**.
   - [Rust](https://rustup.rs) (run `rustup-init.exe` and accept the defaults).
2. **Download this project:** the green **Code** button, then **Download ZIP**, and unzip it.
3. **Double-click `build.cmd`.** The first run downloads and builds
   [plugin-sdk](https://github.com/DK22Pac/plugin-sdk) too, so it needs internet and
   takes a few minutes. It ends with the two finished files:
   `sa-plugin\build\SanAnskateas.asi` and
   `skate-ffi\target\i686-pc-windows-msvc\release\skate_ffi.dll`.
4. **To try it in your game:** install the mod once with the release's
   `Setup.cmd` (that also makes the Skate 3 data), then copy your two files over
   it with `sa-plugin\build.cmd install`. It assumes the Steam game folder; for
   another one, first run `set GAME=C:\your\GTA folder` in the same window.

For maintainers: `release\make-dist.ps1` packs the Releases zip. It needs the
mashup's converter (`iw4l-skate-convert.exe`, from the mashup's own release),
whose location is set at the top of the script.

## Credits

Major credits to these projects and their creators:

- Skate engine: [skate-3-rust-engine](https://github.com/SK8-ENGINE/skate-3-rust-engine),
  as used in [2010 Rust Rewrite Mashup](https://github.com/chasmlol/2010-rust-rewrite-mashup)
  by chasmlol, a fork of [IW4L](https://github.com/vladtrc/iw4L) by vladtrc
  (Apache-2.0, see `mashup/`).
- [plugin-sdk](https://github.com/DK22Pac/plugin-sdk) by DK22Pac (MIT): the base of the game plugin.
- [gta-reversed](https://github.com/gta-reversed/gta-reversed): reference for how San Andreas works.
- [gtasa-open-downgrader](https://github.com/xxanqw/gtasa-open-downgrader): gets San Andreas to 1.0 US.

Unofficial fan project, not affiliated with EA or Rockstar Games. You need to
own Skate 3 and GTA San Andreas.
