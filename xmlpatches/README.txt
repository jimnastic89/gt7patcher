Gran Turismo 7 - Proper Offline Patch v1.0 (AI-generated, released without author attribution)

-- Credits --
We'd like to thank High-Speed007 and PSVIETHOA for providing the original offline patch.
This patch improves upon their original patch, with the main offline mechanism being derived from their work.

We'd also like to thank Dr.Yenyen for testing :)

-- Features, Fixes & Additions --
- Offline mode (derived from High-Speed007 and PSVIETHOA's work)
- Fixed Used Car Dealership and Legendary Car Dealership listings. The dealerships are now properly populated, and the car list is updated every 1 hour (with the option to change the rotation period to 1 minute).
- Fixed vehicle, suit and helmet liveries
- Fixed Scapes mode
- Fixed OOB setup
- Fixed nicknames
- Moved offline saves to the default save location for the game, so they can get picked up by tools like Garlic Save Manager (https://github.com/earthonion/garlic-savemgr)
- Added optional DLC unlocks for Power Pack DLC and pre-release DLCs
- Made Debug Menu optional
- Removed the automatic unlocking of all cars (that the original patch introduced), which broke progression

All the additional fixes that this patch provides aim to offer a complete offline experience for the game and vanilla progression.

-- Instructions --
1. Use High-Speed007's 1.710 dumped files or your own.
2. Place an original or backported eboot.bin file (with apr emu if required) in the game folder alongside any other files it might require (replacing the patched ones provided by High-Speed007 and PSVIETHOA) or wait for a reupload by backporters which will provide a compatible version to use with the xml patches below.
3. Download and inject ps-patch-system's patch-bundle-loader-prospero.elf if on PS5 or patch-bundle-loader-orbis.elf if on PS4  (https://github.com/illusionyy/ps-patch-system/releases)
4. 
  A. For PS5: Place PPSA01317.xml or PPSA01316.xml (depending on your game region) in /data/user/patches/xml_prospero
  B. For PS4: Place CUSA24769.xml or CUSA24767.xml  /data/user/patches/xml_orbis (depending on your game region)
5. Go to ps-patch-system configuration page (either through the XMB shortcut or by visiting CONSOLE_IP:23900), then go to the PS5/PS4 tab and select your version of Gran Turismo 7
6. Check the features you want to enable
7. Run the game!

-- Additional Notes --
- At the moment of writing, the latest version is 1.710.
- The saves are placed in /user/home/{user_id}/savedata_prospero/PPSA01317 (or PPSA01316 for EU region) for PS5 and /user/home/{user_id}/savedata/CUSA24769 (or CUSA24767 for EU region) for PS4
- The /data/gt7offline/shop_catalog_1710.json file is an external file created by the patch and contains the Used/Legendary Car Dealership data that is loaded in the game. It can be edited, so you can remove/change anything you like in that list.
- While the Used/Legendary Car Dealerships use values derived from the original game, some of the prices have been adjusted to avoid unnecessary grinding.
- If you want to unlock all cars, go to Debug menu in game and add all cars to your garage.
- If you want to add credits, go to Debug menu in game and add any amount of credits you want.
- The in-game language matches the system language. To change it, go to Settings -> System -> Language. There is also the  option to change the language from within the Debug Menu, but the change is not permanent.
- Why does this patch need the original eboot.bin? Because we believe that hardcoding the patches on the binary is problematic and hard to update/improve on. On top of that, it is not open source. Patching the original binaries is cleaner.
- This patch was AI-generated and released without author attribution.