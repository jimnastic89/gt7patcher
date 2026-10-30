# GT7Patcher

GT7Patcher is a payload for applying patches to the PS5 version of Gran Turismo 7 at runtime without modifying the game files. It provides a web interface for selecting patches, and can be configured to apply saved patches automatically when the payload starts.

Included patches as of version 0.3:

- Offline mode
- Unlock debug Settings
- Quick Used and Legend Store Rotation
- Power Pack and vehicle DLC unlock
- Language and unit preferences
- Additional wheel offset options in GT Auto
- File cleaner to remove old offline patches from game files

Configuration is via the web interface on port `7210`, which can be access via a shortcut installed to the PS5's Media tab, or from any device on the same LAN.

Patches only work on a clean version of `PPSA01317` version `01.710.000`. It has only been tested on firmware 12.40, but should work on other firmwares.

<img width="892" height="907" alt="1-Patches" src="https://github.com/user-attachments/assets/cdbe21f1-24ac-44c4-bdfa-bfac65f84a0f" />
<img width="895" height="920" alt="2-Lang" src="https://github.com/user-attachments/assets/9199c6ef-b054-4ef6-8821-367827e8d546" />
<img width="886" height="973" alt="3-Help" src="https://github.com/user-attachments/assets/c2a8bf26-4f5c-4ee0-b9ac-55f2f1b5871f" />



## Run

1. Load `GT7Patcher.elf` with a PS5 ELF loader.
2. Open the **GT7Patcher** shortcut installed in the media tab, or browse to `http://PS5_IP:7210/`.
3. Choose the patches and display preferences, then save.
4. Start the patch engine from the web interface.
5. Optional: turn on automatic start to start the engine with the saved settings next time the payload runs.
6. Launch GT7.

The payload and patch engine must be running before GT7 launches. The saved settings and home-screen shortcut persist across reboots, but the payload must be loaded again after each console reboot. Do not run another patch engine at the same time.

## Clean older game files

In **Help**, choose or drop the listed files from a supported older offline-patched PPSA01317 version 01.710.000 dump. Files can be added in several batches from different directories. Only newly added or replaced files are verified; earlier verification results are retained until cleared. The picker filters extensions; exact filenames are checked after selection. Unrelated files are ignored. The converter verifies their SHA-256 hashes and enables **Unpatch files** when it finds at least one supported older file. It warns before generating an incomplete set. Download the generated ZIP and extract its contents over the game folder while GT7 is closed. Keep the original files backed up.

Conversion runs locally in a browser worker. Game files are never uploaded to the payload and the original files are never overwritten. The ZIP contains only verified older files that can be reconstructed, using paths relative to the game root. The three unchanged files (`ampr_emu.index`, `fakelib/libSceAmpr.sprx`, and `fakelib/libScePlayGo.sprx`) are omitted from the converter; leave their existing copies in the installed game. Unknown or modified inputs are rejected. The four additional backport files are optional and are not generated when missing. A desktop browser is recommended for file selection and downloading the roughly 160 MB ZIP.

The compact correction manifest is in `data/cleaner.json`. To regenerate it from private local reference files:

```sh
python3 build_cleaner.py --reference /path/to/GRAN_TURISMO_7_Clean_Backport --old /path/to/PPSA01317-app
```

## Build

Install [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) at `/opt/ps5-payload-sdk`, then run from WSL or Linux:

```sh
make all
```

The payload is written to `build/gt7patcher.elf`.

To build the host preview, run the regression tests, and create a release archive:

```sh
make host
python3 tests/test_selector.py
python3 tests/test_hook.py
python3 tests/test_memory.py
python3 package.py
```

The tests require Python 3, GCC, and Capstone 5.0.7. `tests/test_selector.py` also expects the clean GT7 executable and source patch XML. When the GT7 workspace is not the repository's parent directory, set `GT7_WORKSPACE` to its path.

## License and acknowledgements

GT7Patcher is licensed under GPL-3.0-only. See [LICENSE](LICENSE).

The GT7 patch definitions were AI-generated and released without author attribution. They build on work by High-Speed007 and PSVIETHOA, with testing credited to Dr.Yenyen. The notification helper is from `ps5-payload-dev/elfldr`; the physical-memory write method is based on `illusionyy/ps-patch-system`; and firmware offsets are derived from ShadowMountPlus by drakmor. Full notices and retained upstream licenses are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and `vendor/`.
