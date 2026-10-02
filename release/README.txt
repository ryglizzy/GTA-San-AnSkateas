GTA San Anskateas
=================

Skate 3 skating inside GTA San Andreas. Press J (or L3 + R3 on a controller)
and CJ gets on a skateboard driven by the Skate 3 engine, riding San Andreas'
own streets, with Skate 3's camera, tricks, grinds and bails.

No game files come with this mod: Setup builds what it needs from YOUR copies
of both games. You need to own GTA San Andreas (PC) and Skate 3 (Xbox 360).


Step 1: Get GTA San Andreas ready (one time)
--------------------------------------------
The mod needs the classic PC game at version 1.0 US with an ASI Loader. Not
the Definitive Edition, which is a different game.

1. Download GTA SA Open Downgrader (gtasa-open-downgrader-windows.exe):
   https://github.com/xxanqw/gtasa-open-downgrader/releases
2. Run it. It finds your Steam copy (or pick the game folder yourself).
3. Downgrade the game to 1.0 US.
4. In its mod list, install "ASI Loader & ModLoader". SilentPatch and the
   widescreen fix are good extras.

Already on 1.0 US (an old disc copy)? Only step 4 is needed: you can use the
downgrader to install the ASI Loader on it too.


Step 2: Get your Skate 3 files (one time)
-----------------------------------------
You need Skate 3 for Xbox 360, extracted to a normal folder that has a file
called default.xex and a folder called data in it.

- From your disc's ISO: use extract-xiso (https://github.com/XboxDev/extract-xiso):
      extract-xiso -x "Skate 3.iso"
  It makes a folder with default.xex and data inside.
- From a Games on Demand copy: open it with a tool such as Velocity and
  extract it.

The ISO file itself won't work, only the extracted folder.


Step 3: A controller
--------------------
Skate 3 is played with an Xbox controller. A PlayStation controller works
through DS4Windows.


Step 4: Install the mod
-----------------------
1. Unzip this download anywhere (for example your Desktop).
2. Double-click Setup.cmd.
3. Click Yes if it found your GTA San Andreas folder, or pick it yourself.
4. Pick default.xex from your Skate 3 folder (step 2). The Skate data is made
   in a few seconds.


Step 5: Play
------------
Start GTA San Andreas and load your game. When "Skate 3 is ready" appears,
press J on the keyboard, or click both sticks (L3 + R3), to get on the board.
Press it again to get off.


If something's wrong
--------------------
- Nothing happens when you press J: the game isn't 1.0 US or the ASI Loader
  is missing. Redo step 1.
- "Skate 3 failed to load": run Setup.cmd again and pick default.xex.
- "No Xbox controller found": connect it (or start DS4Windows) before playing.
- Anything else: look at SanAnskateas.log in your GTA San Andreas folder.

Settings are in SanAnskateas.ini in the game folder.

Uninstall: delete SanAnskateas.asi, SanAnskateas.ini and the SanAnskateas
folder from your GTA San Andreas folder.


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

Unofficial fan project, not affiliated with EA or Rockstar Games.
