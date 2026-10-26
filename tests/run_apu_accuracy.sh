#!/bin/sh
# APU accuracy suite.  Needs: gcc, g++, git, python3 with numpy.
#   tests/run_apu_accuracy.sh path/to/main.nes [path/to/other_apu.c]
# 1. spec tests (pitch, length counters, envelopes, sweep muting, levels vs the exact NES mixer, aliasing)
# 2. runs SMB (presses Start, plays), records every APU register write
# 3. renders the same writes through blargg's Nes_Apu (game-music-emu) and scores this APU against it
set -e
cd "$(dirname "$0")/.."
ROM="${1:?usage: $0 main.nes [apu.c]}"
APU="${2:-$(pwd)/src/apu.c}"
OUT=build-apu-test
mkdir -p "$OUT"
[ -d "$OUT/gme" ] || git clone --depth 1 https://github.com/libgme/game-music-emu.git "$OUT/gme"
g++ -O2 -I"$OUT/gme/gme" -o "$OUT/gme_ref" tests/gme_ref.cpp \
    "$OUT/gme/gme/Nes_Apu.cpp" "$OUT/gme/gme/Nes_Oscs.cpp" "$OUT/gme/gme/Blip_Buffer.cpp"
gcc -O2 -Wall -Isrc -DAPU_SRC="\"$APU\"" -o "$OUT/apu_test" \
    tests/apu_test.c src/cpu.c src/ppu.c src/nes.c -lm
"$OUT/apu_test" "$ROM" 1800 "$OUT" || true        # non-zero exit only means a spec test failed
for m in 0:full 1:tonal 2:noise 3:pulse 4:tri; do
    "$OUT/gme_ref" "$OUT/events.bin" "${m%%:*}" 1800 "$OUT/ref_${m##*:}.raw"
done
echo
python3 tests/compare.py "$OUT"
