# Built as a LiteX software package; produces a standalone firmware
# ELF that links into main_ram (SDRAM at 0x40000000). The LiteX BIOS
# in BRAM serial-boots or spi-boots this image and jumps to its entry
# point (`_start` → `link_app_main`).
#
# Build artefacts:
#   link_firmware.elf  — ELF image with .text in main_ram
#   link_firmware.bin  — flat binary, ready for serialboot

include ../include/generated/variables.mak
include $(SOC_DIRECTORY)/software/common.mak

# ----- lwIP integration --------------------------------------------
LWIP_DIR = /opt/lwip
LWIP_INC = -I$(LWIP_DIR)/src/include -I$(LINK_FIRMWARE_DIRECTORY)

LWIP_SRC := $(wildcard $(LWIP_DIR)/src/core/*.c) \
            $(wildcard $(LWIP_DIR)/src/core/ipv4/*.c) \
            $(wildcard $(LWIP_DIR)/src/core/ipv6/*.c) \
            $(LWIP_DIR)/src/netif/ethernet.c

LWIP_OBJS := $(notdir $(LWIP_SRC:.c=.o))

VPATH = $(LINK_FIRMWARE_DIRECTORY) \
        $(LWIP_DIR)/src/core \
        $(LWIP_DIR)/src/core/ipv4 \
        $(LWIP_DIR)/src/core/ipv6 \
        $(LWIP_DIR)/src/netif

# ----- Our firmware objects -----------------------------------------
LINK_OBJS = \
	crt0.o      \
	main.o      \
	link.o      \
	link_audio.o\
	measurement.o\
	measurement_responder.o\
	session.o   \
	tlv.o       \
	median.o    \
	ghost_time.o\
	beat_pulse.o\
	tdm.o       \
	midi.o      \
	eurorack.o  \
	net.o       \
	netif_litex.o\
	http_server.o\
	webui.o

OBJECTS = $(LINK_OBJS) $(LWIP_OBJS)

CFLAGS += $(LWIP_INC) \
	-Wall -Os \
	-ffunction-sections -fdata-sections \
	-Wno-implicit-function-declaration \
	-Wno-incompatible-pointer-types \
	-Wno-unused-function \
	-Wno-address \
	-Wno-unused-variable \
	-Wno-unused-but-set-variable \
	-Wno-pointer-sign \
	-Wno-unused-parameter \
	-Wno-error

all: link_firmware.elf link_firmware.bin

link_firmware.elf: $(OBJECTS)
	$(CC) $(LDFLAGS) -T $(LINK_FIRMWARE_DIRECTORY)/linker.ld -N -o $@ \
		$(OBJECTS) \
		-L../libbase \
		-L../libcompiler_rt \
		-L../libc \
		-Wl,--start-group -lbase -lcompiler_rt -lc -Wl,--end-group \
		-Wl,--gc-sections \
		-Wl,-Map,$@.map

%.bin: %.elf
	$(OBJCOPY) -O binary $< $@

%.o: %.c
	$(compile)

%.o: %.S
	$(assemble)

clean:
	$(RM) *.o link_firmware.elf link_firmware.bin link_firmware.elf.map

.PHONY: all clean
