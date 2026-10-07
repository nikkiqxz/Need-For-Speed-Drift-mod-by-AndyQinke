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
exhaust/backfire event. It must not disable turbo effects or unrelated vehicle
particles.

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
but not the executable code or virtual-address layout. Release v1.1.26 accepts
only `speed.exe`, validates the complete MD5 and SHA-256, parses the 64-byte
`NFSMWRF1` footer and verification code `868086`, verifies its original-image
digest, and then applies the PE-header and entry-signature gates.
The generic SDK entry is still not used because it does not perform this full
target validation.

Release v1.1.26 then performs a separate fail-closed device-binding gate before
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

Release v1.1.26 hooks `GameFrameTick @ 0x00663D30` with MinHook after
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
instead verifies the continuous backfire key from attribute `0x60CEC115` and
drives selected exhaust emitters with the dedicated NFSMS-installed
`fxcar_backfire_flame_slim_v1` key (`0x92C75354`) using the same parameter
   observed in the stock update path (`0x3C088889`). Release 1.1.28 renders paired
   WAVs through a 32-slot software voice pool into the game's active PCM renderer
   at `0x0082049E`; it no longer creates an XAudio2 device. Stock exhaust backfire is suppressed
only after both markers have been validated.

The instance table at `0x009352B0` exposes the primary `PVehicle` object. The
target constructor at `0x00689020` installs main vtable `0x008AA9D8`; the
factory path reaches it from `0x00689820` through the call at `0x00689BEF`.
The constructor initializes the Engine and Transmission fields at `+0xF4` and
`+0xFC`. Diagnostic v0.2.2 proved that the table entries have the expected
main vtable, while also proving that the older `+0x60/+0x68` interpretation
belonged to a different vehicle-related class constructed at `0x006B45C0`.
Version 1.1.26 accepts only the direct `PVehicle` view and requires
exact vtables `0x008AB6E0` and `0x008AB720` before calling any getter. Failed
slot-zero probes are logged at most once per second with all relevant pointers
and vtables.

The native v1.1.26 adapter intentionally reads slot zero only. It keeps one
vehicle state and one render connection, and lazily rebuilds that mapping when
the player changes cars or a race loads. Other table slots are AI vehicles:
they never enter the trigger core, audio backend, pulse service, SunSet light
integration, or stock-backfire suppression. Global emitter
hooks therefore forward AI calls unchanged after a constant-size player-record
lookup. The generic C callback API remains multi-vehicle capable for external
adapters.

Version 1.1.26 caches the validated exhaust-emitter ownership set once per
game frame. Pulse servicing, SunSet light injection, and emitter hooks reuse
that set instead of walking the intrusive emitter list once per outlet. The
connection vtable, render-info identity, and current list membership are still
validated, so this optimization does not remove stale-pointer protection.

The engine getter mapping is likewise validated from the vtable at
`0x008AB6E0`: `0x006A03A0` is `GetRPM`, `0x006A03D0` is `GetRedline`, and
`0x006A03B0` is `GetMaxRPM`. Public SDK declarations are useful for interface
names, but every member offset and callable entry still requires direct target
binary evidence plus the runtime diagnostic gate.

## Stock exhaust replacement

The exhaust update at `0x007561C0` walks `CarRenderConn + 0x3E4`. The diagnostic
hook handles only emitter pointers previously mapped from this list. Runtime logs correlate
`CarRenderConn_HandleFxEvent @ 0x00739070` event codes 3 and 4 with shifts;
the next exhaust update starts the same continuous effect on every left/right
emitter. Event 0 sets the one-shot exhaust flag in the same handler.

Version 1.1.26 suppresses events 0, 3, and 4 only for a cached vehicle owned by
the plugin after both exhaust markers have been validated. It also guards the
stock one-shot entry against a separate call for that managed connection. It
does not change particle pools or global FX flags.

Archie's SunSet derives its stock exhaust lighting from bit `0x10` at
`CarRenderConn + 0x3F8` and a non-zero value at `CarRenderConn + 0x3D8`, not
from emitted particles. Version 1.1.26 no longer writes either field. For the
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

Version 1.1.26 keeps the one-shot hook only for stock suppression and
diagnostics. Plugin flames use the proven continuous emitter entry at
`0x00744A50`, with the dedicated effect key `0x92C75354` after validating that
attribute `0x60CEC115` resolves for the vehicle, parent
matrix at `CarRenderConn + 0x330`, parameter bits `0x3C088889`, intensity
`1.0f`, and velocity pointer from `CarRenderConn + 0x38`. Release 1.1.31 first
validates and arms the emitter, starts the corresponding audio immediately,
then begins the 770 ms visual pulse 100 ms later; the frame hook services it
until expiry. SunSet lighting follows the same delayed pulse window. A missing key,
matrix, velocity pointer, emitter ownership match, or either required marker
rejects the flame, and its paired exhaust audio is not played.

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

`AudioBank` exposes sixteen flat slots:

```
backfire.clip01 ... backfire.clip16
```

The manifest maps these IDs to sixteen WAV paths. Version 1.1.26 parses the same
manifest in both the controller and native backend, then preloads the exact
relative paths declared there instead of relying on compiled-in filenames.
After a flame is accepted, that side independently passes an event-specific
audio gate: 50% for upshifts, 70% for downshifts, and 30% for sustained-limit
events. The first accepted side in one paired sequence creates one complete
batch; a second accepted side cannot duplicate that batch. Two outlets produce
1-2 cues, four produce 2-4, and six or more produce 3-6. All events share a
fresh uniform draw over all sixteen slots for every actual cue. Draws are independent,
so consecutive cues and cues inside one batch may legitimately select the same file.
Multi-cue batches choose simultaneous
or staggered playback with equal probability; the interval is 300 ms for two
outlets, 200 ms for four, and 100 ms for six or more. The native
   backend applies 1.128559 base gain (10% below v1.1.30), full-strength
vehicle-relative direct pan, distance attenuation for non-listener vehicles, and mild distance
low-pass filtering. Version 1.1.16 converts the supplied stereo assets to
centered mono PCM with a -14 dBFS peak target plus 10 ms equal-power fade-in and
120 ms equal-power fade-out, keeps the dry signal on the direct
 positional path, and adds sparse software reflection taps up to 2.30 seconds.
 The reflection path uses a 1.152 gain with prominent 110 ms and 180 ms taps
 plus progressively quieter late reflections for a separated open-air response. It is
weighted toward rear and side channels on quad, 5.1, and 7.1 outputs; direct
dry duplication to those channels was removed. The manifest
loader requires every one of the 16 unique slots; partial and duplicate
manifests fail as a unit. At load/play time the native backend adds a 380 Hz
low-band body component at a 0.60 mix, cascades +4.5 dB/Q 3.0 at 1.45 kHz and
+2.5 dB/Q 4.0 at 2.65 kHz for a retained but less homogenizing metal-pipe body,
plays at a 0.90 frequency ratio, and caps the direct low-pass parameter at
0.90. A 0.82 tone-output trim controls the added energy before PCM clamping,
then an 8 ms equal-power fade-in is applied to every clip. Main backfire clips
stay at unity through 50% of their own duration and fade to zero over the second
half. IN/OUT retain their prior 180 ms tail fade and do not inherit the main
clip's new half-duration envelope.
Each main clip is then composited with two required background assets. `IN.wav`
starts at frame zero with a 0.30 mix; `OUT.wav` starts at exactly 50% of that
main clip's frame count with a 0.24 mix. The supplied stereo sources are converted
to centered 48 kHz/16-bit mono, DC-removed, normalized to -14 dBFS, and processed
 with the same tone/envelope chain. The final composite uses one software voice, so
all three layers share identical marker pan, attenuation, low-pass, and reverb.

 Release 1.1.33 treats the main configuration, the complete audio manifest, all
sixteen main WAV files, and the IN/OUT background WAVs as required startup
resources. If any one is missing or
invalid, native initialization stops before MinHook installation and the game
 keeps its complete stock exhaust behavior. The audio submit hook reads the verified
 renderer receives the frame count as its second argument and reads channel count
 from `0x009C205F + 0x009C2060` plus sample rate at `0x009C2046`; only valid
 16-bit, 1-8 channel submissions are mixed. The fixed
 voice pool and stack channel accumulator avoid per-play and per-submit heap allocation.

 This is a final-output integration, not registration of a new ABK event in
 `NFSMixMaster`. It shares the game's DirectSound buffer, device clock, and submit
 cadence, but track-specific tunnel/occlusion metadata remains unavailable to custom WAVs.

 Release 1.1.28 initially targeted the complete streaming submit implementation at
 `0x00820310`. Static xref validation later proved that the active backend never calls
 or stores that function, explaining why queued voices remained silent. The active
 path starts at `0x00820907`, locks a DirectSound region, calls `0x0082049E` to render
 game PCM into it, and then unlocks it. Release 1.1.33 hooks that renderer, calls the
 original first, and appends plugin PCM before the caller unlocks the buffer.

 Release 1.1.32 is behavior-preserving runtime optimization. The PCM callback
 returns immediately when no software voice is active, iterates active slots in
 the same ascending order when audio is present, and caches values that only
 change with the output sample rate. Flame servicing uses an active-outlet mask,
 while SunSet integration skips inactive frames and snapshots the existing light
 buffer with one guarded read. Trigger probabilities, random draws, audio math,
 cue spacing, flame timing, and light output are unchanged.

 Release 1.1.33 closes the remaining empty-mixer race by storing the active-slot
 mask and pending-play count in one 64-bit atomic state. A zero snapshot is a
 linearizable idle decision: a play that registers before it is observed forces
 the PCM callback through the locked path, while a later play begins after that
 callback's idle decision. The uncontended idle path therefore remains lock-free.

 Release 1.1.34 adds `[BackfireToneByVehicle]`. Keys are case-insensitive
 Collection names below `pvehicle/default/cars/racers`; both whitespace and
 equals separators are accepted. Values range from 0.0 (deep/full/muffled) through
 the unchanged 1.0 baseline to 2.0 (bright/sharp/crisp). The adapter reads the
 `Attrib::Collection` at `PVehicle + 0xD4`, follows parent pointers at `+0x10` for
 at most sixteen levels, and matches the collection key at `+0x20`. Resolution is
 cached until the player's collection pointer changes. Unlisted or unresolved
 cars use 1.0. The value controls per-voice pitch, low-frequency body, and
 high-frequency transient shaping in the game PCM mixer; it is not a direct gain
 multiplier and does not change trigger or cue-selection behavior.

 Release 1.1.35 makes the mapping deliberately asymmetric. Values below 1.0
 use 135% of the previous dark/body shaping and receive a continuous loudness
 compensation curve from 1.0x at neutral to 2.0x at zero; the former dark-side
 direct and reflection trims are removed. Values above 1.0 use 275% of the
 previous bright/transient shaping, while the maximum bright-side pitch change
 increases from +8% to +20%. The exact 1.0 path remains a bit-for-bit neutral
 tone/gain bypass. Triggering, clip selection, ambience taps, and spatialization
 are unchanged.

 Release 1.1.37 promotes the sixteen audio-preview main clips to the shipping
bank and ships per-vehicle values for all 130 racer Collections. Dark-side gain
compensation now multiplies the existing curve by `1 + 0.10 * amount^2`, reaching
2.2x at 0.0 and approximately 2.0296x at 0.1 while remaining exactly 1.0x at
neutral. Bright-side high-band emphasis increases from 2.2 to 4.5, low-band cut
from 0.275 to 0.65, and the maximum pitch increase from 20% to 30%. The exact
1.0 path remains a bit-for-bit tone and gain bypass.

Release 1.1.38 reduces only the below-1.0 compensation above unity by 15%,
preserving a continuous transition into the neutral 1.0 setting. The resulting
gain multipliers are 2.02 at 0.0, about 1.8751 at 0.1, and about 1.4024 at
0.5. Below-neutral ambience is also tapered from 0.8x wet gain at 0.0 through
0.9x at 0.5 to 1.0x at 1.0. Above-neutral gain, tone shaping, and wet gain are
unchanged.

Release 1.1.39 carries the flame sequence ID through every audio cue. The
native adapter caches one tone decision per vehicle and sequence for five
seconds, so staggered cues in one batch cannot drift to different tone values.
By default, 30% of batches interpolate the configured vehicle character 50%
toward neutral 1.0; the other 70% retain the exact per-vehicle value. Both
values are configurable in `[BackfireToneVariation]` as
`batch_toward_neutral_probability` and `batch_toward_neutral_amount`, each in
the inclusive 0.0-1.0 range. The C callback ABI is version 9 and uses the
formerly reserved audio-request bytes for the sequence ID without increasing
the 72-byte request size.

Release 1.1.40 replaces the fixed 50% interpolation with an adaptive smoothstep
curve. `batch_toward_neutral_amount` is now the maximum pull and defaults to
1.0. `batch_full_neutral_distance=0.60` defines the distance at which a selected
batch can resolve fully to neutral. Remaining pull headroom drives a randomized
cross-neutral component capped by `batch_max_neutral_overshoot=0.20`; therefore
near-neutral values move less along the base curve but can cross to the other
side, while values at least 0.60 away resolve to exactly 1.0. The 30% batch gate,
sequence cache, and independent tone RNG remain unchanged.

## Timing and ABI rules

ABI version 8 retains 16-bit left/right exhaust-outlet counts and removes the
obsolete nitrous-state field, making the packed vehicle snapshot 132 bytes. It
retains the flat zero-based `clipIndex`, flame pattern, paired-event sequence ID,
driver controls, marker quaternions, explicit request sides, and lifecycle API. The callback
table uses
`NFSW_EXHAUST_API_VERSION`, a caller-supplied
`structSize`, and 8-byte packing.  Initialize both header fields before
registration and do not wrap `PluginApi.hpp` in a different pack pragma.

In paired shift mode, one categorical decision is made per physical gear-change
cycle. Upshifts use simultaneous sides at 33.75%, sequential sides at 26.25%,
or no event at 40%. Deliberate downshifts above the global 4000 RPM gate use an
independent fixed 80% event probability, then preserve the 45:35 relative
weight when choosing simultaneous versus sequential output.
Simultaneous mode may submit one left and one right request in the same frame;
sequential mode randomizes the first side and schedules the other 300 ms later.
Downshifts no longer require gas, brake, or handbrake input, so a deliberate
downshift while coasting remains eligible.
The legacy 4-8/2-4 burst scheduler remains available only when
`paired_shift_mode=false`.

The native v1.1.26 adapter preserves that side-level behavior for cars with
fewer than four mapped exhaust outlets. With four or more outlets, a
simultaneous pair starts one randomly selected outlet on each side immediately
and starts every remaining outlet 300 ms later. A sequential pair shuffles the
outlets on each side, then starts one outlet every 300 ms while alternating
between the first and second side; after one side is exhausted, the remaining
outlets on the other side follow in order. The adapter reports the true per-side
outlet counts to the core so the audio batch can scale independently of the two
logical flame-side requests.

Starting with v1.1.53, every armed flame and smoke outlet starts exclusively
from the frame service loop. Cars with fewer than four mapped outlets may start
two outlets per frame; cars with four or more mapped outlets start at most one
outlet per frame. This spreads emitter and one-shot smoke setup cost without
changing the requested order, and every outlet receives its full 770 ms visual
lifetime from its actual start frame.

Release 1.1.54 separates the exhaust flame shape from the stock NOS records.
`NFSMWExhaustBackfire-FlameShape.nfsms` copies `emcar_nos_fire` and
`emcar_nos_glow` into dedicated records, scales every Size key to 70 percent,
and raises Speed from 6 to 9. The resulting flame is 30 percent narrower and
shorter in cross-section while traveling 50 percent farther. The stock NOS
emitter group remains untouched.

Release 1.1.55 adds the separate, opt-in
`NFSMWExhaustBackfire-NOSFlameShape.nfsms`. It updates only the four Size keys
of the stock `emcar_nos_fire` and `emcar_nos_glow` records to
`0.245/0.315/0.070/0` and raises their Speed from 6 to 9, matching the complete
dedicated backfire shape. It does not change NOS Life, colors, timing,
emitter-group membership, or game trigger logic.

After one continuous second at the limiter, each sustained attempt has a fixed
40% probability, raised to 70% in neutral with at least 25% throttle.

## Verification checklist for the native bridge

1. Confirm the executable profile before installing any hook.
2. Confirm both marker lookups on a stock car and on a car with no exhaust
   markers; the latter must generate neither request.
3. Verify that the stock suppression hook affects exhaust backfire only.
4. Capture several shifts and verify each decision is exactly one of
   `SIMULTANEOUS`, `SEQUENTIAL`, or `NONE`; accepted flame logs must use
   `mode=PULSE duration=770ms`; accepted sides should produce audio at the
   configured independent 50% upshift or 70% downshift side rate.
5. Hold the engine at the limit for one second without shifting and verify the
   fixed 40% sustained attempts stop as soon as RPM leaves the threshold.
6. Verify `LIVE_AUDIO` gain, pan, low-pass, and distance values change with
   vehicle position while the player vehicle remains at full distance gain.
