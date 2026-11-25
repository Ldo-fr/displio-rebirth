CC      = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
CFLAGS  = -mcpu=cortex-m0 -mthumb -Os -ffreestanding -nostdlib -Wall -Wextra \
          -ffunction-sections -fno-common
BUILD   = ../build/probe

all: $(BUILD)/probe.bin

$(BUILD)/probe.elf: probe.c link.ld
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -T link.ld -Wl,--gc-sections -Wl,-Map=$(BUILD)/probe.map -o $@ probe.c

$(BUILD)/probe.bin: $(BUILD)/probe.elf
	$(OBJCOPY) -O binary $< $@
	arm-none-eabi-size $<

clean:
	rm -rf $(BUILD)

