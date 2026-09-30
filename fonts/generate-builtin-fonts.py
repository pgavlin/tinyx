#!/usr/bin/env python3
"""Generate TinyX's decoded built-in bitmap font tables from X.Org BDF files."""

import argparse
import hashlib
from pathlib import Path


def c_string(value):
    return '"' + value.replace('\\', '\\\\').replace('"', '\\"') + '"'


def parse_bdf(path, accepted, require_all=False):
    raw = path.read_bytes()
    lines = raw.decode("ascii").splitlines()
    font_name = None
    ascent = descent = None
    default = -1
    copyright_value = ""
    glyphs = {}
    i = 0
    while i < len(lines):
        line = lines[i]
        if line.startswith("FONT "):
            font_name = line[5:]
        elif line.startswith("FONT_ASCENT "):
            ascent = int(line.split()[1])
        elif line.startswith("FONT_DESCENT "):
            descent = int(line.split()[1])
        elif line.startswith("DEFAULT_CHAR "):
            default = int(line.split()[1])
        elif line.startswith("COPYRIGHT "):
            copyright_value = line[len("COPYRIGHT "):].strip()
            if copyright_value.startswith('"') and copyright_value.endswith('"'):
                copyright_value = copyright_value[1:-1].replace('""', '"')
        elif line.startswith("STARTCHAR "):
            encoding = None
            dwidth = None
            bbx = None
            bitmap = []
            i += 1
            while i < len(lines) and lines[i] != "ENDCHAR":
                item = lines[i]
                if item.startswith("ENCODING "):
                    values = [int(v) for v in item.split()[1:]]
                    encoding = values[-1]
                elif item.startswith("DWIDTH "):
                    dwidth = int(item.split()[1])
                elif item.startswith("BBX "):
                    bbx = tuple(int(v) for v in item.split()[1:5])
                elif item == "BITMAP":
                    if bbx is None:
                        raise ValueError(f"{path}: BITMAP before BBX")
                    for _ in range(bbx[1]):
                        i += 1
                        bitmap.append(bytes.fromhex(lines[i]))
                i += 1
            if encoding in accepted:
                if dwidth is None or bbx is None or len(bitmap) != bbx[1]:
                    raise ValueError(f"{path}: incomplete glyph {encoding}")
                expected = (bbx[0] + 7) // 8
                if any(len(row) != expected for row in bitmap):
                    raise ValueError(f"{path}: invalid bitmap width for {encoding}")
                if encoding in glyphs:
                    raise ValueError(f"{path}: duplicate encoding {encoding}")
                glyphs[encoding] = (dwidth, bbx, b"".join(bitmap), expected)
        i += 1
    if font_name is None or ascent is None or descent is None:
        raise ValueError(f"{path}: missing font metadata")
    missing = accepted.difference(glyphs)
    if require_all and missing:
        raise ValueError(f"{path}: missing encodings {sorted(missing)[:8]}")
    return {
        "sha256": hashlib.sha256(raw).hexdigest(),
        "source_name": path.name,
        "font_name": font_name,
        "ascent": ascent,
        "descent": descent,
        "default": default,
        "copyright": copyright_value,
        "glyphs": glyphs,
    }


def emit_bytes(out, name, data):
    out.append(f"static const uint8_t {name}[] = {{")
    for i in range(0, len(data), 12):
        out.append("    " + ", ".join(f"0x{x:02x}" for x in data[i:i + 12]) + ",")
    out.append("};")
    out.append("")


def emit_font(out, symbol, parsed, canonical, first, last):
    bitmap = bytearray()
    encoding = []
    metrics = []
    if last <= 255:
        first_col, last_col = first, last
        first_row = last_row = 0
    else:
        if first != 0 or last != 65535:
            raise ValueError("multi-row fonts must cover the complete 16-bit encoding space")
        first_col, last_col = 0, 255
        first_row, last_row = 0, 255
    for code in range(first, last + 1):
        source = parsed["glyphs"].get(code)
        if source is None:
            encoding.append(-1)
            continue
        dwidth, (width, height, xoff, yoff), bits, stride = source
        offset = len(bitmap)
        bitmap.extend(bits)
        metrics.append((xoff, xoff + width, dwidth, height + yoff, -yoff,
                        offset, stride, width, height))
        encoding.append(len(metrics) - 1)

    out.append(f"/* {parsed['source_name']} sha256 {parsed['sha256']} */")
    emit_bytes(out, symbol + "Bitmap", bitmap)
    out.append(f"static const TinyXDecodedGlyph {symbol}Glyphs[] = {{")
    for m in metrics:
        out.append("    { %d, %d, %d, %d, %d, 0, %d, %d, %d, %d }," % m)
    out.append("};")
    out.append("")
    out.append(f"static const int16_t {symbol}Encoding[] = {{")
    for i in range(0, len(encoding), 16):
        out.append("    " + ", ".join(str(x) for x in encoding[i:i + 16]) + ",")
    out.append("};")
    out.append("")
    props = [("FONT", canonical), ("COPYRIGHT", parsed["copyright"])]
    out.append(f"static const TinyXDecodedFontProperty {symbol}Properties[] = {{")
    for key, value in props:
        out.append(f"    {{ {c_string(key)}, {c_string(value)}, 0 }},")
    out.append("};")
    out.append("")
    default = parsed["default"]
    if default < first or default > last:
        default = first
    out.append(f"const TinyXDecodedFont {symbol} = {{")
    out.append(f"    {c_string(canonical)}, {first_col}, {last_col}, {first_row}, {last_row}, {default},")
    out.append(f"    {parsed['ascent']}, {parsed['descent']},")
    out.append(f"    {symbol}Glyphs, sizeof({symbol}Glyphs) / sizeof({symbol}Glyphs[0]),")
    out.append(f"    {symbol}Encoding, sizeof({symbol}Encoding) / sizeof({symbol}Encoding[0]),")
    out.append(f"    {symbol}Bitmap, sizeof({symbol}Bitmap),")
    out.append(f"    {symbol}Properties, sizeof({symbol}Properties) / sizeof({symbol}Properties[0])")
    out.append("};")
    out.append("")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--fixed", type=Path, required=True)
    parser.add_argument("--cursor", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    fixed = parse_bdf(args.fixed, set(range(65536)))
    cursor = parse_bdf(args.cursor, set(range(154)), require_all=True)
    out = [
        "/* Generated by fonts/generate-builtin-fonts.py; do not edit. */",
        "/* Sources: font-misc-misc 1.1.3 and font-cursor-misc 1.0.4. */",
        "#ifdef HAVE_DIX_CONFIG_H",
        "#include <dix-config.h>",
        "#endif",
        "",
        "#include \"embedded-font.h\"",
        "",
    ]
    emit_font(out, "tinyxEmbeddedFixedFont", fixed,
              "-misc-fixed-medium-r-semicondensed--13-120-75-75-c-60-iso10646-1", 0, 65535)
    emit_font(out, "tinyxEmbeddedCursorFont", cursor, "cursor", 0, 153)
    out.extend([
        "const TinyXEmbeddedFontName tinyxEmbeddedFontNames[] = {",
        "    { \"fixed\", &tinyxEmbeddedFixedFont },",
        "    { \"6x13\", &tinyxEmbeddedFixedFont },",
        "    { \"-misc-fixed-medium-r-semicondensed--13-100-100-100-c-60-iso10646-1\", &tinyxEmbeddedFixedFont },",
        "    { \"-misc-fixed-medium-r-semicondensed--13-120-75-75-c-60-iso10646-1\", &tinyxEmbeddedFixedFont },",
        "    { \"-misc-fixed-medium-r-semicondensed--13-100-100-100-c-60-iso8859-1\", &tinyxEmbeddedFixedFont },",
        "    { \"-misc-fixed-medium-r-semicondensed--13-120-75-75-c-60-iso8859-1\", &tinyxEmbeddedFixedFont },",
        "    { \"cursor\", &tinyxEmbeddedCursorFont },",
        "};",
        "const size_t tinyxEmbeddedFontNameCount =",
        "    sizeof(tinyxEmbeddedFontNames) / sizeof(tinyxEmbeddedFontNames[0]);",
        "",
    ])
    args.output.write_text("\n".join(out), encoding="ascii")


if __name__ == "__main__":
    main()
