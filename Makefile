# KallistiOS build.  Source your KOS environ.sh first, then: ./build.sh "Super Mario Bros. (World).nes"
TARGET = smb_dc.elf
OBJS = src/dc_main.o src/cpu.o src/ppu.o src/apu.o src/nes.o romdisk.o
KOS_ROMDISK_DIR = romdisk
KOS_CFLAGS += -O2 -Wall -Isrc

all: $(TARGET)

include $(KOS_BASE)/Makefile.rules

$(TARGET): $(OBJS)
	kos-cc -o $(TARGET) $(OBJS) -lm

clean:
	-rm -f $(TARGET) $(OBJS) romdisk.o romdisk.img

run: $(TARGET)
	$(KOS_LOADER) $(TARGET)

dist: $(TARGET)
	-rm -f $(OBJS) romdisk.o romdisk.img
	$(KOS_STRIP) $(TARGET)
