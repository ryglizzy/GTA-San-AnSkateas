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
2. Place the exe in your game folder (the one with gta_sa.exe) and run it.
3. Check all the tick boxes and click Downgrade.
4. "ASI Loader & ModLoader" is the only mod the skating needs; the others
   (SilentPatch, widescreen fix, SkyGFX...) are optional extras.

Do this yourself before Setup: Setup and build.cmd never run the downgrader.
If you skip it, Setup notices and offers to open the downgrader's page.

Already on 1.0 US (an old disc copy)? You still need the ASI Loader: the
downgrader can install it on that copy too.


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
- Skating stops by itself or acts strange: press Ctrl + J to restart the
  Skate engine (about 10 seconds). No need to restart the game.
- Black screen after Alt+Tab: download DXVK 3.1.1
  (https://github.com/doitsujin/DXVK), open its x32 folder and drop
  d3d9.dll into your GTA San Andreas folder (next to gta_sa.exe).
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
