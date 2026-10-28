<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Fonts

Store reusable font files and generated font sources here.

- Use descriptive names that include the family, weight, size, and format when relevant.
- Document the source, license, character range, conversion command, and expected destination.
- Check Flash and internal-RAM impact before adding a font; the ESP32-C3 has no PSRAM.
- Do not commit fonts whose license does not permit redistribution.

## Pocket Pal 16 px subset

| File | Purpose |
| --- | --- |
| [`pocket_pal_chars.txt`](pocket_pal_chars.txt) | Character inventory: 133 code points, one `U+XXXX` per line with a comment column. It is both the generation input and the coverage reference. |
| [`pocket_pal_font_16.c`](pocket_pal_font_16.c) | Generated LVGL 9.5 font source, symbol `pocket_pal_font_16`. |

```c
LV_FONT_DECLARE(pocket_pal_font_16);
```

The `.c` file must be added to the consuming component's `SRCS`; declaring the
symbol does not compile or link it. Text widgets then need this font assigned on
the part that draws the text, and `CONFIG_LV_TXT_ENC_UTF8` must be enabled.

### Source and license

| Field | Value |
| --- | --- |
| Family | Noto Sans SC |
| Instance | Variable font instantiated at `wght=400` |
| License | SIL Open Font License 1.1 |
| Download | <https://github.com/google/fonts/raw/main/ofl/notosanssc/NotoSansSC%5Bwght%5D.ttf> |
| Local filename used for generation | `NotoSansSC.ttf` |
| Committed | No. Only the generated C source and the inventory are tracked. |

Noto Sans SC is a variable font: the generator instantiates weight 400 through
Pillow's `set_variation_by_axes([400])` rather than rasterizing the default
instance. The license permits redistribution, but the 17 MB source file is not
committed because the generated subset is reproducible from the URL above and
the repository only needs the rendered result.

### Coverage

The inventory holds printable ASCII `U+0020`-`U+007E`, the middle dot `U+00B7`
used as a separator, and 37 Chinese characters: the 35 unique characters of the
[string inventory](../../docs/pocket-pal-design.md#strings-inventory-fixed-set-drives-the-font-subset)
in the product requirements, plus `U+4E00` and `U+4E0B` from the cooldown
refusal, which the requirements list only inside a phrase.
[`pocket_pal_chars.txt`](pocket_pal_chars.txt) is the authoritative list, and the
Chinese README spells the sentences out for reviewers.

Anything outside this set - an arbitrary Chinese Wi-Fi SSID, a user-entered name,
full-width punctuation - has no glyph in this font and is not covered by it. The
applications that display such text need either an explicit unsupported-character
policy or a fallback font that contains the glyphs.

### Why this font is generated in-repo

LVGL's own converter, `lv_font_conv` 1.5.3 (2021), writes corrupt glyph bitmaps
when it runs on a current Node.js release. The output looks like a valid font and
the build succeeds; the defect only appears on the panel as every label showing
noise. [`tools/gen_pocket_pal_font.py`](../../tools/gen_pocket_pal_font.py)
therefore rasterizes the inventory with Pillow/FreeType and emits the LVGL
`fmt_txt` structures directly, then re-reads the file it wrote and decodes every
glyph exactly the way LVGL's PLAIN 4 bpp reader does. Any mismatch fails the
command. See the
[LVGL font format notes](../../docs/reference/shinku-chen/lvgl-font-format-and-glyph-coverage.md)
for the failure this avoids.

### Regeneration

Run from the repository root. Generation needs Pillow; the coverage check does
not.

```bash
# Coverage gate: standard library only.
python3 tools/gen_pocket_pal_font.py --check

# Regenerate the font. Any interpreter with Pillow works.
/tmp/pocket-pal-font/venv/bin/python tools/gen_pocket_pal_font.py \
  --font /tmp/pocket-pal-font/NotoSansSC.ttf --python-requires-pillow
```

`--check` scans `main/pocket_pal_*.c` and `main/pocket_pal_*.h` for non-ASCII
characters inside C string literals only - never comments or character literals -
and fails with the missing `U+XXXX` list when a new UI string needs a glyph the
inventory lacks. While no Pocket Pal screen renders text yet, it checks the PRD
inventory instead, so the gate is never vacuous. Add the code point to
`pocket_pal_chars.txt` and regenerate; never edit the generated `.c` by hand.

### Format and metrics

| Field | Value |
| --- | --- |
| Size / bit depth | 16 px, 4 bpp |
| Bitmap format | PLAIN (`bitmap_format = 0`), uncompressed, high nibble first, every glyph byte-aligned |
| cmaps | 2: `FORMAT0_TINY` for `U+0020`-`U+007E` (95 glyphs), `SPARSE_TINY` for the remaining 38 code points |
| Kerning | None; `.kern_dsc = NULL` |
| `line_height` | 19 px |
| `base_line` | 4 px |

`line_height` is a layout parameter, not the font's natural line spacing. Noto
Sans SC measures about 24 px of natural line height at 16 px; setting the font to
that value would waste a quarter of the 240 x 320 panel on leading and silently
crop text laid out for 19 px lines.

`base_line = 4` is measured from this rasterization rather than copied from the
font's `hhea`/`OS2` values. In this inventory the ink reaches 15 px above the
baseline and 5 px below it, which is 20 px and therefore one pixel more than the
19 px line box. `base_line = 4` keeps the full ascent, so no Chinese character and
no letter top is clipped, and gives up only the last row of `U+007C` VERTICAL
LINE, which Pocket Pal never displays. `base_line = 5` would instead shave the
top of `U+0060`.

### Flash impact

Measured at generation time:

| Item | Bytes |
| --- | --- |
| Glyph bitmaps (133 glyphs) | 8 519 |
| Glyph descriptors, cmaps, `unicode_list`, `lv_font_fmt_txt_dsc_t`, `lv_font_t` | 1 256 |
| **Total constant data** | **9 775 (9.5 KiB)** |

The font is `const` data that LVGL reads straight out of flash, so it costs no
static RAM and no heap at runtime. The figure is the sum of the emitted arrays
and the size of the LVGL 9.5 descriptor structures on a 32-bit target; the
compiled object measures 9 836 bytes of constant data, which confirms the
estimate within the linker's alignment padding. Regenerating with a larger
inventory grows the bitmap section roughly in proportion to the number of
additional glyphs, at about 60-90 bytes per Chinese character at this size.

