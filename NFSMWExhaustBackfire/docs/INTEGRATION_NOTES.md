# NFSMW Exhaust Backfire Integration Notes

This package contains the timing and selection core for an exhaust backfire
plugin.  It deliberately does not call undocumented `speed.exe` functions.
The game-specific layer is the `IGameBridge` implementation that will be
added after the target executable and effect/audio signatures are verified.

## Bridge contract

`IGameBridge::collectVehicles` must run on the game thread and fill one
`VehicleSnapshot` per active vehicle:

- `id` is a stable vehicle handle for the lifetime of the instance.  Do not
  use a recycled raw pointer as the ID without a generation check.
- `rpm`, `redlineRpm`, and `maxRpm` come from the vehicle's `IEngine`.  The
  scheduler treats `atRedline` and `atMaxRpm` as optional fast paths; it can
  also derive the state from the numeric values.  Normalize all three to
  absolute revolutions per minute (for example `9000.0`).  Some MW accessors
  expose kRPM (`9.0`); multiply those values by 1000 before filling the
  snapshot, or the probability curve will be wrong.
- `shiftEvent` and `gearChanged` should be one-frame pulses.  If the game only
  exposes a level, the bridge must edge-detect it before handing it to the
  controller.
- `leftExhaust` and `rightExhaust` are world-space transforms resolved from
  the current model. Position uses `x/y/z`; orientation is a normalized
  quaternion in `qx/qy/qz/qw` order. A zero quaternion is treated as identity,
  but adapters should always provide the real marker orientation. Refresh the
  transforms when a model/customization changes.

Call every exported `NFSW_Exhaust_*` function on the same game thread. Call
`NFSW_Exhaust_OnFrame` exactly once per game frame with a monotonic millisecond
timestamp; do not pass wall-clock time that can move backwards. Callbacks are
synchronous and non-reentrant: an adapter callback must not invoke any
`NFSW_Exhaust_*` function before returning.

The default configuration requires **both** markers.  This is intentional:
the requested behavior says a car without either `LEFT_EXHAUST` or
`RIGHT_EXHAUST` must not produce a flame or a backfire sound.  A bridge may
support single-marker cars by setting `requireBothMarkers=false` explicitly.

`spawnExhaustFlame` and `playBackfireAudio` are synchronous, game-thread
callbacks.  The controller calls the audio method only after the bridge
accepts the flame request. Each accepted flame then has an independent 30%
chance to produce a sound cue. Both `effectId` and `assetId` are borrowed pointers: consume or copy
them inside the callback and never retain them for asynchronous work.  Adapter
callbacks must not throw exceptions across the C boundary.

`spawnExhaustFlame` must return true if and only if it actually created or
submitted the flame. A false return keeps that scheduled shot pending for a
later frame until its 500 ms deadline. Never create a flame and then return
false, or the retry would duplicate the visual without the matching first
audio cue.

Both request structures include an explicit zero-based `side` field
(`NFSW_EXHAUST_SIDE_LEFT` or `NFSW_EXHAUST_SIDE_RIGHT`) and the same complete
marker transform. Use the side to select side-specific engine behavior; do
not infer it again from coordinate signs.

`setVanillaExhaustEnabled` is a hook for the original exhaust-backfire path.
It is a required callback, not an optional logging hook.
When `suppress_vanilla=true`, the bridge should suppress only the stock
exhaust/backfire event.  It must not disable NOS flames, turbo effects, or
unrelated vehicle particles.

Call `NFSW_Exhaust_Shutdown` on the game thread before destroying the adapter
or tearing down its live vehicles. Shutdown restores currently tracked
vehicles, stops frame processing, and releases the callback table. When a
vehicle has already disappeared from `collectVehicles`, the core only drops
its internal state and deliberately does not call the adapter with the expired
ID; the adapter must discard any per-vehicle suppression entry as part of its
own vehicle-destruction hook.

## Current executable profile

The supplied target is the verified Reforged-C5C5-868086 image documented in
`TARGET_PROFILE.md`. The 64-byte Reforged footer changes the file fingerprint
but not the executable code or virtual-address layout. Release v1.1.15 accepts
only `speed.exe`, validates the complete MD5 and SHA-256, parses the 64-byte
`NFSMWRF1` footer and verification code `868086`, verifies its original-image
digest, and then applies the PE-header and entry-signature gates.
The generic SDK entry is still not used because it does not perform this full
target validation.

Release v1.1.15 then performs a separate fail-closed device-binding gate before
registering the core or installing hooks. It requires the SMBIOS System UUID,
Windows `MachineGuid`, and system-volume serial, hashes all three in a
product-specific domain, and protects the deterministic binding payload with
Windows DPAPI in `LOCAL_MACHINE` and UI-forbidden mode. The canonical envelope
is written atomically to `SCRIPTS/NFSMWExhaustBackfire.device.json`; it exposes
only uppercase hexadecimal DPAPI ciphertext. An existing binding is decrypted
and compared in constant time. Invalid JSON, modified ciphertext, another
computer's DPAPI data, or a payload mismatch is rejected without replacing the
existing file. The binding module runs on the delayed worker thread, outside
the loader lock.

Known addresses in this exact C5C5 code layout include:

- `IEngine::GetIHandle`: `0x00404020`
- `StringToKey`: `0x00454640`
- `PVehicle` instance table: `0x009352B0`
- particle pool initialization: `0x004FF0A0`
- particle pool globals: `0x00916060`, `0x00916064`, `0x00916068`
- audio helpers whose signatures are still unverified:
  `0x004EA6B0` and `0x00713B20`

A production bridge must validate the exact basename, full executable
fingerprint, PE fields and per-function signatures before installing hooks.
It must fail closed and leave the stock game untouched when any check fails.
Do not call the particle/audio addresses above until their calling conventions
and request structures have been verified in the target process.

Release v1.1.15 hooks `GameFrameTick @ 0x00663D30` with MinHook after
validating its first 16 bytes. The detour calls the original first, then runs
`NFSW_Exhaust_OnFrame(GetTickCount64())` on the game thread. It reads
`IEngine*` and `ITransmission*` from `PVehicle + 0xF4` and `PVehicle + 0xFC`,
validates their vtables, and guards native pointer reads/getter calls. It reads
`IRenderable*` from `PVehicle + 0x108`, requires vtable `0x008ADCC8` and all
seven expected virtual-function entries, then invokes the verified `GetModel`
slot at `0x006B8F30`. The returned pointer must equal `IRenderable + 8` before
the adapter calls `eModel_GetPositionMarker @ 0x005016D0` for hashes
`LEFT_EXHAUST=0xBCF8A18B` and `RIGHT_EXHAUST=0xBD7CF15E`. Runtime v0.2.5
proved that this root-model query safely returns null; the marker hashes also
exist as an adjacent pair in the stock executable, so the unresolved part is
the owning part/Solid hierarchy rather than the hash algorithm. Runtime v0.2.6
proved that the Renderable owner field at `+0x84` is the matching
`CarRenderConn` (equivalently `IRenderable + 0x38`). Version 0.2.7 adds guarded
marker-list enumeration and forwarding-only hooks for
`CarRenderConn::OnLoaded @ 0x00750B20`,
`CarRenderConn::HandleFxEvent @ 0x00739070` and the one-shot emitter at
`0x00744980`. Version 0.2.8 added a forwarding hook for the continuous
emitter update at `0x00744A50`, restricted to emitters already mapped from the
exhaust list. It links each live vehicle through `IRenderable + 0x38`, supplies
the confirmed left/right markers to the timing core. Version 0.3.3 first used
the per-vehicle one-shot effect from attribute `0xB699B7BE`; runtime proved
that call returned successfully but produced no visible flame. Version 0.4.0
instead reads the continuous backfire key from attribute `0x60CEC115` and
drives selected exhaust emitters using the same parameter
observed in the stock update path (`0x3C088889`). It also plays the paired WAV
through a 32-voice spatial XAudio2 pool. Stock exhaust backfire is suppressed
only after both markers have been validated.

The instance table at `0x009352B0` exposes the primary `PVehicle` object. The
target constructor at `0x00689020` installs main vtable `0x008AA9D8`; the
factory path reaches it from `0x00689820` through the call at `0x00689BEF`.
The constructor initializes the Engine and Transmission fields at `+0xF4` and
`+0xFC`. Diagnostic v0.2.2 proved that the table entries have the expected
main vtable, while also proving that the older `+0x60/+0x68` interpretation
belonged to a different vehicle-related class constructed at `0x006B45C0`.
Version 1.1.15 accepts only the direct `PVehicle` view and requires
exact vtables `0x008AB6E0` and `0x008AB720` before calling any getter. Failed
slot-zero probes are logged at most once per second with all relevant pointers
and vtables.

The native v1.1.15 adapter intentionally reads slot zero only. It keeps one
vehicle state and one render connection, and lazily rebuilds that mapping when
the player changes cars or a race loads. Other table slots are AI vehicles:
they never enter the trigger core, audio backend, pulse service, SunSet light
integration, NOS edge tracking, or stock-backfire suppression. Global emitter
hooks therefore forward AI calls unchanged after a constant-size player-record
lookup. The generic C callback API remains multi-vehicle capable for external
adapters.

The engine getter mapping is likewise validated from the vtable at
`0x008AB6E0`: `0x006A03A0` is `GetRPM`, `0x006A03D0` is `GetRedline`, and
`0x006A03B0` is `GetMaxRPM`. Public SDK declarations are useful for interface
names, but every member offset and callable entry still requires direct target
binary evidence plus the runtime diagnostic gate.

## Stock exhaust and NOS separation

The exhaust update at `0x007561C0` walks `CarRenderConn + 0x3E4`; NOS uses the
separate list at `CarRenderConn + 0x3EC`. Both paths can call the continuous
emitter update at `0x00744A50`, so the diagnostic hook logs only emitter
pointers previously mapped from `+0x3E4`. Runtime logs correlate
`CarRenderConn_HandleFxEvent @ 0x00739070` event codes 3 and 4 with shifts;
the next exhaust update starts the same continuous effect on every left/right
emitter. Event 0 sets the one-shot exhaust flag in the same handler.

Version 1.1.15 suppresses events 0, 3, and 4 only for a cached vehicle owned by
the plugin after both exhaust markers have been validated. It also guards the
stock one-shot entry against a separate call for that managed connection. It
does not change particle pools, global FX flags, or the independent NOS list.

Archie's SunSet derives its stock exhaust lighting from bit `0x10` at
`CarRenderConn + 0x3F8` and a non-zero value at `CarRenderConn + 0x3D8`, not
from emitted particles. Version 1.1.15 no longer writes either field. For the
installed SunSet 1.15.2 image (timestamp `0x6A4A2A2E`, image size `0x41000`),
it validates the internal car-light collector at RVA `0xB410` and its call
site before installing an optional hook. After SunSet has populated its own
per-frame light buffer, the plugin appends one light for each exhaust emitter
whose plugin pulse is currently active. The light uses SunSet's parsed
`ExhaustLightConfig` object, so `Position`, `Direction`, `Color`,
`InnerAngle`, `OuterAngle`, `Intensity`, `Range`, and `Specular` continue to
come from `SunSetData/SpotLights.yml`. It also uses SunSet's current weather
light-power value, transforms the configured position plus the exact emitter
position by `CarRenderConn + 0x330`, and normalizes the transformed direction.
The caller performs SunSet's normal light clamp immediately afterward.

The SunSet hook is optional and fail-closed. A missing module, unsupported PE
profile, changed code signature, unreadable configuration, invalid light,
or full 512-entry buffer skips only the compatibility light; exhaust flames
and audio continue normally. The integration neither redistributes SunSet nor
depends on its YAML DLL directly.

Static analysis gives the following provisional ABI for the stock one-shot
emitter at `0x00744980` (the callee returns with `ret 0x10`):

```cpp
void __thiscall EmitOneShot(
    EmitterMatrixNode* emitter,    // ECX; local matrix begins at +0x10
    const bMatrix4* parentMatrix,  // stock path uses CarRenderConn + 0x330
    std::uint32_t effectKey,
    float intensity,              // stock path passes 1.0f
    const void* velocity);         // stock path uses CarRenderConn + 0x38
```

Version 1.1.15 keeps the one-shot hook only for stock suppression and
diagnostics. Plugin flames use the proven continuous emitter entry at
`0x00744A50`, with the effect key read from attribute `0x60CEC115`, parent
matrix at `CarRenderConn + 0x330`, parameter bits `0x3C088889`, intensity
`1.0f`, and velocity pointer from `CarRenderConn + 0x38`. The first call arms
a 550 ms pulse and the frame hook services it until expiry. A missing key,
matrix, velocity pointer, emitter ownership match, or either required marker
rejects the flame, and its paired exhaust audio is not played.

The input-controls block is read as a guarded 0x24-byte structure, but its NOS
button byte at offset `+0x21` is diagnostic only. Version 1.1.11 enumerates the
independent NOS emitter list at `CarRenderConn + 0x3EC` during `OnLoaded` and
marks NOS active only when one of those exact emitters reaches the verified
continuous update at `0x00744A50` with a valid effect. A 180 ms activity grace
window prevents low-frame-rate flicker. With no NOS emitters, no real update,
or after NOS runs dry, holding the bound key cannot create false state edges.
The hook observes and forwards the call unchanged; it never modifies NOS
particles or the input state.

## Marker and effect adapter boundary

The public SDK exposes `PVehicle::GetModel()` and
`IRenderable::GetModelHandle()`, but its `IModel` interface has no marker/node
lookup method.  Marker resolution therefore belongs in a small adapter with
an explicit result (`present` plus world transform), rather than in the timing
core.  The adapter should resolve the exact names `LEFT_EXHAUST` and
`RIGHT_EXHAUST`; treating an arbitrary exhaust-tip offset as a fallback would
violate the marker requirement.

The initial effect ID is the placeholder `exhaust_backfire`.  The effect
adapter can later map it to a verified stock resource (for example one of the
`FLAME01`-`FLAME12` records) or to a user-supplied effect without changing the
scheduler.

## Audio bank and spatial playback

`AudioBank` exposes twelve flat slots:

```
backfire.clip01 ... backfire.clip12
```

The manifest maps these IDs to the twelve existing WAV paths. After a flame is
accepted, that side independently passes a 30% audio gate before selecting one
slot with `std::uniform_int_distribution(0, 11)`. There is no group draw or
within-group draw; every file has probability 1/12. NOS edge audio intentionally
has no exhaust-flame dependency. The native
backend applies 0.999306 base gain (20% below v1.1.11), narrowed
vehicle-relative pan with direct
crossfeed, distance attenuation for non-listener vehicles, and mild distance
low-pass filtering. Version 1.1.11 converts the supplied 8-channel assets to
centered mono PCM with 14 dB of headroom plus 10 ms equal-power fade-in and
120 ms equal-power fade-out, keeps the dry signal on the direct
positional path, and sends a second copy through an audible 0.95-second XAudio2
reverb bus created at the mastering voice's actual device sample rate. The wet bus is
weighted toward rear and side channels on quad, 5.1, and 7.1 outputs; direct
dry duplication to those channels was removed. The manifest
loader requires every one of the 12 unique slots; partial and duplicate
manifests fail as a unit. At load/play time the native backend adds a 380 Hz
low-band body component at a 0.80 mix, cascades +7 dB/Q 3.0 at 1.45 kHz and
+4.5 dB/Q 4.0 at 2.65 kHz for an intentionally pronounced metal-pipe body,
plays at a 0.86 frequency ratio, and caps the direct low-pass parameter at
0.82. A 0.78 tone-output trim controls the added energy before PCM clamping,
so the change reshapes timbre instead of producing an uncontrolled volume jump.
The shipped WAV files remain unchanged.

## Timing and ABI rules

ABI version 6 replaces the audio request's old group/variant bytes with a flat
zero-based `clipIndex` plus a reserved byte while preserving the request size
and offsets. It retains version 5's flame pattern, paired-event sequence ID,
driver controls, marker quaternions, explicit request sides, NOS state, and
lifecycle API. The callback table uses
`NFSW_EXHAUST_API_VERSION`, a caller-supplied
`structSize`, and 8-byte packing.  Initialize both header fields before
registration and do not wrap `PluginApi.hpp` in a different pack pragma.

In paired shift mode, one categorical decision is made per physical gear-change
cycle. Upshifts use simultaneous sides at 22.5%, sequential sides at 17.5%, or
no event at 60%. Deliberate downshifts above the global 4000 RPM gate use an
independent fixed 80% event probability, then preserve the 45:35 relative
weight when choosing simultaneous versus sequential output.
Simultaneous mode may submit one left and one right request in the same frame;
sequential mode randomizes the first side and schedules the other 300 ms later.
Downshifts no longer require gas, brake, or handbrake input, so a deliberate
downshift while coasting remains eligible.
The legacy 4-8/2-4 burst scheduler remains available only when
`paired_shift_mode=false`.

The native v1.1.15 adapter preserves that side-level behavior for cars with
fewer than four mapped exhaust outlets. With four or more outlets, a
simultaneous pair starts one randomly selected outlet on each side immediately
and starts every remaining outlet 300 ms later. A sequential pair shuffles the
outlets on each side, then starts one outlet every 300 ms while alternating
between the first and second side; after one side is exhausted, the remaining
outlets on the other side follow in order. Audio remains one probability check
per accepted side request rather than one check per physical outlet.

After one continuous second at the limiter, each sustained attempt has a fixed
20% probability. Real NOS emitter start and stop edges use 30% and 35%
respectively; successful NOS edges choose simultaneous or 300 ms sequential
paired audio with equal probability in the shipped configuration.

## Verification checklist for the native bridge

1. Confirm the executable profile before installing any hook.
2. Confirm both marker lookups on a stock car and on a car with no exhaust
   markers; the latter must generate neither request.
3. Verify that the stock suppression hook affects exhaust backfire only and
   leaves NOS unchanged.
4. Capture several shifts and verify each decision is exactly one of
   `SIMULTANEOUS`, `SEQUENTIAL`, or `NONE`; accepted flame logs must use
   `mode=PULSE duration=550ms`, and every accepted side must have one audio log.
5. Hold the engine at the limit for one second without shifting and verify the
   fixed 20% sustained attempts stop as soon as RPM leaves the threshold.
6. Use NOS normally and verify only its real emitter START and END edges can
   produce paired audio. Then hold the NOS key on a car without NOS and after
   exhausting NOS; verify there is no audio and stock NOS visuals are intact.
7. Verify `LIVE_AUDIO` gain, pan, low-pass, and distance values change with
   vehicle position while the player vehicle remains at full distance gain.
