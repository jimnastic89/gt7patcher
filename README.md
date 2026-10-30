# GT7Patcher

GT7Patcher is a payload for applying patches to the PS5 version of Gran Turismo 7 at runtime without modifying the game files. It provides a web interface for selecting patches, and can be configured to apply saved patches automatically when the payload starts.

Included patches as of version 0.3:

- Offline mode
- Unlock debug Settings
- Quick Used and Legend Store Rotation
- Power Pack and vehicle DLC unlock
- Language and unit preferences
- Additional wheel offset options in GT Auto

Configuration is via the web interface on port `7210`, which can be access via a shortcut installed to the PS5's Media tab, or from any device on the same LAN.

Patches only work on a clean version of `PPSA01317` version `01.710.000`. It has only been tested on firmware 12.40, but should work on other firmwares.

<img width="903" height="946" alt="Patches" src="https://github.com/user-attachments/assets/5b174388-419c-472a-879a-d2af4145e58c" />
<img width="903" height="941" alt="Lang" src="https://github.com/user-attachments/assets/fb4adc90-b430-482c-9fb2-62dd57a2b70a" />

## Run

1. Load `GT7Patcher.elf` with a PS5 ELF loader.
2. Open the **GT7Patcher** shortcut installed in the media tab, or browse to `http://PS5_IP:7210/`.
3. Choose the patches and display preferences, then save.
4. Start the patch engine from the web interface.
5. Optional: turn on automatic start to start the engine with the saved settings next time the payload runs.
6. Launch GT7.

The payload and patch engine must be running before GT7 launches. The saved settings and home-screen shortcut persist across reboots, but the payload must be loaded again after each console reboot. Do not run another patch engine at the same time.

To change credits while playing, select **Live Settings** in Patches, save, and
start GT7 with the patch engine running. Once your save is loaded, the Live
Settings tab shows the current balance and accepts amounts from 0 to 100,000,000.
Apply runs the game's native credit setter and currency notification on its UI
update thread, so the game counter updates without a screen transition. The web
balance refreshes every five seconds without replacing an amount being edited.

Live Settings is off by default. Unchecking it hides the tab; saving it disabled
rejects live writes and disables an existing bridge's requests. Start GT7 with
it unchecked to leave the game hook entirely uninstalled. Its memory is owned
by the game and is reclaimed when GT7 exits.

## Clean older game files

In **Help**, choose or drop the listed files from a supported older offline-patched PPSA01317 version 01.710.000 dump. Files can be added in several batches from different directories. Only newly added or replaced files are verified; earlier verification results are retained until cleared. The picker filters extensions; exact filenames are checked after selection. Unrelated files are ignored. The converter verifies their SHA-256 hashes, and enables **Unpatch files** when all seven required files are recognized. Download the generated ZIP and extract its contents over the game folder while GT7 is closed. Keep the original files backed up. Convert all required files together; replacing only eboot.bin failed in console testing.

Conversion runs locally in a browser worker. Game files are never uploaded to the payload and the original files are never overwritten. The ZIP contains only the seven required files and any recognized optional files supplied, using paths relative to the game root. Already-clean files are included unchanged. The three unchanged files (`ampr_emu.index`, `fakelib/libSceAmpr.sprx`, and `fakelib/libScePlayGo.sprx`) are omitted from the converter; leave their existing copies in the installed game. Unknown or modified inputs are rejected. The four additional backport files are optional too; missing optional files are not generated. A desktop browser is recommended for file selection and downloading the roughly 160 MB ZIP.

The release excludes the experimental Live Settings patch and tab. Its development source remains in the repository. Saved experimental selections are discarded when loaded by this release.

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
python3 tests/test_runtime_bridge.py
python3 tests/test_live_settings.py
python3 tests/test_hud_config_migration.py
node tests/test_live_settings.js
python3 package.py
```

The tests require Python 3, GCC, and Capstone 5.0.7. `tests/test_selector.py` also expects the clean GT7 executable and source patch XML. When the GT7 workspace is not the repository's parent directory, set `GT7_WORKSPACE` to its path.

## License and acknowledgements

GT7Patcher is licensed under GPL-3.0-only. See [LICENSE](LICENSE).

The GT7 patch definitions were AI-generated and released without author attribution. They build on work by High-Speed007 and PSVIETHOA, with testing credited to Dr.Yenyen. The notification helper is from `ps5-payload-dev/elfldr`; the physical-memory write method is based on `illusionyy/ps-patch-system`; and firmware offsets are derived from ShadowMountPlus by drakmor. Full notices and retained upstream licenses are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and `vendor/`.
