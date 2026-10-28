#!/usr/bin/env python3
"""Generate and verify the Pocket Pal 16 px LVGL font subset.

The converter hosted by the LVGL project (lv_font_conv 1.5.3) writes corrupt
glyph bitmaps when it runs on current Node.js releases, and the corruption is
invisible until every label renders as noise on the panel. This tool therefore
rasterizes the fixed character inventory with Pillow/FreeType, emits the LVGL 9
``fmt_txt`` structures itself, and then re-reads the file it just wrote and
decodes every glyph exactly the way LVGL's PLAIN 4 bpp reader does. A mismatch
fails the command instead of reaching a device.

Modes
    --check   Coverage gate. Needs only the standard library. Scans the Pocket
              Pal sources for non-ASCII code points inside C string literals and
              asserts that the committed inventory covers each one.
    generate  Rasterize the inventory and rewrite the font source. Requires
              Pillow in the interpreter and the ``--font`` source font.

The inventory (``assets/fonts/pocket_pal_chars.txt``) is both the coverage
reference and the generation input, so shrinking one content variant cannot
silently drop glyphs that another variant still displays.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
INVENTORY_PATH = ROOT / "assets" / "fonts" / "pocket_pal_chars.txt"
OUTPUT_PATH = ROOT / "assets" / "fonts" / "pocket_pal_font_16.c"
SOURCE_GLOB = "pocket_pal_*.c", "pocket_pal_*.h"
SOURCE_DIR = ROOT / "main"
PRD_PATH = ROOT / "docs" / "pocket-pal-design.zh_CN.md"

FONT_SIZE = 16
FONT_BPP = 4
SYMBOL = "pocket_pal_font_16"
GUARD = "POCKET_PAL_FONT_16"
LARGE_CONST_ATTRIBUTE = "LV_ATTRIBUTE_LARGE_CONST"

# 16 px text on a 240 x 320 panel needs a 19 px line box: LVGL lays out a label
# from line_height, and Noto Sans SC measures 24 px of natural line height at
# 16 px, which would waste a quarter of the screen on leading.
LINE_HEIGHT = 19
# base_line is measured from this rasterization, not copied from the font's
# hhea/OS2 values. The inventory's deepest descender reaches 5 px below the
# baseline while its highest ink reaches 15 px above it: 15 + 5 = 20 px of ink
# does not fit a 19 px line box, so one pixel has to go. base_line = 4 keeps the
# full ascent (no CJK or letter top is clipped) and loses only the last row of
# U+007C VERTICAL LINE, which Pocket Pal never displays. base_line = 5 would
# instead shave the top of U+0060.
BASE_LINE = 4

CMAP_FORMAT0_TINY = "LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY"
CMAP_SPARSE_TINY = "LV_FONT_FMT_TXT_CMAP_SPARSE_TINY"

ASCII_START = 0x20
ASCII_END = 0x7E


class GeneratorError(RuntimeError):
    """Raised for any condition that must fail the command."""


def display_path(path: Path) -> str:
    """Repository-relative path for messages, falling back to an absolute one."""
    try:
        return path.relative_to(ROOT).as_posix()
    except ValueError:
        return path.as_posix()


# ---------------------------------------------------------------------------
# Inventory and coverage
# ---------------------------------------------------------------------------


def load_inventory(path: Path) -> list[int]:
    """Return the sorted, de-duplicated code points listed in ``path``."""
    if not path.is_file():
        raise GeneratorError(f"inventory file not found: {display_path(path)}")
    codepoints: list[int] = []
    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        match = re.fullmatch(r"U\+([0-9A-Fa-f]{4,6})", line.split()[0])
        if not match:
            raise GeneratorError(
                f"{display_path(path)}:{number}: expected 'U+XXXX', got {raw!r}"
            )
        codepoints.append(int(match.group(1), 16))
    if not codepoints:
        raise GeneratorError(f"{display_path(path)} lists no code points")
    duplicates = sorted({cp for cp in codepoints if codepoints.count(cp) > 1})
    if duplicates:
        raise GeneratorError(
            "inventory repeats " + ", ".join(f"U+{cp:04X}" for cp in duplicates)
        )
    return sorted(codepoints)


def _decode_escape(text: str, index: int) -> tuple[bytes, int]:
    """Decode one C escape sequence starting after the backslash."""
    char = text[index]
    simple = {
        "a": 0x07, "b": 0x08, "f": 0x0C, "n": 0x0A, "r": 0x0D,
        "t": 0x09, "v": 0x0B, "\\": 0x5C, "'": 0x27, '"': 0x22, "?": 0x3F,
    }
    if char in simple:
        return bytes([simple[char]]), index + 1
    if char == "\n":  # backslash-newline: a line continuation, not a character
        return b"", index + 1
    if char in "01234567":
        end = index
        while end < len(text) and end < index + 3 and text[end] in "01234567":
            end += 1
        return bytes([int(text[index:end], 8) & 0xFF]), end
    if char == "x":
        end = index + 1
        while end < len(text) and text[end] in "0123456789abcdefABCDEF":
            end += 1
        if end == index + 1:
            raise GeneratorError("malformed \\x escape in a string literal")
        return bytes([int(text[index + 1 : end], 16) & 0xFF]), end
    if char in "uU":
        width = 4 if char == "u" else 8
        digits = text[index + 1 : index + 1 + width]
        if len(digits) != width or not all(c in "0123456789abcdefABCDEF" for c in digits):
            raise GeneratorError(f"malformed \\{char} escape in a string literal")
        return chr(int(digits, 16)).encode("utf-8"), index + 1 + width
    # Unknown escape: C treats it as the escaped character itself.
    return char.encode("utf-8"), index + 1


def extract_string_literals(source: str) -> list[bytes]:
    """Return the decoded bytes of every C string literal, ignoring comments.

    Character literals are skipped: they cannot carry UI text for LVGL, and
    treating them as strings would let ``'"'`` open a phantom literal.
    """
    literals: list[bytes] = []
    index = 0
    length = len(source)
    while index < length:
        char = source[index]
        if char == "/" and source.startswith("//", index):
            newline = source.find("\n", index)
            index = length if newline < 0 else newline + 1
        elif char == "/" and source.startswith("/*", index):
            end = source.find("*/", index + 2)
            index = length if end < 0 else end + 2
        elif char == "'":
            index += 1
            while index < length and source[index] != "'":
                index += 2 if source[index] == "\\" else 1
            index += 1
        elif char == '"':
            index += 1
            data = bytearray()
            while index < length and source[index] != '"':
                if source[index] == "\\":
                    chunk, index = _decode_escape(source, index + 1)
                    data += chunk
                else:
                    data += source[index].encode("utf-8")
                    index += 1
            index += 1
            literals.append(bytes(data))
        else:
            index += 1
    return literals


def codepoints_in_literals(literals: list[bytes]) -> set[int]:
    """Collect the non-ASCII code points of decoded C string literals.

    Printable ASCII is always in the inventory, so only the characters a subset
    could actually be missing are worth reporting.
    """
    found: set[int] = set()
    for literal in literals:
        try:
            text = literal.decode("utf-8")
        except UnicodeDecodeError:
            # A literal built from octal/hex bytes is still valid C; decode what
            # can be decoded rather than discarding the whole file.
            text = literal.decode("utf-8", errors="replace")
        found.update(ord(char) for char in text if ord(char) > 0x7F)
    return found


def pocket_pal_sources() -> list[Path]:
    paths: list[Path] = []
    for pattern in SOURCE_GLOB:
        paths.extend(sorted(SOURCE_DIR.glob(pattern)))
    return paths


def prd_inventory_codepoints() -> tuple[set[int], str]:
    """Read the PRD string inventory used before any Pocket Pal source exists."""
    if not PRD_PATH.is_file():
        raise GeneratorError(f"source document not found: {display_path(PRD_PATH)}")
    lines = PRD_PATH.read_text(encoding="utf-8").splitlines()
    heading = next(
        (n for n, line in enumerate(lines) if line.startswith("#") and "字符串清单" in line),
        None,
    )
    if heading is None:
        raise GeneratorError(
            f"{display_path(PRD_PATH)}: no heading containing '字符串清单' found"
        )

    # The inventory is the first paragraph under that heading; later paragraphs
    # in the same section discuss the font route and are not UI strings.
    block: list[str] = []
    for line in lines[heading + 1 :]:
        stripped = line.strip()
        if stripped.startswith("## "):
            break
        if not stripped:
            if block:
                break
            continue
        block.append(stripped)
    if not block:
        raise GeneratorError(
            f"{display_path(PRD_PATH)}: the string inventory section is empty"
        )
    phrases = [part.strip() for part in " ".join(block).split("\u00b7")]
    phrases = [phrase for phrase in phrases if phrase]
    found = {ord(char) for phrase in phrases for char in phrase}
    return found, f"{display_path(PRD_PATH)} inventory ({len(phrases)} strings)"


def show_codepoint(codepoint: int) -> str:
    text = f"U+{codepoint:04X}"
    if 0x20 <= codepoint <= 0x10FFFF and not 0xD800 <= codepoint <= 0xDFFF:
        try:
            return f"{text} ({chr(codepoint)})"
        except (ValueError, OverflowError):
            return text
    return text


def report_missing(missing: list[int], inventory: list[int], where: str) -> int:
    print(
        f"pocket_pal font coverage: FAIL - {len(missing)} code point(s) missing "
        f"from {display_path(INVENTORY_PATH)}",
        file=sys.stderr,
    )
    print(f"  source: {where}", file=sys.stderr)
    for codepoint in missing:
        note = ""
        if 0xE000 <= codepoint <= 0xF8FF:
            note = " (private use area: an LV_SYMBOL_* icon needs a fallback font)"
        print(f"  missing {show_codepoint(codepoint)}{note}", file=sys.stderr)
    print(
        "  fix: add the code point to the inventory and regenerate with\n"
        f"       /tmp/pocket-pal-font/venv/bin/python tools/gen_pocket_pal_font.py "
        f"--font /tmp/pocket-pal-font/NotoSansSC.ttf --python-requires-pillow",
        file=sys.stderr,
    )
    print(f"  inventory currently holds {len(inventory)} code points", file=sys.stderr)
    return 1


def run_check() -> int:
    inventory = load_inventory(INVENTORY_PATH)
    known = set(inventory)
    sources = pocket_pal_sources()

    found: set[int] = set()
    where = ""
    if sources:
        literals: list[bytes] = []
        for path in sources:
            literals.extend(extract_string_literals(path.read_text(encoding="utf-8")))
        found = codepoints_in_literals(literals)
        where = f"{len(sources)} source file(s) in {display_path(SOURCE_DIR)}/"

    if not found:
        # No Pocket Pal screen renders text yet: hold the gate on the PRD strings
        # so a regression there still fails before the UI lands.
        found, where = prd_inventory_codepoints()

    missing = sorted(codepoint for codepoint in found if codepoint not in known)
    if missing:
        return report_missing(missing, inventory, where)

    print(f"pocket_pal font coverage: PASS ({len(found)} chars)")
    print(f"  scanned {where}; inventory {display_path(INVENTORY_PATH)} "
          f"holds {len(inventory)} code points")
    return 0


# ---------------------------------------------------------------------------
# Rasterization
# ---------------------------------------------------------------------------


class Glyph:
    """One rasterized glyph, independent of any C representation."""

    __slots__ = ("codepoint", "adv_w", "width", "height", "ofs_x", "ofs_y", "levels", "bitmap_index")

    def __init__(self, codepoint: int, adv_w: int, width: int, height: int,
                 ofs_x: int, ofs_y: int, levels: list[int]) -> None:
        self.codepoint = codepoint
        self.adv_w = adv_w
        self.width = width
        self.height = height
        self.ofs_x = ofs_x
        self.ofs_y = ofs_y
        self.levels = levels
        self.bitmap_index = 0

    def packed(self) -> bytes:
        """PLAIN 4 bpp packing: high nibble first, glyph starts byte-aligned."""
        packed = bytearray()
        for offset in range(0, len(self.levels), 2):
            high = self.levels[offset]
            low = self.levels[offset + 1] if offset + 1 < len(self.levels) else 0
            packed.append((high << 4) | low)
        return bytes(packed)


def load_pillow_font(font_path: Path, weight: int):
    try:
        from PIL import ImageFont  # noqa: PLC0415 - generate mode only
    except ImportError as error:  # pragma: no cover - environment dependent
        raise GeneratorError(
            "Pillow is required for generation; run it with the font venv, e.g.\n"
            "  /tmp/pocket-pal-font/venv/bin/python tools/gen_pocket_pal_font.py "
            "--font /tmp/pocket-pal-font/NotoSansSC.ttf --python-requires-pillow"
        ) from error

    if not font_path.is_file():
        raise GeneratorError(f"source font not found: {font_path}")
    font = ImageFont.truetype(str(font_path), FONT_SIZE)
    axes = font.get_variation_axes()
    if axes:
        names = [axis.get("name") for axis in axes]
        if len(axes) != 1 or b"Weight" not in names:
            raise GeneratorError(f"unsupported variation axes in {font_path}: {names}")
        font.set_variation_by_axes([weight])
    return font


def rasterize(font, codepoint: int) -> Glyph:
    """Rasterize one code point and trim the transparent border Pillow adds."""
    character = chr(codepoint)
    mask, (origin_x, origin_y) = font.getmask2(character, mode="L")
    width, height = mask.size
    pixels = bytes(mask)
    if len(pixels) != width * height:
        raise GeneratorError(
            f"U+{codepoint:04X}: Pillow returned {len(pixels)} bytes for a "
            f"{width}x{height} mask"
        )

    rows = [
        y for y in range(height)
        if any(pixels[y * width : (y + 1) * width])
    ]
    if not rows:
        # Blank glyph (space): keep the advance, no box.
        return Glyph(codepoint, _advance(font, character), 0, 0, 0, 0, [])

    top, bottom = rows[0], rows[-1]
    columns = [
        x for x in range(width)
        if any(pixels[y * width + x] for y in range(top, bottom + 1))
    ]
    if not columns:
        raise GeneratorError(f"U+{codepoint:04X}: ink rows but no ink columns")

    left, right = columns[0], columns[-1]
    ink_w, ink_h = right - left + 1, bottom - top + 1
    levels: list[int] = []
    for y in range(top, bottom + 1):
        for x in range(left, right + 1):
            levels.append(pixels[y * width + x] >> 4)

    ascent = font.getmetrics()[0]
    ofs_x = origin_x + left
    # LVGL places the box at line_height - base_line - box_h - ofs_y, so
    # ofs_y = ascent_above_baseline_in_ink - box_h keeps the ink on the baseline.
    ofs_y = ascent - (origin_y + top) - ink_h
    return Glyph(codepoint, _advance(font, character), ink_w, ink_h, ofs_x, ofs_y, levels)


def _advance(font, character: str) -> int:
    return int(font.getlength(character) * 16 + 0.5)


def plan_glyphs(font, codepoints: list[int]) -> tuple[list[Glyph], list[int], int]:
    """Rasterize the inventory and split it into the ASCII run and the sparse run.

    Returns the glyph list in glyph-id order (id 0 reserved), the sorted sparse
    code points, and the index in ``glyphs`` where the sparse run starts.
    """
    ascii_run = [cp for cp in codepoints if ASCII_START <= cp <= ASCII_END]
    expected_ascii = list(range(ASCII_START, ASCII_END + 1))
    if ascii_run != expected_ascii:
        raise GeneratorError(
            "inventory must contain the full printable ASCII range U+0020-U+007E"
        )
    sparse_run = sorted(cp for cp in codepoints if not ASCII_START <= cp <= ASCII_END)
    ordered = ascii_run + sparse_run

    glyphs: list[Glyph] = []
    offset = 0
    for codepoint in ordered:
        glyph = rasterize(font, codepoint)
        glyph.bitmap_index = offset
        offset += len(glyph.packed())
        glyphs.append(glyph)
    return glyphs, sparse_run, len(ascii_run)


# ---------------------------------------------------------------------------
# Emission
# ---------------------------------------------------------------------------


def c_array(values: bytes | list[int], per_line: int) -> str:
    """Format integer values as an indented C initializer body."""
    lines = []
    for start in range(0, len(values), per_line):
        chunk = ", ".join(f"0x{value:x}" for value in values[start : start + per_line])
        lines.append(f"    {chunk},")
    return "\n".join(lines)


def glyph_comment(glyph: Glyph) -> str:
    character = chr(glyph.codepoint)
    if character == '"':
        character = '\\"'
    elif character == "\\":
        character = "\\\\"
    elif character == " ":
        character = "(space)"
    return f"/* U+{glyph.codepoint:04X} {character} */"


def table_bytes(cmaps: int = 2, sparse: int = 0, glyphs: int = 0) -> int:
    """Estimate the Flash cost of everything except the glyph bitmaps.

    Sizes follow the ESP32-C3 (32-bit) layout of the LVGL 9.5 structures.
    """
    glyph_dsc = 8 * (glyphs + 1)          # bitmap_index:20 + adv_w:12 + four bytes
    cmap_array = 24 * cmaps               # two pointers plus three scalars
    unicode_list = 2 * sparse
    font_dsc = 24                         # lv_font_fmt_txt_dsc_t
    font = 36                             # lv_font_t
    return glyph_dsc + cmap_array + unicode_list + font_dsc + font


def emit_font(glyphs: list[Glyph], sparse_run: list[int], ascii_count: int,
              font_path: Path, font_sha: str, weight: int) -> str:
    bitmap = bytearray()
    for glyph in glyphs:
        bitmap += glyph.packed()
    sparse_start = ascii_count + 1  # glyph id 1 is the first ASCII glyph
    range_start = sparse_run[0]
    unicode_offsets = [cp - range_start for cp in sparse_run]
    flash = len(bitmap) + table_bytes(sparse=len(sparse_run), glyphs=len(glyphs))

    out: list[str] = []
    out.append("/*******************************************************************************")
    out.append(f" * Size: {FONT_SIZE} px")
    out.append(f" * Bpp: {FONT_BPP}")
    out.append(f" * Source: Noto Sans SC (SIL OFL 1.1), wght={weight} instance")
    out.append(f" * Font SHA-256: {font_sha}")
    out.append(f" * Glyphs: {len(glyphs)} from assets/fonts/pocket_pal_chars.txt")
    out.append(f" * Flash: {flash} bytes (bitmap {len(bitmap)} + tables {flash - len(bitmap)})")
    out.append(" * Regenerate:")
    out.append(" *   /tmp/pocket-pal-font/venv/bin/python tools/gen_pocket_pal_font.py \\")
    out.append(f" *     --font {font_path} --python-requires-pillow")
    out.append(" *")
    out.append(" * Generated by tools/gen_pocket_pal_font.py, which re-parses this file and")
    out.append(" * decodes every glyph with LVGL's PLAIN 4 bpp reader before it succeeds.")
    out.append(" * Do not edit by hand: change the inventory and regenerate instead.")
    out.append(" ******************************************************************************/")
    out.append("")
    out.append('#include "lvgl.h"')
    out.append("")
    out.append(f"#ifndef {GUARD}")
    out.append(f"    #define {GUARD} 1")
    out.append("#endif")
    out.append("")
    out.append("/* Always compiled: Pocket Pal has no Kconfig switch for its own subset. */")
    out.append("#if 1")
    out.append("")
    out.append("/*-----------------")
    out.append(" *    BITMAPS")
    out.append(" *----------------*/")
    out.append("")
    out.append("/*Store the image of the glyphs*/")
    out.append(f"static {LARGE_CONST_ATTRIBUTE} const uint8_t glyph_bitmap[] = {{")
    for glyph in glyphs:
        out.append(f"    {glyph_comment(glyph)}")
        packed = glyph.packed()
        if packed:
            out.append(c_array(packed, per_line=12))
        out.append("")
    if out:
        # Drop the blank line that follows the last glyph.
        while out and out[-1] == "":
            out.pop()
    out.append("};")
    out.append("")
    out.append("/*-----------------")
    out.append(" *  GLYPH DESCRIPTION")
    out.append(" *----------------*/")
    out.append("")
    out.append("/*An empty descriptor is required at index 0: LVGL reserves it for \"no glyph\"*/")
    out.append("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {")
    out.append("    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0}"
               " /* id = 0 reserved */,")
    for glyph in glyphs:
        out.append(
            f"    {{.bitmap_index = {glyph.bitmap_index}, .adv_w = {glyph.adv_w}, "
            f".box_w = {glyph.width}, .box_h = {glyph.height}, "
            f".ofs_x = {glyph.ofs_x}, .ofs_y = {glyph.ofs_y}}},"
        )
    out.append("};")
    out.append("")
    out.append("/*-----------------")
    out.append(" *  CHARACTER MAPPING")
    out.append(" *----------------*/")
    out.append("")
    out.append("/*Collect the unicode lists and glyph_id offsets*/")
    out.append(f"static const uint16_t unicode_list_1[] = {{")
    out.append(c_array(unicode_offsets, per_line=8))
    out.append("};")
    out.append("")
    out.append(f"static const lv_font_fmt_txt_cmap_t cmaps[] = {{")
    out.append("    {")
    out.append(f"        .range_start = {ASCII_START}, .range_length = {ascii_count}, "
               f".glyph_id_start = 1,")
    out.append(f"        .unicode_list = NULL, .glyph_id_ofs_list = NULL, "
               f".list_length = 0, .type = {CMAP_FORMAT0_TINY}")
    out.append("    },")
    out.append("    {")
    out.append(f"        .range_start = {range_start}, "
               f".range_length = {sparse_run[-1] - range_start + 1}, "
               f".glyph_id_start = {sparse_start},")
    out.append(f"        .unicode_list = unicode_list_1, .glyph_id_ofs_list = NULL, "
               f".list_length = {len(sparse_run)}, .type = {CMAP_SPARSE_TINY}")
    out.append("    }")
    out.append("};")
    out.append("")
    out.append("/*--------------------")
    out.append(" *  ALL CUSTOM DATA")
    out.append(" *--------------------*/")
    out.append("")
    out.append("/*Store all the custom data of the font*/")
    out.append("static const lv_font_fmt_txt_dsc_t font_dsc = {")
    out.append("    .glyph_bitmap = glyph_bitmap,")
    out.append("    .glyph_dsc = glyph_dsc,")
    out.append("    .cmaps = cmaps,")
    out.append("    .kern_dsc = NULL,")
    out.append("    .kern_scale = 0,")
    out.append("    .cmap_num = 2,")
    out.append(f"    .bpp = {FONT_BPP},")
    out.append("    .kern_classes = 0,")
    out.append("    .bitmap_format = 0,   /* LV_FONT_FMT_TXT_PLAIN */")
    out.append("};")
    out.append("")
    out.append("/*-----------------")
    out.append(" *  PUBLIC FONT")
    out.append(" *----------------*/")
    out.append("")
    out.append("/*Initialize a public general font descriptor*/")
    out.append(f"const lv_font_t {SYMBOL} = {{")
    out.append("    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    "
               "/*Function pointer to get glyph's data*/")
    out.append("    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    "
               "/*Function pointer to get glyph's bitmap*/")
    out.append(f"    .line_height = {LINE_HEIGHT},          "
               "/*The maximum line height required by the font*/")
    out.append(f"    .base_line = {BASE_LINE},             "
               "/*Baseline measured from the bottom of the line*/")
    out.append("    .subpx = LV_FONT_SUBPX_NONE,")
    out.append("    .underline_position = -2,")
    out.append("    .underline_thickness = 1,")
    out.append("    .dsc = &font_dsc           "
               "/*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */")
    out.append("};")
    out.append("")
    out.append("")
    out.append("#endif /*#if 1*/")
    out.append("")
    return "\n".join(out)


# ---------------------------------------------------------------------------
# Self-verification: re-read the emitted file and decode it as LVGL does
# ---------------------------------------------------------------------------


def parse_emitted(source: str) -> tuple[bytes, list[dict[str, int]], list[dict], list[int]]:
    """Parse the glyph bitmap, descriptors, cmaps, and sparse list from C text."""
    def array_body(name: str) -> str:
        match = re.search(
            rf"static [^\n]*?{re.escape(name)}\[\] = \{{(.*?)\n\}};", source, re.S
        )
        if not match:
            raise GeneratorError(f"emitted file has no {name}[] array")
        return re.sub(r"/\*.*?\*/", "", match.group(1), flags=re.S)

    bitmap_values = [int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]+)", array_body("glyph_bitmap"))]
    oversized = [value for value in bitmap_values if value > 0xFF]
    if oversized:
        raise GeneratorError(
            f"glyph_bitmap has {len(oversized)} value(s) that do not fit a byte, "
            f"first 0x{oversized[0]:x}"
        )
    bitmap = bytes(bitmap_values)
    unicode_list = [
        int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]+)", array_body("unicode_list_1"))
    ]

    dsc_body = array_body("glyph_dsc")
    descriptors = [
        {key: int(value) for key, value in re.findall(r"\.(\w+) = (-?\d+)", entry)}
        for entry in re.findall(r"\{([^{}]*)\}", dsc_body)
    ]

    cmap_body = re.search(r"const lv_font_fmt_txt_cmap_t cmaps\[\] = \{(.*?)\n\};", source, re.S)
    if not cmap_body:
        raise GeneratorError("emitted file has no cmaps[] array")
    cmaps: list[dict] = []
    for entry in re.findall(r"\{([^{}]*(?:\{[^{}]*\}[^{}]*)*)\}", cmap_body.group(1)):
        fields: dict = {}
        for key, value in re.findall(r"\.(\w+) = ([^,\n]+)", entry):
            value = value.strip()
            fields[key] = int(value) if re.fullmatch(r"-?\d+", value) else value
        cmaps.append(fields)
    return bitmap, descriptors, cmaps, unicode_list


def resolve_glyph_id(cmaps: list[dict], unicode_list: list[int], codepoint: int) -> int:
    """Reimplement LVGL's get_glyph_dsc_id() over the parsed cmaps."""
    for cmap in cmaps:
        relative = codepoint - cmap["range_start"]
        if relative < 0 or relative >= cmap["range_length"]:
            continue
        if cmap["type"] == CMAP_FORMAT0_TINY:
            return cmap["glyph_id_start"] + relative
        if cmap["type"] == CMAP_SPARSE_TINY:
            if relative in unicode_list:
                return cmap["glyph_id_start"] + unicode_list.index(relative)
            return 0
        raise GeneratorError(f"unexpected cmap type in emitted file: {cmap['type']}")
    return 0


def decode_plain_4bpp(bitmap: bytes, index: int, width: int, height: int) -> list[int]:
    """Decode one glyph the way lv_font_get_bitmap_fmt_txt() does (stride 0)."""
    levels: list[int] = []
    cursor = index
    odd = False
    for _ in range(width * height):
        if not odd:
            levels.append(bitmap[cursor] >> 4)
        else:
            levels.append(bitmap[cursor] & 0xF)
            cursor += 1
        odd = not odd
    return levels


def verify_emitted(path: Path, glyphs: list[Glyph], sparse_run: list[int],
                   ascii_count: int) -> None:
    source = path.read_text(encoding="utf-8")
    if f"const lv_font_t {SYMBOL} =" not in source:
        raise GeneratorError(f"{path.name}: missing symbol {SYMBOL}")
    if ".bitmap_format = 0" not in source:
        raise GeneratorError(f"{path.name}: bitmap_format is not PLAIN")
    if "LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY" not in source:
        raise GeneratorError(f"{path.name}: missing the FORMAT0_TINY cmap")
    if "LV_FONT_FMT_TXT_CMAP_SPARSE_TINY" not in source:
        raise GeneratorError(f"{path.name}: missing the SPARSE_TINY cmap")

    bitmap, descriptors, cmaps, unicode_list = parse_emitted(source)
    if len(cmaps) != 2:
        raise GeneratorError(f"{path.name}: expected 2 cmaps, found {len(cmaps)}")
    if len(descriptors) != len(glyphs) + 1:
        raise GeneratorError(
            f"{path.name}: {len(descriptors)} glyph descriptors for {len(glyphs)} glyphs"
        )
    if unicode_list != [cp - sparse_run[0] for cp in sparse_run]:
        raise GeneratorError(f"{path.name}: unicode_list does not match the sparse run")
    if unicode_list != sorted(unicode_list):
        raise GeneratorError(f"{path.name}: unicode_list is not sorted for bsearch")

    problems: list[str] = []
    for order, glyph in enumerate(glyphs):
        glyph_id = resolve_glyph_id(cmaps, unicode_list, glyph.codepoint)
        if glyph_id != order + 1:
            problems.append(
                f"U+{glyph.codepoint:04X}: cmap resolves to glyph id {glyph_id}, "
                f"expected {order + 1}"
            )
            continue
        descriptor = descriptors[glyph_id]
        expected = {
            "bitmap_index": glyph.bitmap_index,
            "adv_w": glyph.adv_w,
            "box_w": glyph.width,
            "box_h": glyph.height,
            "ofs_x": glyph.ofs_x,
            "ofs_y": glyph.ofs_y,
        }
        for key, value in expected.items():
            if descriptor.get(key) != value:
                problems.append(
                    f"U+{glyph.codepoint:04X}: {key} is {descriptor.get(key)}, expected {value}"
                )
        if glyph.width and glyph.height:
            decoded = decode_plain_4bpp(
                bitmap, descriptor["bitmap_index"], descriptor["box_w"], descriptor["box_h"]
            )
            if decoded != glyph.levels:
                differing = sum(
                    actual != wanted for actual, wanted in zip(decoded, glyph.levels)
                )
                problems.append(
                    f"U+{glyph.codepoint:04X}: {differing} pixel(s) differ between the "
                    f"Pillow rasterization and the decoded C bitmap"
                )
        if descriptor["bitmap_index"] + (glyph.width * glyph.height + 1) // 2 > len(bitmap):
            problems.append(f"U+{glyph.codepoint:04X}: bitmap_index runs past glyph_bitmap")
        if descriptor["box_w"] and descriptor["adv_w"] <= 0:
            problems.append(f"U+{glyph.codepoint:04X}: zero advance width")

    if problems:
        for problem in problems[:20]:
            print(f"  {problem}", file=sys.stderr)
        if len(problems) > 20:
            print(f"  ... and {len(problems) - 20} more", file=sys.stderr)
        raise GeneratorError(f"{path.name}: self-verification failed")

    # The descriptor must still be brace-balanced after the round trip.
    if source.count("{") != source.count("}"):
        raise GeneratorError(f"{path.name}: unbalanced braces")
    ascii_cmap, sparse_cmap = cmaps
    if ascii_cmap["range_start"] != ASCII_START or ascii_cmap["range_length"] != ascii_count:
        raise GeneratorError(f"{path.name}: ASCII cmap range is wrong")
    if sparse_cmap["glyph_id_start"] != ascii_count + 1:
        raise GeneratorError(f"{path.name}: sparse cmap does not follow the ASCII run")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------


def run_generate(args: argparse.Namespace) -> int:
    inventory = load_inventory(INVENTORY_PATH)
    font_path = Path(args.font).expanduser()
    font = load_pillow_font(font_path, args.weight)
    font_sha = hashlib.sha256(font_path.read_bytes()).hexdigest()

    glyphs, sparse_run, ascii_count = plan_glyphs(font, inventory)
    source = emit_font(glyphs, sparse_run, ascii_count, font_path, font_sha, args.weight)
    OUTPUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT_PATH.write_text(source, encoding="utf-8")

    verify_emitted(OUTPUT_PATH, glyphs, sparse_run, ascii_count)

    bitmap_bytes = sum(len(glyph.packed()) for glyph in glyphs)
    flash = bitmap_bytes + table_bytes(sparse=len(sparse_run), glyphs=len(glyphs))
    above = max(glyph.height + glyph.ofs_y for glyph in glyphs)
    below = -min(glyph.ofs_y for glyph in glyphs)
    print(f"pocket_pal font: WROTE {display_path(OUTPUT_PATH)} "
          f"({OUTPUT_PATH.stat().st_size} bytes on disk)")
    print(f"  glyphs: {len(glyphs)} (+1 reserved) at {FONT_SIZE} px, {FONT_BPP} bpp, PLAIN")
    print(f"  cmaps: FORMAT0_TINY U+{ASCII_START:04X}-U+{ASCII_END:04X} "
          f"({ascii_count}) + SPARSE_TINY {len(sparse_run)} code points")
    print(f"  metrics: line_height {LINE_HEIGHT}, base_line {BASE_LINE}; ink reaches "
          f"{above} px above and {below} px below the baseline")
    print(f"  flash: ~{flash} bytes ({bitmap_bytes} bitmap + {flash - bitmap_bytes} tables, "
          f"{flash / 1024:.1f} KiB)")
    print("  self-verification: PASS - every glyph re-decoded from the emitted C "
          "matches the Pillow rasterization pixel for pixel")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Generate or check the Pocket Pal 16 px LVGL font subset.",
    )
    parser.add_argument("--check", action="store_true",
                        help="stdlib-only coverage check against the Pocket Pal sources")
    parser.add_argument("--font", help="path to the licensed source font (generate mode)")
    parser.add_argument("--weight", type=int, default=400,
                        help="variable-font weight instance (default: 400)")
    parser.add_argument("--python-requires-pillow", action="store_true",
                        help="acknowledge that generate mode needs Pillow in this interpreter")
    args = parser.parse_args(argv)

    try:
        if args.check:
            return run_check()
        if not args.font:
            parser.error("generate mode needs --font (or pass --check for coverage only)")
        if not args.python_requires_pillow:
            parser.error("generate mode must confirm its dependency with --python-requires-pillow")
        return run_generate(args)
    except GeneratorError as error:
        print(f"gen_pocket_pal_font: ERROR: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
