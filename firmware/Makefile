# Built as a LiteX software package; LiteX provides BUILDINC_DIRECTORY,
# CPU=vexriscv, the toolchain prefix, and the standard rules. The
# Makefile is invoked from `<build>/software/link_firmware/`, with the
# directory containing this file in `SRC_DIR`.

include ../include/generated/variables.mak
include $(SOC_DIRECTORY)/software/common.mak

OBJECTS = \
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
	net.o       \
	ipv6.o      \
	http_server.o\
	webui.o

CFLAGS += -I$(LINK_FIRMWARE_DIRECTORY) -Wall -Os \
	-ffunction-sections -fdata-sections \
	-Wno-implicit-function-declaration \
	-Wno-incompatible-pointer-types \
	-Wno-unused-function

# Disable Werror — the firmware references LiteEth raw frame helpers
# that may not exist in every LiteEth revision; we tolerate the
# missing-prototype warnings rather than fail the build.

all: link_firmware.elf link_firmware.bin

link_firmware.elf: $(OBJECTS)
	$(CC) $(LDFLAGS) -T $(LINK_FIRMWARE_DIRECTORY)/linker.ld -N -o $@ \
		$(OBJECTS) \
		-L$(BUILDINC_DIRECTORY)/../libbase  -lbase \
		-L$(BUILDINC_DIRECTORY)/../libliteeth -lliteeth \
		-L$(BUILDINC_DIRECTORY)/../liblitedram -llitedram \
		-Wl,--gc-sections

%.bin: %.elf
	$(OBJCOPY) -O binary $< $@

%.o: $(LINK_FIRMWARE_DIRECTORY)/%.c
	$(compile)

clean:
	$(RM) $(OBJECTS) link_firmware.elf link_firmware.bin

.PHONY: all clean
