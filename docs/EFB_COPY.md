# EFB copies (BP 0x52)

`gcrecomp_gp` handles both kinds of EFB copy. Pure conversion and tiling live in `src/gp/efb_copy.{h,cpp}`
(unit test: `tests/efb_copy_test.cpp`, run with `ctest`).

| Register | Use |
|---|---|
| BP 0x49 / 0x4A | source rectangle (x, y; width-1, height-1) |
| BP 0x4B | destination address in main memory (`value << 5`), texture copies only |
| BP 0x4D | destination stride in 32-byte units, per **row of tiles** |
| BP 0x4E | Y scale, `256 / scale` (9 bits); applied only when command bit 10 is set |
| BP 0x43 | pixel format; `Z24` (3) turns the copy into a depth copy |

Copy command bits:

| Bits | Meaning | Handled |
|---|---|---|
| 3, 4-6 | destination format (bit 3 is the high bit of the nibble) | yes, see below |
| 9 | half scale (2x2 box filter) | yes |
| 10 | apply the Y scale from BP 0x4E (nearest row) | yes |
| 11 | clear the EFB afterwards | yes |
| 14 | 1 = copy to the XFB (present a frame), 0 = copy to a texture in memory | yes |
| 15 | intensity formats take luma (Y = 0.257R + 0.504G + 0.098B + 16) instead of R | yes |
| 0-1, 2, 7-8, 12-13, 16 | clamp, YUV, gamma, frame-to-field, mipmap generation | ignored |

Color formats: I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8, A8, R8, G8, B8, RG8, GB8. Depth formats (Z24 mode):
Z4, Z8, Z16, Z24X8, Z8M, Z8L, Z16L. Output uses the console's tile layouts, so the existing texture path
decodes it. The byte order of RG8, GB8, Z16 and Z16L and the Z24X8 alpha byte (0xFF) follow the usual
conventions but are not verified against hardware.

Texture copies read the EFB back from the GPU on every copy. The texture cache is keyed on a hash of the guest
bytes, so entries overlapping the destination are re-decoded on next use without explicit invalidation. A
destination outside RAM is ignored with a message on stderr. Texture copies do not present a frame.
