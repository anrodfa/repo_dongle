"""
Genera una fuente LVGL nueva reescalando otra ya generada (.c de lv_font_conv,
4 bpp sin comprimir), solo con los caracteres pedidos.

Para el reloj del dongle hacian falta tamanos que el modulo Prospector no trae
(FRAC solo llega a 48 px) y no estan los .ttf originales para pasarlos por
lv_font_conv. Reescalar el bitmap una vez aqui, con Lanczos, queda mucho mas
limpio que escalar en el dongle con LVGL. Se pierde el kerning (la fecha va
sin el; en la previsualizacion no se nota).

Uso (los .c fuente estan en el modulo, boards/shields/prospector_adapter/src/fonts):
    python tools/scale_font.py FR_Regular_48.c 4/3 Clock_Time_64 "-0123456789:" > src/fonts/Clock_Time_64.c
    python tools/scale_font.py FG_Medium_24.c 5/4 Clock_Date_30 " 0123456789abcdefghijklmnopqrstuvwxyz" > src/fonts/Clock_Date_30.c

Necesita Pillow.
"""

import re
import sys
from fractions import Fraction

from PIL import Image

PAD = 2


def parse(path):
    src = open(path, encoding="utf-8", errors="replace").read()
    bpp = int(re.search(r"\.bpp = (\d+)", src).group(1))
    if bpp != 4 or "--no-compress" not in src:
        sys.exit("solo fuentes de 4 bpp sin comprimir")
    bm = src[src.index("glyph_bitmap[]"):src.index("glyph_dsc[]")]
    bm = re.sub(r"/\*.*?\*/", "", bm, flags=re.S)
    bitmap = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{1,2})\b", bm))
    glyphs = [tuple(int(v) for v in m.groups()) for m in re.finditer(
        r"\{\.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), \.box_h = (\d+), "
        r"\.ofs_x = (-?\d+), \.ofs_y = (-?\d+)\}", src)]
    start = int(re.search(r"\.range_start = (\d+)", src).group(1))
    length = int(re.search(r"\.range_length = (\d+)", src).group(1))
    if src.count(".range_start") != 1:
        sys.exit("solo fuentes con un unico rango de caracteres")
    metrics = {k: int(re.search(rf"\.{k} = (-?\d+)", src).group(1))
               for k in ("line_height", "base_line", "underline_position", "underline_thickness")}
    return bitmap, glyphs, start, length, metrics


def glyph_image(bitmap, idx, w, h):
    img = Image.new("L", (w + 2 * PAD, h + 2 * PAD), 0)
    pos = idx * 8
    for y in range(h):
        for x in range(w):
            v = (bitmap[pos // 8] >> (4 - pos % 8)) & 0xF
            img.putpixel((x + PAD, y + PAD), v * 17)
            pos += 4
    return img


def scale_glyph(bitmap, g, s):
    idx, adv, w, h, ox, oy = g
    adv_new = round(adv * s)
    if w == 0 or h == 0:
        return adv_new, 0, 0, 0, 0, b""
    img = glyph_image(bitmap, idx, w, h)
    W, H = round(img.width * s), round(img.height * s)
    img = img.resize((W, H), Image.LANCZOS)
    # Recorta lo que queda por debajo de 1/16 tras cuantizar a 4 bits.
    box = img.point(lambda v: 255 if v >= 8 else 0).getbbox()
    if box is None:
        return adv_new, 0, 0, 0, 0, b""
    l, t, r, b = box
    img = img.crop(box)
    # ofs_y se mide desde la linea base hacia arriba hasta el borde inferior.
    ox_new = round((ox - PAD) * s + l)
    oy_new = round((oy - PAD) * s + (H - b))
    bits = []
    for y in range(img.height):
        for x in range(img.width):
            bits.append(min(15, round(img.getpixel((x, y)) / 17)))
    if len(bits) % 2:
        bits.append(0)
    data = bytes((bits[i] << 4) | bits[i + 1] for i in range(0, len(bits), 2))
    return adv_new, img.width, img.height, ox_new, oy_new, data


def ranges(chars):
    cps = sorted(set(ord(c) for c in chars))
    out = []
    for cp in cps:
        if out and cp == out[-1][0] + out[-1][1]:
            out[-1][1] += 1
        else:
            out.append([cp, 1])
    return out


def main():
    path, scale, name, chars = sys.argv[1:5]
    s = float(Fraction(scale))
    bitmap, glyphs, start, length, m = parse(path)
    rs = ranges(chars)

    bitmap_out = bytearray()
    dsc = ["    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */"]
    comments = []
    for first, n in rs:
        for cp in range(first, first + n):
            if not start <= cp < start + length:
                sys.exit(f"{chr(cp)!r} no esta en la fuente de origen")
            adv, w, h, ox, oy, data = scale_glyph(bitmap, glyphs[cp - start + 1], s)
            dsc.append(f"    {{.bitmap_index = {len(bitmap_out)}, .adv_w = {adv}, .box_w = {w}, "
                       f".box_h = {h}, .ofs_x = {ox}, .ofs_y = {oy}}}")
            comments.append((cp, len(bitmap_out), data))
            bitmap_out += data

    guard = name.upper()
    o = []
    o.append("#include <lvgl.h>")
    o.append("/*******************************************************************************")
    o.append(f" * Generado con tools/scale_font.py a partir de {path.replace(chr(92), '/').split('/')[-1]}")
    o.append(f" * (modulo prospector-zmk-module), escalado x{scale}. Bpp: 4. No editar a mano.")
    o.append(f" * Caracteres: {''.join(chr(c) for f, n in rs for c in range(f, f + n))}")
    o.append(" ******************************************************************************/")
    o.append("")
    o.append(f"#ifndef {guard}\n#define {guard} 1\n#endif\n\n#if {guard}\n")
    o.append("static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {")
    for cp, idx, data in comments:
        o.append(f"    /* U+{cp:04X} \"{chr(cp)}\" */")
        for i in range(0, len(data), 16):
            o.append("    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 16]) + ",")
    o.append("};\n")
    o.append("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {")
    o.append(",\n".join(dsc))
    o.append("};\n")
    o.append("static const lv_font_fmt_txt_cmap_t cmaps[] = {")
    gid = 1
    for first, n in rs:
        o.append(f"    {{.range_start = {first}, .range_length = {n}, .glyph_id_start = {gid},")
        o.append("     .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0,")
        o.append("     .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY},")
        gid += n
    o.append("};\n")
    o.append("static const lv_font_fmt_txt_dsc_t font_dsc = {")
    o.append("    .glyph_bitmap = glyph_bitmap,")
    o.append("    .glyph_dsc = glyph_dsc,")
    o.append("    .cmaps = cmaps,")
    o.append("    .kern_dsc = NULL,")
    o.append("    .kern_scale = 0,")
    o.append(f"    .cmap_num = {len(rs)},")
    o.append("    .bpp = 4,")
    o.append("    .kern_classes = 0,")
    o.append("    .bitmap_format = 0,")
    o.append("};\n")
    o.append(f"const lv_font_t {name} = {{")
    o.append("    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,")
    o.append("    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,")
    o.append(f"    .line_height = {round(m['line_height'] * s)},")
    o.append(f"    .base_line = {round(m['base_line'] * s)},")
    o.append("    .subpx = LV_FONT_SUBPX_NONE,")
    o.append(f"    .underline_position = {round(m['underline_position'] * s)},")
    o.append(f"    .underline_thickness = {max(1, round(m['underline_thickness'] * s))},")
    o.append("    .dsc = &font_dsc,")
    o.append("    .fallback = NULL,")
    o.append("    .user_data = NULL,")
    o.append("};\n")
    o.append(f"#endif /*#if {guard}*/")
    sys.stdout.write("\n".join(o) + "\n")
    print(f"{name}: {len(bitmap_out)} B de bitmap, {len(dsc) - 1} glifos", file=sys.stderr)


main()
