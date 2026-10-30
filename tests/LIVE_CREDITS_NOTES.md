# Native Live Settings bridge

Implemented and console-verified for PPSA01317, content version 01.710.000.

## Native currency path

The ProgressData singleton slot is at executable RVA 0x066e9648. Its vtable
RVA is 0x053edca8 and ordinary credit member offset is +0x14c. The reader resolves
the singleton afresh and checks title, version, vtable, and root stability.
Captured heap addresses are never used by the payload.

The bridge runs these methods inside the game's UI update callback:

1. MakeCurrencyAcquiredEvent at RVA 0x035db2e0, with a zero amount and owned
   GT7Patcher label strings, records the prior balance.
2. ProgressData::setMyCredits at RVA 0x0250e420 updates the native accessor and
   its change flags.
3. AcquireCredits at RVA 0x035dbcc0 publishes the updated balance and dispatches
   the currency event. A zero delta avoids counting the desired absolute balance
   as newly earned currency.
4. The event destructor at RVA 0x0097b3c0 releases its owned data.

The native ordinary-credit cap is 100,000,000. The API and UI accept 0 through
that cap. Paid credits are not changed. The normal ProgressData credit member
is the amount exposed by the feature.

## Callback, allocation, and publication

The active MENU::mUpdateContextPS4 input-update callback is at RVA 0x0028bb20,
with a vtable slot at RVA 0x051bee08. Its arguments are the context pointer and
float delta time. Both the bootstrap and compiled handler preserve these and
always forward to the original callback. The earlier resource-clearing method
at absolute 0x01da2740 was not the frame update callback and is not hooked.

A small bootstrap occupies verified-zero executable padding at RVA 0x03f0e940.
It allocates a private 16 KiB mapping through the game's own libkernel mmap on
its UI thread. The payload writes and verifies the compiled handler and mailbox,
sets/verifies mapping permissions, writes the entry pointer, and finally
publishes a 32-bit ready state. The bootstrap enters the completed handler only
after that publication. The vtable remains pointed at the bootstrap; publishing
a high-address handler does not require changing that pointer externally.

Requests publish their complete body before the QUEUED state. The game claims
each request with CAS, validates the object and expected balance again, executes
it once, verifies the result, and acknowledges completion. Timeouts never fall
back to raw balance writes or overwrite an in-flight request. No debugger attach,
watchpoint, thread hijack, or extra helper ELF is required for normal operation.

## Optional patch

Live Settings is bit 6 and is off by default. GT7LIVEHOOK 1 in saved snapshots
separates it from legacy HUD/debug uses of that bit. The static XML line list
is empty; apply_gt7 installs the runtime bridge only when this bit is selected.
Unchecking hides the tab. Saving it off rejects writes and disables submissions
to an existing bridge. Starting GT7 with it off leaves callback and padding
untouched. Bridge memory stays owned by the game until the game exits.

## Verification

- Actual emitted bootstrap bytes executed with a mocked mmap and original
  callback: allocation occurs once, float argument survives, and the ready-state
  handshake forwards to the published handler.
- Actual compiled handler bytes executed for idle, no-change, and native-call
  requests, preserving delta time and constructor/setter/grant/destructor order.
- Sanitizer tests cover stale balance/object, disabled requests, limits,
  exactly-once execution, and native event cleanup.
- Host API, saved-selection migration, UI polling/draft preservation, existing
  memory-copy, and launch-hook tests pass.
- Console test process 140 used the isolated port-7212 diagnostic build. Native
  increase 45,950 -> 45,951 and decrease back to 45,950 were acknowledged; the
  user confirmed the in-game counter updated without a screen transition.
- The production payload on port 7210 launched process 155 with the patch off.
  Read-only inspection proved callback 0x0068bb20 unchanged and all bootstrap
  padding still zero. The write API returned HTTP 409.
- The production payload then launched process 168 with the patch on. Its own
  launch path installed the bridge and reported 45,950. Native increases and
  decreases through port 7210 succeeded and restored 45,950. Saved disable
  rejected writes, and re-enable restored availability. Original mask 111 was
  restored, with language/unit choices and autostart unchanged.

Two earlier probes failed before native currency requests: the first selected
an inactive clearing callback; a later build exited the game after activating
an allocated handler. Permission validation and game-thread publication were
added together, and the revised bridge passed the above tests. The exact cause
of that earlier exit was not independently established. Do not reuse old probe
binaries. The current production payload has been installed and tested.

Console evidence is saved in the workspace at work/live-production-verification.json.
The test-only build target gt7patcher-live-test.elf uses port 7212 and bypasses
ShellCore engine/icon setup; it is not the production release artifact.
