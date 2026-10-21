# GT7Patcher 0.1

GT7Patcher is a PS5 payload that applies the following patches to Gran Turismo 7:

- Offline mode
- Debug Settings
- Quick Store Rotation
- Power Pack and vehicle DLC unlock
- Language and unit preferences

Configuration is via a web interface on port `7210`.

<img width="914" height="858" alt="Patches" src="https://github.com/user-attachments/assets/85c5921e-5ad9-4248-979a-e1c539dfdd25" />
<img width="928" height="941" alt="Lang" src="https://github.com/user-attachments/assets/a1a5d6e4-0ef3-4ba9-9027-c53441b04fa0" />


Tested on firmware 12.40. Only works on a clean version of `PPSA01317` version `01.710.000`

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
