IBARA ARRANGE FOR MAME
======================

Cave's Ibara (arcade, 2005) with the ARRANGE mode of the PlayStation 2 port (Taito, 2006) built into the arcade
program, played in MAME, with the GADGETS info panels beside the game.

This download holds NO game data. You make the ROM set yourself from two things you own:
  1. MAME's ibara.zip: the original arcade set, '06. 3. 7 ver. (MAME set name "ibara").
  2. An ISO of the Japanese PS2 disc: Ibara (SLPM-66301).
make_arrange_rom.exe checks both, takes the PS2 pictures ARRANGE uses from the disc (the HUD, the TAITO logo,
the ending picture, the ranking banner and the BULLET DESIGN pages), applies the ARRANGE patch to the arcade set,
and checks that every file it writes is exactly the tested one.

WHAT YOU NEED
- Windows and MAME for Windows (tested with 0.289; the GADGETS need the layout plugin).
- MAME's ibara.zip (see above).
- An ISO of the Japanese PS2 disc, SLPM-66301 (the whole disc image, about 3.9 GB).

MAKING THE ROM SET (once)
1. Unzip this folder anywhere.
2. Double-click make_arrange_rom.exe. It asks for your ibara.zip, then for the PS2 ISO
   (or put ibara.zip and the .iso beside it first, and it finds them itself;
   or drag them onto it; or run: make_arrange_rom.exe <ibara.zip> <disc.iso>).
3. It writes roms\ibara.zip in this folder. That is the ARRANGE set. Your own ibara.zip is not changed.

HOW TO PLAY
1. Double-click ibara_arrange.bat.
2. The first time, a window asks where your mame.exe is: choose it. The answer is kept in mame_path.txt in this
   folder, so you are asked only once. To choose another MAME, delete mame_path.txt.
3. The very first boot shows NOW LOADING for a while (the game copies its flash ROM, as the real board does).

WHAT IS IN HERE
  make_arrange_rom.exe makes roms\ibara.zip from your files (it holds only the patch: no Cave or Taito data)
  ibara_arrange.bat    starts the game
  artwork\ibara\       the GADGETS (from the ibara360 Xbox 360 port)
  roms\, nvram\, sta\  made later: the ARRANGE set, and this game's scores, settings and save states, kept apart
                       from the original ibara's

CONTROLS (MAME's default keys: coin 5, start 1)
  A       shot; held, the ship slows down and fires 1.5 times as fast
  B       bomb
  C       SUB WEAPON CHANGE (with two or more stored)
  D       FORMATION CHANGE (with options): NORMAL, BACK, WIDE, ROLLING, SEARCH
  C + D   aim lock (press one while holding the other)
  Map C and D to buttons 3 and 4 in MAME's input menu if your pad has them elsewhere.

SERVICE MENU (F2) > DIP SWITCH SETTING
- "ranking save": scores are kept only when it is ON (a new nvram has it OFF). Choose "save" to keep it.
- "bullet design": NORMAL / COLOR / COLORLESS, the PS2's bullet graphics. Or hold C (COLOR) or D (COLORLESS)
  while the game boots, for that session only.

NOTES
- MAME reports ibara_u4 and u2 as WRONG CHECKSUMS. That is expected: the program and data are patched.
- Do not copy roms\ibara.zip into your MAME's ROM folder: there it would replace the original game. Do not
  point it at the original ibara's nvram either: the original wipes its score tables when it finds ARRANGE's
  9-digit scores (the .bat keeps them apart).
- If make_arrange_rom.exe says a file differs: it needs exactly MAME's ibara set ('06. 3. 7 ver.) and the
  Japanese disc SLPM-66301. Other versions, the older ibarao set or a damaged ISO will not work.
- If the GADGETS do not show, turn on the layout plugin in MAME's Plugins menu and choose the "GADGETS" view
  in Video Options. They read the game as the arcade ibara: ARRANGE's sub weapons and formations are not in
  them (the in-game HUD shows those).

Ibara (c) 2005 Cave / AMI. Ibara PS2 port and its ARRANGE mode (c) 2006 Taito. Fan-made and non-commercial.
Made with Claude.
