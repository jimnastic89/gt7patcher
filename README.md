# GT7Patcher 0.1

GT7Patcher is a standalone PS5 payload for Gran Turismo 7. It provides a browser interface for choosing patches and display preferences, saves those choices on the console, and applies them when GT7 starts.

Supported configuration:

- PS5 firmware 12.40
- GT7 title ID `PPSA01317`
- GT7 content version `01.710.000`
- Web interface on port `7210`

The payload includes offline mode, Debug Settings, Quick Store Rotation, Power Pack and vehicle DLC unlock patches. It also supports language and unit preferences. Only GT7 receives patch writes.

## Run

1. Fully close GT7 and load `GT7Patcher-v0.1.elf` with a PS5 ELF loader.
2. Open the **GT7Patcher** shortcut installed on the PS5 home screen, or browse to `http://PS5_IP:7210/`.
3. Choose the patches and display preferences, then save them.
4. Start the patch engine from the web interface unless automatic start is enabled.
5. Launch GT7.

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

The tests require Python 3, GCC, and Capstone 5.0.7. `tests/test_selector.py` also expects the clean GT7 executable and source patch XML in the adjacent workspace paths used by the project.

## License and acknowledgements

GT7Patcher is licensed under GPL-3.0-only. See [LICENSE](LICENSE).

The GT7 patch definitions were AI-generated and released without author attribution. They build on work by High-Speed007 and PSVIETHOA, with testing credited to Dr.Yenyen. The notification helper is from `ps5-payload-dev/elfldr`; the physical-memory write method is based on `illusionyy/ps-patch-system`; and firmware offsets are derived from ShadowMountPlus by drakmor. Full notices and retained upstream licenses are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and `vendor/`.
