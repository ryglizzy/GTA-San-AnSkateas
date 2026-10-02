# GTA San Anskateas

Skate 3 skating inside GTA San Andreas. Press **J** (or **L3 + R3** on a
controller) and CJ gets on a skateboard driven by the Skate 3 engine, riding
San Andreas' own streets and collision, with Skate 3's camera, controls,
tricks, grinds and bails.

**No game files are included.** The setup builds what the mod needs from your
own copies of both games.

## Download and install

Get the latest zip from the **Releases** page and follow its `README.txt`. In short:

1. GTA San Andreas for PC **version 1.0 US** (downgrade newer copies first)
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

Needs Windows, Visual Studio 2022 Build Tools (C++), and Rust with the
`i686-pc-windows-msvc` target (`rustup target add i686-pc-windows-msvc`).

1. Clone [plugin-sdk](https://github.com/DK22Pac/plugin-sdk) next to this
   repository's folders as `plugin-sdk/` and build its San Andreas library
   (`plugin-sdk/output/lib/Plugin.lib`) with its premake/Visual Studio projects.
2. `skate-ffi\build.cmd` builds the DLL (pass your converted `skate-data\assets`
   path to also run the offline checks).
3. `sa-plugin\build.cmd` builds the ASI (`sa-plugin\build.cmd install` copies it
   into the game; set `GAME` if your game isn't in the default Steam folder).
4. `powershell -File release\make-dist.ps1` assembles the download in
   `release\dist\`. It also needs the mashup's release build for its converter
   (`iw4l-skate-convert.exe`); see the script.

## Credits

- Skate engine and converter: **2010 Rust Rewrite Mashup** by chasmlol, a fork
  of **IW4L** by vladtrc (Apache-2.0). Files changed here are marked
  `SanAnskateas addition`; see `mashup/LICENSE` and `mashup/NOTICE`.
  `skate-ffi/src/rails.rs` and `skate-ffi/src/board_export.rs` are adapted from
  the same project.
- **plugin-sdk** by DK22Pac and contributors (MIT).

Skate 3 is © Electronic Arts. Grand Theft Auto: San Andreas is © Rockstar
Games. This is an unofficial fan project, not affiliated with or endorsed by
either. You need to own both games.
