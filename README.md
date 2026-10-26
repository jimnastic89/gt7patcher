# GT7Patcher

GT7Patcher is a PS5 payload that applies the following patches to Gran Turismo 7:

- Offline mode
- Debug Settings
- Quick Store Rotation
- Power Pack and vehicle DLC unlock
- Language and unit preferences
- More wheel offset options in GT Auto

Configuration is via a web interface on port `7210`.

<img width="924" height="941" alt="Patches" src="https://github.com/user-attachments/assets/b5289945-b627-4bc0-9afc-58fcffb81414" />
<img width="913" height="931" alt="Lang" src="https://github.com/user-attachments/assets/3a5f5a76-21d5-4b7e-a2b0-86427692ac3c" />


Tested on firmware 12.40. Firmware is not checked at runtime. Only works on a clean version of `PPSA01317` version `01.710.000`
GT7Patcher is a standalone PS5 payload for Gran Turismo 7. It provides a browser interface for choosing patches and display preferences, saves those choices on the console, and applies them when GT7 starts.

Supported configuration:

- GT7 title ID `PPSA01317`
- GT7 content version `01.710.000`
- Web interface on port `7210`

The payload includes offline mode, Debug Settings, Quick Store Rotation, Power Pack and vehicle DLC unlock patches. It also supports language and unit preferences. An experimental wheel-offset patch displays the car's native Wide values as `Front <front> / Rear <rear>`, then adds ten progressively wider pairs by subtracting 5 through 50 from both values. Only GT7 receives patch writes. The source definition for the experimental Debug environment patch is retained, but it is not included in builds.

## Run

1. Load `GT7Patcher.elf` with a PS5 ELF loader.
2. Open the **GT7Patcher** shortcut installed in the media tab, or browse to `http://PS5_IP:7210/`.
3. Choose the patches and display preferences, then save.
4. Start the patch engine from the web interface.
5. Optional: turn on automatic start to start the engine with the saved settings next time the payload runs.
6. Launch GT7.

Keep the payload running while GT7 launches. The saved settings and home-screen shortcut persist across reboots, but the payload must be loaded again after each reboot. Do not run another patch engine at the same time.

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
