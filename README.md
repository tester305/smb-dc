# Super Mario Bros. for the Sega Dreamcast

The game code is the original 6502 program assembled from the community SMB
disassembly (threecreepio/smb-disassembly).  On the Dreamcast it runs on the small NES
core in `src/` (6502 interpreter, scanline PPU, 4-channel APU), so edits you make to the
disassembly's assembly flow straight into the Dreamcast build.

## Build
1. Install cc65 + make (`sudo apt install cc65 make`) and a working KallistiOS toolchain.
2. `. /opt/toolchains/dc/kos/environ.sh`  (your KOS path)
3. `./build.sh "/path/to/Super Mario Bros. (World).nes"`  ->  `smb_dc.elf`

The ROM is only used to supply the CHR graphics, which the disassembly can't contain.
Run the ELF in Flycast, or with `make run` via dc-tool.  For a burnable disc image use
mkdcdisc: `mkdcdisc -e smb_dc.elf -o smb.cdi` (check `mkdcdisc --help` for your version).

## Controls
D-pad / stick = move, A = jump, X or B = run / fireball, Start = Start, Y = Select,
A+B+X+Y+Start = quit to BIOS.

## PC test
`make -f Makefile.host ROM=path/to/main.nes test` runs the core headless, scripts a play-through
and writes /tmp/frame_*.ppm and /tmp/out.wav.

## Notes
- Mapper 0 (NROM) only, which is all SMB needs.  No DMC channel (SMB doesn't use it).
- Output is forced to 60 Hz (VGA, or NTSC-interlaced on composite/RGB).
- Audio is generated inside the AICA stream callback, so it runs at the sound chip's own pace.
