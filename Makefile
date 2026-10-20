PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
CFLAGS = -std=c11 -Os -Wall -Wextra -Werror -Ibuild -Ivendor/notify
all: build/gt7patcher.elf
build/assets.h: generate.py web/index.html data/PPSA01317-original.xml assets/icon0.png
	python3 generate.py
build/hook.o: src/hook.c
	$(CC) -c -Os -fPIC -ffreestanding -fno-builtin -fno-stack-protector -fno-asynchronous-unwind-tables -fvisibility=hidden $< -o $@
build/hook.elf: build/hook.o src/hook.ld
	$(LD) --static -T src/hook.ld build/hook.o -o $@
build/hook.bin: build/hook.elf
	$(OBJCOPY) -O binary --only-section=.blob $< $@
build/hook_blob.h: build/hook.bin build_hook.py
	python3 build_hook.py $(NM)
build/notify.o: vendor/notify/notify.c
	$(CC) -Wall -Werror -O2 -c $< -o $@
build/gt7patcher.elf: src/main.c src/config.c src/config.h src/engine.c src/engine.h build/assets.h build/hook_blob.h build/notify.o
	$(CC) $(CFLAGS) -DPS5 src/main.c src/config.c src/engine.c build/notify.o -lSceSystemService -o $@
host: build/assets.h
	gcc $(CFLAGS) -D_DEFAULT_SOURCE src/main.c src/config.c -o build/gt7patcher-host
.PHONY: all host
