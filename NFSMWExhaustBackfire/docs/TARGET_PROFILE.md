# Target executable profile

The native adapter targets one executable only:

```text
Verified installation path: C:\MOSTWANTED\speed.exe
Accepted basename:          speed.exe
File size:                  6,135,872 bytes (0x5DA040)
MD5:                        FCBBAC633822546B4B5D86DB5F2F03DE
SHA-256:                    188FB0748DA83AE54126D1F674AB81D8B8B1CCF9D57311A46CC76D8BF71BCD8A
Machine:                    0x014C (x86)
PE optional-header magic:   0x010B (PE32)
Timestamp:                  0x438E4C8C
Preferred image base:       0x00400000
SizeOfImage:                0x00693000
Entry-point RVA:            0x003C4040
PE checksum:                0x005DF89D
```

The final 64 bytes must contain a valid `NFSMWRF1` version-1 footer with
verification code `868086`. The footer stores the canonical SHA-256 calculated
over the original 6,135,808 bytes with the four-byte PE checksum cleared:

```text
Canonical SHA-256: 941C755553B9D6B6C842AEBA8B7EB22600D828AF69F945B377F4146A833DC41D
```

Removing that footer and restoring the original checksum produces the C5C5
image:

```text
Original size:   6,135,808 bytes (0x5DA000)
Original MD5:    C5C5DBED69AF10A43A6037FD312384F2
Original SHA-256: 36FB81DB38469BABF15E9EDFD40959CB22609DB9005A51D4250BBC86BBCF3DD9
```

Therefore the code and fixed virtual addresses use the C5C5 layout. The
adapter must not select the separate C051 profile. Renaming the executable
does not alter its PE imports or code addresses, but the formal adapter accepts
only `speed.exe` to match the shared Reforged startup gate.

## Compatibility boundary

The game uses the ThirteenAG Ultimate ASI Loader through `dinput8.dll` and has
many existing ASIs. In particular, another plugin already hooks `0x004BA940`
and `0x006349B0`, and MultiGear owns transmission/shift hooks. This adapter
must not detour those entries. It should read the final engine and transmission
state, use a separately verified game-thread frame callback, and call the
existing particle system without changing its pools or capacity.

Every undocumented function used by the adapter needs its own expected-byte
signature or unique AOB check. If a target is already patched or its signature
does not match, initialization must stop before any write is made.
