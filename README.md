# Ibara ARRANGE for MAME

Cave's **Ibara** (arcade, 2005) with the **ARRANGE mode** of the PlayStation 2 port (Taito, 2006) built into the
arcade program as SH-3 code, played in MAME, with the [GADGETS](https://github.com/PaulAtreides67/ibara-mame-gadgets)
info panels beside the game. It comes from the [ibara360](https://github.com/PaulAtreides67/ibara360) Xbox 360
port's ARRANGE.

**Download:** the zip under [Releases](../../releases). It holds no game data: you make the ROM set yourself.

## Requirements

- **MAME's `ibara.zip`**: the original arcade set, '06. 3. 7 ver. (set name `ibara`; `ibara_u4` CRC32 D5FB6657).
- **An ISO of the Japanese PS2 disc**: Ibara, **SLPM-66301** (the whole disc image).
- Windows and MAME for Windows (tested with 0.289; the GADGETS need its layout plugin).

## Making the ROM set

Run `make_arrange_rom.exe` from the release and give it your `ibara.zip` and the PS2 ISO (it asks for them, finds
them beside it, or takes them as arguments: `make_arrange_rom.exe <ibara.zip> <disc.iso>`). It:

1. checks your `ibara.zip` (every file's CRC32) and the disc's `IBARAR.PDM` / `IBARAR.TBL` (size and CRC32);
2. makes the PS2 pictures ARRANGE uses from the disc: the HUD (RANK gauge, boss bar, sub weapon plates), the TAITO
   logo, the ending picture, the ranking's "Arrange Mode" banner and the BULLET DESIGN pages;
3. applies the ARRANGE patch (the SH-3 code, its hooks into the arcade program, and the flash ROM's bookkeeping);
4. checks that every file it made is byte-identical to the tested set, and writes `roms\ibara.zip`.

Then start the game with `ibara_arrange.bat`, which keeps the ARRANGE set, its nvram and its save states apart from
the original `ibara`. The release's `README.txt` has the controls and settings.

## What ARRANGE adds

The PS2 ARRANGE rules (32 shot types, options and formations, sub weapons, roses, rank, 9-digit scores and its own
score table), its HUD drawn by the game itself, the TAITO logo, the PS2 ending picture, BULLET DESIGN (NORMAL /
COLOR / COLORLESS, in the service menu or held at boot) and "ARRANGE VER" on the title screen.

## Sources

- `tools/make_arrange_rom.c`: the ROM maker. Its disc reading and picture code is ibara360's `ps2extract`.
- `tools/arrpatch.h`: the patch. It holds only the patch's own bytes; every PS2 picture comes from the player's disc
  and every unchanged byte from the player's `ibara.zip`.
- `tools/build.sh`: builds the .exe with mingw-w64 (Docker) and the zlib 1.2.11 sources.
- `release/`: the files of the release zip besides the .exe and the GADGETS artwork.

## Credits

Ibara (c) 2005 Cave / AMI. The Ibara PS2 port and its ARRANGE mode (c) 2006 Taito. Fan-made and non-commercial.
Made with Claude.
