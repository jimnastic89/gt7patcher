PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
CFLAGS = -std=c11 -Os -Wall -Wextra -Werror -Ibuild -Ivendor/notify
PATCH_OBJECTS = build/offline_mode.o build/debug_settings.o build/quick_store_rotation.o \
	build/unlock_power_pack.o build/unlock_vehicle_dlcs.o build/wheel_offset_custom.o \
	build/opening_movie_skip.o
all: build/gt7patcher.elf
build/%.o: patches/%.S
	$(CC) -c -nostdlib $< -o $@
build/assets.h: generate.py web/index.html web/cleaner.js web/cleaner-worker.js data/cleaner.json patches/manifest.json assets/icon0.png $(PATCH_OBJECTS)
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
build/live_boot.o: src/live_boot.S
	$(CC) -c $< -o $@
build/live_bridge.o: src/live_bridge.c src/live_bridge.h
	$(CC) -c -Os -fPIC -ffreestanding -fno-builtin -fno-stack-protector -fno-asynchronous-unwind-tables -fvisibility=hidden $< -o $@
build/live_%.elf: build/live_%.o src/live_bridge.ld
	$(LD) --static -e $(if $(findstring boot,$@),live_boot_entry,live_frame) -T src/live_bridge.ld $< -o $@
build/live_%.bin: build/live_%.elf
	$(OBJCOPY) -O binary --only-section=.blob $< $@
build/live_blob.h: build/live_boot.bin build/live_bridge.bin build/live_boot.elf build/live_bridge.elf build_live_bridge.py
	python3 build_live_bridge.py $(NM)
build/gt7patcher.elf: src/main.c src/config.c src/config.h src/engine.c src/engine.h src/live_bridge.h build/assets.h build/hook_blob.h build/live_blob.h build/notify.o
	$(CC) $(CFLAGS) -DPS5 src/main.c src/config.c src/engine.c build/notify.o -lSceSystemService -lSceUserService -o $@
host: build/assets.h
	gcc $(CFLAGS) -D_DEFAULT_SOURCE src/main.c src/config.c -o build/gt7patcher-host
build/gt7patcher-live-test.elf: src/main.c src/config.c src/engine.c src/live_bridge.h build/assets.h build/hook_blob.h build/live_blob.h build/notify.o
	$(CC) $(CFLAGS) -Wno-unused-function -DPS5 -DGT7_LIVE_TEST -DPORT=7212 src/main.c src/config.c src/engine.c build/notify.o -lSceSystemService -lSceUserService -o $@
.PHONY: all host
