CC      = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
SIZE    = arm-none-eabi-size
CFLAGS  = -mcpu=cortex-m3 -mthumb -Os -ffreestanding -nostdlib -Wall -Wextra \
          -ffunction-sections -fdata-sections -fno-common
BUILD   = ../build/firmware
SRCS    = startup.c usb_hid.c epd.c bus_scan.c main.c

all: $(BUILD)/displio-fw.bin

$(BUILD)/displio-fw.elf: $(SRCS) stm32f103.h usb_hid.h epd.h bus_scan.h link.ld
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -T link.ld -Wl,--gc-sections -Wl,-Map=$(BUILD)/displio-fw.map -o $@ $(SRCS)

$(BUILD)/displio-fw.bin: $(BUILD)/displio-fw.elf
	$(OBJCOPY) -O binary $< $@
	$(SIZE) $<

clean:
	rm -rf $(BUILD)
