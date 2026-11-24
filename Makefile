PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
CFLAGS = -std=c11 -Os -Wall -Wextra -Werror -Ibuild -Ivendor/notify
DEVELOPMENT_CFLAGS = -DGT7_DIAGNOSTICS
PATCH_OBJECTS = build/offline_mode.o build/debug_settings.o build/quick_store_rotation.o \
	build/unlock_power_pack.o build/unlock_vehicle_dlcs.o build/wheel_offset_custom.o \
	build/opening_movie_skip.o build/debug_environment.o build/clock_fix.o build/music_jingle_filter.o \
	build/free_photo_camera.o
all: build/gt7patcher.elf
build/%.o: patches/%.S
	$(CC) -c -nostdlib $< -o $@
build/assets.h: generate.py web/index.html web/cleaner.js web/cleaner-worker.js data/cleaner.json patches/manifest.json assets/icon0.png $(PATCH_OBJECTS)
	python3 generate.py
build/race_menu_groups.h: generate.py web/index.html
	python3 generate.py --race-menu
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
build/live_boot.o: src/live_boot.S src/live_bridge.h
	$(CC) -c $< -o $@
build/live_bridge.o: src/live_bridge.c src/live_bridge.h src/hud_tree.h src/menu_runtime.h
	$(CC) -c -Os -fPIC -ffreestanding -fno-builtin -fno-stack-protector -fno-asynchronous-unwind-tables -fvisibility=hidden $< -o $@
build/live_trace.o: src/live_trace.c src/live_bridge.h
	$(CC) -c -Os -fPIC -ffreestanding -fno-builtin -fno-stack-protector -fno-asynchronous-unwind-tables -fvisibility=hidden -mgeneral-regs-only $< -o $@
build/live_boot.elf: build/live_boot.o src/live_bridge.ld
	$(LD) --static -e live_boot_entry -T src/live_bridge.ld $< -o $@
build/live_bridge.elf: build/live_bridge.o build/live_trace.o src/live_bridge.ld
	$(LD) --static -e live_frame -T src/live_bridge.ld build/live_bridge.o build/live_trace.o -o $@
build/live_%.elf: build/live_%.o src/live_bridge.ld
	$(LD) --static -e live_frame -T src/live_bridge.ld $< -o $@
build/live_%.bin: build/live_%.elf
	$(OBJCOPY) -O binary --only-section=.blob $< $@
build/wheel_patch.o: src/wheel_patch.c src/wheel_patch.h src/menu_runtime.h src/hud_tree.h src/live_bridge.h
	$(CC) -c -Os -fPIC -ffreestanding -fno-builtin -fno-stack-protector -fno-asynchronous-unwind-tables -fvisibility=hidden $< -o $@
build/wheel_patch.elf: build/wheel_patch.o src/live_bridge.ld
	$(LD) --static -e wheel_frame -T src/live_bridge.ld $< -o $@
build/wheel_patch.bin: build/wheel_patch.elf
	$(OBJCOPY) -O binary --only-section=.blob $< $@
build/live_blob.h: build/wheel_patch.bin build/wheel_patch.elf build/live_boot.bin build/live_bridge.bin build/live_boot.elf build/live_bridge.elf build_live_bridge.py src/live_trace.c
	python3 build_live_bridge.py $(NM)
build/gt7patcher.elf: src/main.c src/config.c src/config.h src/engine.c src/career.c src/career.h src/career_import.c src/career_import.h src/wheel_patch.h src/engine.h src/engine_diagnostics.h src/live_bridge.h src/hud_tree.h build/assets.h build/race_menu_groups.h build/hook_blob.h build/live_blob.h build/notify.o
	$(CC) $(CFLAGS) $(DEVELOPMENT_CFLAGS) -DPS5 src/main.c src/config.c src/engine.c src/career.c src/career_import.c build/notify.o -lSceSystemService -lSceUserService -lSceFsInternalForVsh -o $@
host: build/assets.h
	gcc $(CFLAGS) -D_DEFAULT_SOURCE src/main.c src/config.c -o build/gt7patcher-host
build/gt7patcher-live-test.elf: src/main.c src/config.c src/engine.c src/career.c src/career_import.c src/engine_diagnostics.h src/live_bridge.h build/assets.h build/race_menu_groups.h build/hook_blob.h build/live_blob.h build/notify.o
	$(CC) $(CFLAGS) $(DEVELOPMENT_CFLAGS) -Wno-unused-function -DPS5 -DGT7_LIVE_TEST -DPORT=7212 src/main.c src/config.c src/engine.c src/career.c src/career_import.c build/notify.o -lSceSystemService -lSceUserService -lSceFsInternalForVsh -o $@
# Host test of the career merge, against decrypted saves:
# make test-career MEMORY=memory.dat OFFLINE=gt7save.bin [EXPECTED=gt7save.bin]
test-career: tests/test_career.c src/career.c src/career.h
	mkdir -p build
	gcc -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined tests/test_career.c src/career.c -o build/test_career
	build/test_career $(MEMORY) $(OFFLINE) $(EXPECTED)
.PHONY: all host release diagnostic test-career

release: build/assets.h build/race_menu_groups.h build/hook_blob.h build/live_blob.h build/notify.o
	$(CC) $(CFLAGS) -DPS5 src/main.c src/config.c src/engine.c src/career.c src/career_import.c build/notify.o -lSceSystemService -lSceUserService -lSceFsInternalForVsh -o build/gt7patcher.elf

diagnostic: all
