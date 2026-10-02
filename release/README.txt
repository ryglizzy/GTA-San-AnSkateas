GTA San Anskateas
=================

Skate 3 skating inside GTA San Andreas. Press J (or L3 + R3 on a controller)
and CJ gets on a skateboard driven by the Skate 3 engine, riding San Andreas'
own streets, with Skate 3's camera and controls.

No game files come with this mod. Setup builds what it needs from YOUR copies
of both games.

What you need
-------------
- GTA San Andreas for PC, version 1.0 US (downgrade newer copies first with
  gtasa-open-downgrader: https://github.com/xxanqw/gtasa-open-downgrader),
  with an ASI loader (Silent's ASI Loader).
- Skate 3 for Xbox 360, extracted: its default.xex with the game's "data"
  folder beside it. From your disc's ISO use extract-xiso
  (extract-xiso -x "Skate 3.iso"); a Games on Demand copy can be extracted
  with a tool such as Velocity. ISO files themselves don't work.
- An Xbox (XInput) controller to skate. DS4Windows works for PlayStation pads.

Install
-------
1. Double-click Setup.cmd.
2. Confirm or pick your GTA San Andreas folder.
3. Pick default.xex from your extracted Skate 3 folder. The converter copies
   out only what skating needs into <GTA>\SanAnskateas\skate-data. Your game
   folders' own files are not changed.
4. Start the game, wait for "Skate 3 is ready", press J (or L3 + R3).

Settings are in SanAnskateas.ini in the game folder. A log is written to
SanAnskateas.log there.

Uninstall: delete SanAnskateas.asi, SanAnskateas.ini and the SanAnskateas
folder from the game folder.

Credits and licences
--------------------
Major credits to these projects and their creators:
- Skate engine: skate-3-rust-engine (https://github.com/SK8-ENGINE/skate-3-rust-engine),
  as used in 2010 Rust Rewrite Mashup by chasmlol
  (https://github.com/chasmlol/2010-rust-rewrite-mashup), a fork of IW4L by
  vladtrc (Apache License 2.0; see licenses\). The converter
  (converter\iw4l-skate-convert.exe) comes from the mashup and bundles Python,
  NumPy, Pillow, UTT and the Skate 3 Custom Engine Layer, whose licences are
  in converter\licenses\.
- plugin-sdk by DK22Pac (https://github.com/DK22Pac/plugin-sdk, MIT; see
  licenses\plugin-sdk-LICENSE.txt).
- gta-reversed (https://github.com/gta-reversed/gta-reversed): reference for
  how San Andreas works.
- gtasa-open-downgrader (https://github.com/xxanqw/gtasa-open-downgrader).

Unofficial fan project, not affiliated with EA or Rockstar Games. You need to
own Skate 3 and GTA San Andreas.