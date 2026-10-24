#!/bin/sh
# 1) assemble the SMB disassembly (CHR graphics come from YOUR ROM dump)
# 2) put the result in the romdisk as smb.nes
# 3) build the Dreamcast ELF if a KallistiOS environment is loaded
set -e
cd "$(dirname "$0")"
ROM="${1:-Super Mario Bros. (World).nes}"

[ -f "$ROM" ] || { echo "ROM not found: $ROM"; echo "usage: ./build.sh \"path/to/Super Mario Bros. (World).nes\""; exit 1; }
command -v ca65 >/dev/null && command -v ld65 >/dev/null || { echo "cc65 is required:  sudo apt install cc65 make"; exit 1; }

[ -d smb-disassembly ] || git clone --depth 1 https://github.com/threecreepio/smb-disassembly.git
cp "$ROM" "smb-disassembly/Super Mario Bros. (World).nes"
make -C smb-disassembly clean >/dev/null 2>&1 || true
make -C smb-disassembly

mkdir -p romdisk
cp smb-disassembly/main.nes romdisk/smb.nes
echo "romdisk/smb.nes md5: $(md5sum romdisk/smb.nes | cut -d' ' -f1)  (reference World ROM: 811b027eaf99c2def7b933c5208636de)"

if [ -n "$KOS_BASE" ]; then
    make clean
    make
    echo "Built smb_dc.elf"
else
    echo "KOS_BASE is not set - source your KallistiOS environ.sh, then run:  make"
fi
