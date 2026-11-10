# Rev-limit investigation

Date: 2026-11-10

## Update (session 2): physics limiter located

The physics limiter has been found and verified. It is a `u16` maximum-RPM
field inside the engine **spec** object:

- vehicle `0xec059a9070` -> `[+0x18]` engine `0xec05a6cb78` (vtable
  `0x57d48f8`) -> `[+0x28]` spec `0xec05a68018`.
- `spec+0x298` = max RPM (**physics limiter**). Viper = 7000.
- `spec+0x29a` = idle RPM (800).
- `spec+0x3ae..+0x3b8` = red-zone values, display only.

Poking `spec+0x298` to 8000 moved the in-game limiter (user-confirmed), so
this single field is the enforcement point. The accessor is
`getMaxRpm @ 0x5417c0` (`mov rax,[rdi+0x28]; movzx eax,word [rax+0x298]`),
reached only through a vtable (no direct callers).

Arena layout is stable across game restarts (same addresses after a
crash/relaunch). Preallocated pools: 22 vehicle slots at `0xec059a9070`
stride `0x1920`; 22 engine slots at `0xec05a6cb78` stride `0x4f50`. Only the
active car's slot 0 is populated.

**Open question for implementation:** no global/data pointer exists to this
sim graph (proven by a full scan of the code module, library module and the
`0xec` arena), so the override must reach it through the bridge-sampled car
object (`car_implementation` / `car_expanded` / `car_style`) or a runtime
scan. The exact hop chain from the bridge's car object to the engine spec is
still being pinned down read-only (`work/_walk.py`).

**Crash history:** GT7 was brought down three times by the optional
observation trace hooks (`/api/live/hooks?install=1`, mode-9 hooks that call
a C routine from hot in-game paths). Those hooks are a manual diagnostic that
the web UI never calls; they are **not** installed at launch. Decision: do
**not** use code hooks for the rev-limit feature. Implement it as a
payload-side (or bridge-side) direct write of `spec+0x298`, re-applied on car
change, with no code hook.

## Objective

Add a Live Settings control which increases a car's rev limit by a requested
RPM amount, without altering the current engine RPM. The desired behaviour is
for the change to affect the actual limiter and, where derived from it, the
tachometer red zone and maximum scale.

The initial question remains open: whether this can be stored as a native
per-car save setting, or must remain an active runtime override.

## Reference observations

The user supplied these in-race observations:

- Dodge Viper: red zone begins at approximately 6,500 RPM; limiter is
  approximately 7,000 RPM; tachometer scale ends at 8,000 RPM.
- Volkswagen 1200: red zone is approximately 3,800--4,000 RPM; limiter is
  approximately 4,000 RPM.

The 7,000 and 4,000 figures were observed from an analogue tachometer. The
6,500 and 8,000 Viper figures are not assumed to be separate authoritative
engine parameters: they may be derived display values.

## Confirmed findings

### Existing Live Settings support

There is no active rev-limit feature or dormant native rev-limit setter in the
current repository. The existing bridge supports garage-backed wheel settings
and engine swaps, but no RPM setter.

The live bridge's car pointers are only snapshots of temporary `Car` wrappers.
`sample_car()` destroys the wrapper after sampling. Those addresses can be
reused, so they must not be used as stable in-race physics pointers.

### Debug Setting names are not usable

The retail executable contains these registered names:

- `getDebugSettingAddMaxRPM`
- `setDebugSettingAddMaxRPM`

They are not a working public setting in this build:

- `setDebugSettingAddMaxRPM` resolves to `0x4003d0`, containing only `ret`.
- `getDebugSettingAddMaxRPM` resolves to `0x1ee9730`, which writes a constant
  zero to its output and returns.

For one temporary test, the getter's zero was changed to `2000`. A reversible
counter probe then observed zero calls while the Viper race was running. The
getter was restored to its original zero-return implementation. No Debug
Settings UI option was added or relied upon.

### HUD and cached car values are not enforcement

Several exact-value candidates were found in heap objects and in UI-related
data. Writes to those candidates did not change the Viper's 7,000 RPM limiter
or the Volkswagen's 4,000 RPM limiter. They are therefore rejected as limiter
controls.

The executable exposes UI-facing bindings named `engineRPM`, `engineRPMMax`,
and `engineRedRPM`. These identify an engine-information interface used by
presentation code, but do not yet identify the physics limiter's owner or a
setter. Their getters were not called during a short in-race probe while the
HUD was hidden/paused, so they are not currently a viable live path.

## Safety and state

- No repository source files were modified during this investigation before
  this document was created.
- No payload build was made or installed.
- The only game-code modification was the temporary six-byte
  `getDebugSettingAddMaxRPM` getter test. It was restored and verified as
  `66 c7 07 00 00 c3`.
- Temporary probes on UI getter wrappers were restored byte-for-byte.
- No current-RPM field was intentionally modified.

## Current conclusion

There is not enough evidence to claim that rev limit can be saved directly in
garage data, nor enough evidence to implement a safe active override. The
obvious debug setting is a retail stub, and all tested exact-value heap fields
were presentation, cached, generic lookup, or otherwise non-enforcing data.

## Remaining work

1. Locate the in-race physics engine object, independently of HUD bindings and
   temporary garage `Car` wrappers.
2. Identify the limiter calculation or virtual method that consumes that
   object's effective maximum RPM.
3. Trace the value back to its car-parameter source and establish whether it
   originates in serialised garage data or only in constructed race state.
4. Only after that, implement either:
   - a native garage-data setter, if the value has a persistent supported
     representation; or
   - a narrowly scoped bridge override applied to the effective limiter and
     derived tachometer inputs, if it does not.
5. Verify with at least two cars of different native limits and confirm that
   engine RPM itself is never written.

## Result (verified end-to-end)

Decision: runtime override (option 4b). The physics limiter is the engine spec
u16 at `spec+0x298`, reachable only from the fixed, version-locked arena anchor
(`vehicle 0xec059a9070 -> [+0x18] engine -> [+0x28] spec`). It is NOT
persistable in save data: the spec is rebuilt when a car is (re)constructed and
no global/data pointer references the graph, so the override is re-applied by a
1 Hz tick in the payload loop. Only `spec+0x298` is ever written; live engine RPM
and the display-only redline (`spec+0x3ae`) are never touched.

Verified on 2026-11-10 (PPSA01317 v01.710.000), payload 0.35.26-11-10_1122:

| step        | car     | raw spec+0x298                        | in-game                        |
|-------------|---------|---------------------------------------|--------------------------------|
| read        | Viper   | 7000 (idle 800, red 6300)             | -                              |
| +1000       | Viper   | 8000                                  | limiter moved (user confirmed) |
| change car  | VW 1200 | native 4000 -> tick re-applied 5000   | ~5000 by engine sound          |
| reset (0)   | VW 1200 | 4000                                  | back to native                 |

- The re-apply tick followed the car change automatically (no re-submit).
- Idle and redline fields were unchanged throughout.
- Two wiring bugs were fixed en route: the status JSON root brace (dropped when
  the `revLimit` object was added) and an over-strict pid guard in
  `gt7_rev_limit_set` (the page tracks the limiter sample, not the pid).

## Correction: the red zone is NOT spec+0x3ae

The earlier note "`spec+0x3ae..+0x3b8` = red-zone values, display only" is
**wrong**. Writing those four u16 fields (verified, they persist) does **not**
move the tachometer red zone, so they are not what the gauge draws from.

What the tach actually matches is the car's **display record**, mirrored in the
riding car's `expanded` object at `expanded+0x8f8`:

```
+0x8fe = idle (800)   +0x900 = tach max / scale   +0x902 = red zone
```

For the VW this is `4000 / 3800`, which matches the observed face (scale 4000,
red 3800); for the Viper the observed face is scale 8000 / red ~6500 while the
physics spec holds 7000 / 6300 -- i.e. the display record and the physics spec
are **independent**, and the feature currently only moves the physics spec.

This record is not writable in place: a direct write to `expanded+0x902`
succeeds but is reverted by the game within ~3 s (the pointer itself is stable
across frames). The same 24-byte record appears as ~23 copies in the heap and
arena (e.g. `0xec05999190`, `0xec06b03068`), so the game keeps the display copy
in sync from an upstream source. Moving the red zone therefore requires finding
that source (a write-watchpoint trace on `expanded+0x902`), not writing the
display record or the physics spec.

Tooling note: `/api/live/find` does **not** URL-decode its query, so a `hex:`
pattern must be passed raw (percent-encoding the `:` silently searches for the
literal text and returns no hits).

