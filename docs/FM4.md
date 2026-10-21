# Forza Motorsport 4 (Xbox 360) livery support

Phase 1 covers the `.liv` livery design: decrypt, decode, edit, re-encode and re-sign.
It is ported from the standalone Python tool `fm4-livery-editor`.

Use **File > Import FM4 Livery (.liv)...** and **File > Export FM4 Livery (.liv)...**. The first
time you are asked for your own `default.pe` and your profile ID (the 16-hex-digit profile
folder name). Both are remembered. Nothing from the game is bundled; the AES and HMAC keys are
derived at runtime from your `default.pe` and the profile ID.

## Container

```text
0x00  byte[32]  HMAC-SHA256 over header + ciphertext
0x20  byte[20]  header: five big-endian u32; plaintext length = XOR of all five
0x24  byte[16]  AES-128-CBC IV (header bytes 4..19)
0x34  byte[]    ciphertext, padded with 0xbb
```

Key for kind K: `base[i] ^ xuid[(i + idx) & 7]`, where `base` is read from `default.pe`
(design AES `0xEC80` idx 5, HMAC `0xEC60` idx 1; group AES `0x1D0C4` idx 2, HMAC `0x1D0A4` idx 7).

## Plaintext (`crlv`, big-endian)

```text
0x00  "crlv", version 3
0x10  u32 car ordinal
0x29  u32 = 142 + 30 * layers + non-empty panels
0x40  six panels: front, rear, roof, left, right, wing
      panel = u32 n, u32 (n > 0), [pad byte + n * 30-byte layers], 15-byte trailer
tail  the last 12 bytes of the panel area and the first 12 of the tail hold six
      little-endian u32 per-panel layer counts
layer = u16 shape, f32 spin, f32 x, f32 y, f32 scaleX, f32 scaleY, f32 skew, ARGB (4 bytes)
```

Panels map to the editor's livery sections Front, Back, Top, Left, Right, Spoiler.
Header, trailers and tail of an imported file are kept, so an untouched file round-trips byte for byte.

## Not yet done

- 3D preview with the FM4 car model (`.carbin`, `.xds`), and unwrap overlays.
- STFS packaging for Xenia / modded consoles, and the `.fm4livery` share format.
- FM4 vinyl groups (`lvyg` / `.lvg`); importing one reports a clear error.
- Shape sets: FM4 families `j`, `k`, `l` (ids 1001-1240) have no geometry in the editor yet, so those
  layers import and export with their ids intact but draw nothing.
- Panel axis and rotation conventions follow the raw FM4 values; they still need an in-game check.
- FM4 has no masks or raster logos; those layers are skipped on export and reported.

## Other Xbox 360 titles (FM2, FM3, Horizon 1, Horizon 2 on 360)

Not supported, because nothing about their save layout has been verified: there are no sample
files and no documentation in the FM4 tool this was ported from. The importer only accepts `crlv`
version 3 and reports the version it found otherwise, so another title's file is refused instead
of being misread. FM4 is the only Xbox 360 title confirmed to match this format. To add the others,
send an unmodified save per title (the `.liv` plus your `default.pe` and profile ID).
