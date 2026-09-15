# NFSMW MultiGear tests

The test executables are independent of the ASI entry point and do not load or
modify the game. Build and run all of them from the `NFSMWMultiGear` directory:

```powershell
cmake -S . -B build-x86 -G "Visual Studio 17 2022" -A Win32 -DBUILD_TESTING=ON
cmake --build build-x86 --config Release
ctest --test-dir build-x86 -C Release --output-on-failure
```

`GearConfigTests` covers bare forward-gear counts, raw `slots=` counts, exact matching,
missing-file behavior, duplicate replacement, malformed-line rejection, and
the warning emitted when ratios for requested higher gears are absent.

`GameAccessTests` checks the CryptoAPI MD5 result against the standard `abc`
test vector, covers valid and invalid `SafeRead` addresses, verifies writable
and read-only behavior for `SafeWrite`, verifies that only the stamped Reforged
profile is accepted for hooks, and checks both possible inline-data
offsets selected by metadata bit 15.

`GearConfigPhase4Tests` covers 8-12-gear forms, paired ratio/power values,
legacy multiplier defaults, malformed pair rejection, inverse ratio/efficiency
proxy math, and runtime multiplier isolation. The isolation cases verify that
reverse, neutral, and gears 1-7 remain at base efficiency, a multiplier applies
only to its matching current high gear at a verified live-power call, static or
rejected callers remain neutral, failed or mismatched current-gear reads remain
neutral, and transitions through every high gear leave no multiplied value
behind when returning to low gears.

`HudGearDisplayTests` covers the vanilla numeric mapping, the exact CustomHud
10/11/12 glyph mapping, right-anchored glyph placement, and x86 CALL encoding.

`StartupGateTests` covers the structured `868086` footer, truncated and altered
footer rejection, deterministic fixed-source hardware IDs, the schema-2
DPAPI-local-machine binding envelope and uppercase hexadecimal ciphertext,
tamper rejection, v1 plaintext migration, current-machine identity acquisition,
first creation, repeat verification, mismatch preservation, the real stamped
executable, and a single-byte executable tamper.

`DiagnosticLogTests` verifies the formal build's no-log mode: opening and
writing the diagnostic logger succeeds as a no-op and does not create a file.

A warning keeps parsing and diagnostics separate from game policy. The safe
runtime policy is to skip an entry whose `ratioCount` is less than
`RequiredRatioCount()`; the tests do not assume any ratio extrapolation.
