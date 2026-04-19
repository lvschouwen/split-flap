CC	:= avr-gcc
LD	:= avr-ld
OBJCOPY	:= avr-objcopy
OBJDUMP	:= avr-objdump
SIZE	:= avr-size

TARGET = twiboot
SOURCE = $(wildcard *.c)

# Build for the Arduino Nano (ATmega328p @ 16MHz crystal) by default.
# Upstream twiboot defaults to attiny85 @ 8MHz internal RC.
MCU = atmega328p

# Default programmer: USBasp. Override with AVRDUDE_PROG=... make install if
# you're using Arduino-as-ISP (-c stk500v1 -P /dev/ttyACM0 -b 19200) or a
# different setup.
AVRDUDE_PROG := -c usbasp

# ---------------------------------------------------------------------------

ifeq ($(MCU), atmega8)
# atmega8:
# Fuse L: 0x84 (8Mhz internal RC-Osz., 2.7V BOD)
# Fuse H: 0xda (512 words bootloader)
AVRDUDE_MCU=m8
AVRDUDE_FUSES=lfuse:w:0x84:m hfuse:w:0xda:m

BOOTLOADER_START=0x1C00
endif

ifeq ($(MCU), atmega88)
# atmega88:
# Fuse L: 0xc2 (8Mhz internal RC-Osz.)
# Fuse H: 0xdd (2.7V BOD)
# Fuse E: 0xfa (512 words bootloader)
AVRDUDE_MCU=m88
AVRDUDE_FUSES=lfuse:w:0xc2:m hfuse:w:0xdd:m efuse:w:0xfa:m

BOOTLOADER_START=0x1C00
endif

ifeq ($(MCU), atmega168)
# atmega168:
# Fuse L: 0xc2 (8Mhz internal RC-Osz.)
# Fuse H: 0xdd (2.7V BOD)
# Fuse E: 0xfa (512 words bootloader)
AVRDUDE_MCU=m168 -F
AVRDUDE_FUSES=lfuse:w:0xc2:m hfuse:w:0xdd:m efuse:w:0xfa:m

BOOTLOADER_START=0x3C00
endif

ifeq ($(MCU), atmega328p)
# atmega328p on the Arduino Nano (16MHz external crystal):
# Fuse L: 0xff (full-swing crystal, longest startup — matches what the stock
#               optiboot ships with; keeps the 16MHz crystal running)
# Fuse H: 0xdc (1024 word / 2KB bootloader section, BOOTRST set so reset vector
#               enters the bootloader first)
# Fuse E: 0xfd (BOD at 2.7V)
AVRDUDE_MCU=m328p -F
AVRDUDE_FUSES=lfuse:w:0xff:m hfuse:w:0xdc:m efuse:w:0xfd:m

BOOTLOADER_START=0x7C00
CFLAGS_TARGET=-DF_CPU=16000000ULL -DTIMER_IRQFREQ_MS=16
endif

ifeq ($(MCU), attiny85)
# attiny85:
# Fuse L: 0xe2 (8Mhz internal RC-Osz.)
# Fuse H: 0xdd (2.7V BOD)
# Fuse E: 0xfe (self programming enable)
AVRDUDE_MCU=t85
AVRDUDE_FUSES=lfuse:w:0xe2:m hfuse:w:0xdd:m efuse:w:0xfe:m

BOOTLOADER_START=0x1C00
CFLAGS_TARGET=-DUSE_CLOCKSTRETCH=1 -DVIRTUAL_BOOT_SECTION=1
endif

# ---------------------------------------------------------------------------

CFLAGS = -pipe -g -Os -mmcu=$(MCU) -Wall -fdata-sections -ffunction-sections
CFLAGS += -Wa,-adhlns=$(*F).lst -DBOOTLOADER_START=$(BOOTLOADER_START) $(CFLAGS_TARGET)
LDFLAGS = -Wl,-Map,$(@:.elf=.map),--cref,--relax,--gc-sections,--section-start=.text=$(BOOTLOADER_START)
LDFLAGS += -nostartfiles

# ---------------------------------------------------------------------------

$(TARGET): $(TARGET).elf
	@$(SIZE) -B -x --mcu=$(MCU) $<

$(TARGET).elf: $(SOURCE:.c=.o)
	@echo " Linking file:  $@"
	@$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^
	@$(OBJDUMP) -h -S $@ > $(@:.elf=.lss)
	@$(OBJCOPY) -j .text -j .data -O ihex $@ $(@:.elf=.hex)
	@$(OBJCOPY) -j .text -j .data -O binary $@ $(@:.elf=.bin)

%.o: %.c $(MAKEFILE_LIST)
	@echo " Building file: $<"
	@$(CC) $(CFLAGS) -o $@ -c $<

clean:
	rm -rf $(SOURCE:.c=.o) $(SOURCE:.c=.lst) $(addprefix $(TARGET), .elf .map .lss .hex .bin)

install: $(TARGET).elf
	avrdude $(AVRDUDE_PROG) -p $(AVRDUDE_MCU) -U flash:w:$(<:.elf=.hex)

fuses:
	avrdude $(AVRDUDE_PROG) -p $(AVRDUDE_MCU) $(patsubst %,-U %, $(AVRDUDE_FUSES))
