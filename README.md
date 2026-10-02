# GTA San Anskateas

Skate 3 skating inside GTA San Andreas. Press **J** (or **L3 + R3** on a
controller) and CJ gets on a skateboard driven by the Skate 3 engine, riding
San Andreas' own streets and collision, with Skate 3's camera, controls,
tricks, grinds and bails.

**No game files are included.** The setup builds what the mod needs from your
own copies of both games.

## Download and install

Get the latest zip from the **Releases** page and follow its `README.txt`. In short:

1. GTA San Andreas for PC **version 1.0 US** (downgrade newer copies first with
   [gtasa-open-downgrader](https://github.com/xxanqw/gtasa-open-downgrader))
   with an ASI loader (Silent's ASI Loader).
2. Skate 3 for **Xbox 360, extracted**: `default.xex` with its `data` folder
   (from your own disc via extract-xiso, or a Games on Demand copy via Velocity).
3. An Xbox (XInput) controller. DS4Windows works for PlayStation pads.
4. Unzip, double-click `Setup.cmd`, pick your GTA folder and your `default.xex`.

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
3. **Download [plugin-sdk](https://github.com/DK22Pac/plugin-sdk)** the same way and
   unzip it inside this project's folder, renamed to `plugin-sdk` (so that
   `plugin-sdk\plugin_sa` exists).
4. **Double-click `build.cmd`.** The first run takes a while (it also builds
   plugin-sdk). It ends with the two finished files:
   `sa-plugin\build\SanAnskateas.asi` and
   `skate-ffi\target\i686-pc-windows-msvc\release\skate_ffi.dll`.
5. **To try it in your game:** run `sa-plugin\build.cmd install` (it assumes the
   Steam folder; for another folder, first run `set GAME=C:\your\GTA folder` in
   the same window). Run the release's `Setup.cmd` once beforehand to create
   the Skate 3 data.

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
