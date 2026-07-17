#!/usr/bin/env python3

import os
import sys
import math
import re
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
FEATHER_DIR = ROOT / "assets" / "icons" / "feather"
MATERIAL_ROUNDED_DIR = ROOT / "assets" / "icons" / "material-rounded"
ICON_H = ROOT / "include" / "lens" / "icon.h"
ICON_DATA_C = ROOT / "src" / "icon_data.c"

ICON_RENDER_STROKE = 0
ICON_RENDER_FILL = 1

ICON_SOURCES = (
    (FEATHER_DIR, ICON_RENDER_STROKE),
    # Appended after Feather so the existing public icon IDs stay stable.
    (MATERIAL_ROUNDED_DIR, ICON_RENDER_FILL),
)

CMD_MOVE_TO = 0
CMD_LINE_TO = 1
CMD_CUBIC_TO = 2
CMD_QUAD_TO = 3
CMD_CLOSE = 4
CMD_ADD_CIRCLE = 5
CMD_ADD_RECT = 6

PARAM_COUNTS = {
    "M": 2, "m": 2, "L": 2, "l": 2,
    "H": 1, "h": 1, "V": 1, "v": 1,
    "C": 6, "c": 6, "S": 4, "s": 4,
    "Q": 4, "q": 4, "T": 2, "t": 2,
    "A": 7, "a": 7,
    "Z": 0, "z": 0,
}


def tokenize_d(d):
    token_re = re.compile(
        r"([MmLlHhVvCcSsQqTtAaZz])"
        r"|([+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?)"
    )
    tokens = []
    for m in token_re.finditer(d):
        if m.group(1):
            tokens.append(m.group(1))
        else:
            tokens.append(float(m.group(2)))
    return tokens


def arc_to_cubics(x1, y1, rx, ry, phi_deg, fa, fs, x2, y2):
    rx = abs(rx)
    ry = abs(ry)
    if rx == 0 or ry == 0:
        return [("C", [x2, y2, x2, y2, x2, y2])]

    phi = math.radians(phi_deg)
    cos_phi = math.cos(phi)
    sin_phi = math.sin(phi)

    dx = (x1 - x2) / 2.0
    dy = (y1 - y2) / 2.0
    x1p = cos_phi * dx + sin_phi * dy
    y1p = -sin_phi * dx + cos_phi * dy

    x1p2 = x1p * x1p
    y1p2 = y1p * y1p
    rx2 = rx * rx
    ry2 = ry * ry

    lam = x1p2 / rx2 + y1p2 / ry2
    if lam > 1:
        ls = math.sqrt(lam)
        rx *= ls
        ry *= ls
        rx2 = rx * rx
        ry2 = ry * ry

    num = max(0.0, rx2 * ry2 - rx2 * y1p2 - ry2 * x1p2)
    den = rx2 * y1p2 + ry2 * x1p2
    if den == 0:
        return [("C", [x2, y2, x2, y2, x2, y2])]

    cprime = math.sqrt(num / den)
    if fa == fs:
        cprime = -cprime

    cxp = cprime * rx * y1p / ry
    cyp = -cprime * ry * x1p / rx

    cx = cos_phi * cxp - sin_phi * cyp + (x1 + x2) / 2.0
    cy = sin_phi * cxp + cos_phi * cyp + (y1 + y2) / 2.0

    def vec_angle(ux, uy, vx, vy):
        return math.atan2(ux * vy - uy * vx, ux * vx + uy * vy)

    theta1 = vec_angle(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry)
    dtheta = vec_angle(
        (x1p - cxp) / rx, (y1p - cyp) / ry,
        (-x1p - cxp) / rx, (-y1p - cyp) / ry,
    )

    if fs == 0 and dtheta > 0:
        dtheta -= 2 * math.pi
    elif fs == 1 and dtheta < 0:
        dtheta += 2 * math.pi

    n_segs = max(1, int(math.ceil(abs(dtheta) / (math.pi / 2.0))))
    seg_angle = dtheta / n_segs

    result = []
    for i in range(n_segs):
        t1 = theta1 + i * seg_angle
        dt = seg_angle
        half_dt = dt / 2.0
        if abs(half_dt) > 1e-10:
            k = (4.0 / 3.0) * math.tan(half_dt / 2.0)
        else:
            k = 0.0

        cos_t1 = math.cos(t1)
        sin_t1 = math.sin(t1)
        cos_t2 = math.cos(t1 + dt)
        sin_t2 = math.sin(t1 + dt)

        ep1x = rx * cos_t1
        ep1y = ry * sin_t1
        ep2x = rx * cos_t2
        ep2y = ry * sin_t2

        cp1x = ep1x - k * rx * sin_t1
        cp1y = ep1y + k * ry * cos_t1
        cp2x = ep2x + k * rx * sin_t2
        cp2y = ep2y - k * ry * cos_t2

        def xform(px, py):
            return (
                cos_phi * px - sin_phi * py + cx,
                sin_phi * px + cos_phi * py + cy,
            )

        c1 = xform(cp1x, cp1y)
        c2 = xform(cp2x, cp2y)
        p3 = xform(ep2x, ep2y)
        result.append(("C", [c1[0], c1[1], c2[0], c2[1], p3[0], p3[1]]))

    return result


def parse_svg_path(d):
    if not d or not d.strip():
        return []
    tokens = tokenize_d(d)
    commands = []
    pos = 0

    def consume(n):
        nonlocal pos
        nums = []
        for _ in range(n):
            if pos < len(tokens) and isinstance(tokens[pos], float):
                nums.append(tokens[pos])
                pos += 1
            else:
                return nums
        return nums

    while pos < len(tokens):
        tok = tokens[pos]
        if not isinstance(tok, str):
            break
        cmd = tok
        pos += 1

        if cmd in ("Z", "z"):
            commands.append((cmd, []))
            continue

        n = PARAM_COUNTS.get(cmd, 0)
        if n == 0:
            continue

        params = consume(n)
        if len(params) != n:
            break
        commands.append((cmd, params))

        repeat = cmd
        if cmd == "M":
            repeat = "L"
        elif cmd == "m":
            repeat = "l"

        while pos < len(tokens) and isinstance(tokens[pos], float):
            params = consume(n)
            if len(params) != n:
                break
            commands.append((repeat, params))

    return commands


def to_absolute(commands):
    result = []
    cx, cy = 0.0, 0.0
    sx, sy = 0.0, 0.0
    last_cmd = None
    last_cp = None

    for cmd, params in commands:
        is_rel = cmd.islower()
        base = cmd.upper()

        if base == "M":
            if is_rel:
                x = cx + params[0]
                y = cy + params[1]
            else:
                x, y = params[0], params[1]
            result.append(("M", [x, y]))
            cx, cy = x, y
            sx, sy = x, y
            last_cp = None

        elif base == "L":
            if is_rel:
                x = cx + params[0]
                y = cy + params[1]
            else:
                x, y = params[0], params[1]
            result.append(("L", [x, y]))
            cx, cy = x, y
            last_cp = None

        elif base == "H":
            if is_rel:
                x = cx + params[0]
            else:
                x = params[0]
            result.append(("L", [x, cy]))
            cx = x
            last_cp = None

        elif base == "V":
            if is_rel:
                y = cy + params[0]
            else:
                y = params[0]
            result.append(("L", [cx, y]))
            cy = y
            last_cp = None

        elif base == "C":
            x1, y1, x2, y2, x, y = params
            if is_rel:
                x1 += cx
                y1 += cy
                x2 += cx
                y2 += cy
                x += cx
                y += cy
            result.append(("C", [x1, y1, x2, y2, x, y]))
            last_cp = (x2, y2)
            cx, cy = x, y

        elif base == "S":
            x2, y2, x, y = params
            if is_rel:
                x2 += cx
                y2 += cy
                x += cx
                y += cy
            if last_cmd and last_cmd.upper() in ("C", "S") and last_cp:
                x1 = 2 * cx - last_cp[0]
                y1 = 2 * cy - last_cp[1]
            else:
                x1, y1 = cx, cy
            result.append(("C", [x1, y1, x2, y2, x, y]))
            last_cp = (x2, y2)
            cx, cy = x, y

        elif base == "Q":
            qx, qy, x, y = params
            if is_rel:
                qx += cx
                qy += cy
                x += cx
                y += cy
            result.append(("Q", [qx, qy, x, y]))
            last_cp = (qx, qy)
            cx, cy = x, y

        elif base == "T":
            x, y = params
            if is_rel:
                x += cx
                y += cy
            if last_cmd and last_cmd.upper() in ("Q", "T") and last_cp:
                qx = 2 * cx - last_cp[0]
                qy = 2 * cy - last_cp[1]
            else:
                qx, qy = cx, cy
            result.append(("Q", [qx, qy, x, y]))
            last_cp = (qx, qy)
            cx, cy = x, y

        elif base == "A":
            rx_a, ry_a, phi, fa, fs_a, x, y = params
            if is_rel:
                x += cx
                y += cy
            arcs = arc_to_cubics(cx, cy, rx_a, ry_a, phi, fa, fs_a, x, y)
            result.extend(arcs)
            cx, cy = x, y
            last_cp = None

        elif base == "Z":
            result.append(("Z", []))
            cx, cy = sx, sy
            last_cp = None

        last_cmd = cmd

    return result


def commands_to_cmds(commands):
    cmds = []
    for cmd, params in commands:
        if cmd == "M":
            c = [CMD_MOVE_TO, params[0], params[1], 0.0, 0.0, 0.0, 0.0]
            cmds.append(c)
        elif cmd == "L":
            c = [CMD_LINE_TO, params[0], params[1], 0.0, 0.0, 0.0, 0.0]
            cmds.append(c)
        elif cmd == "C":
            c = [CMD_CUBIC_TO, params[0], params[1], params[2], params[3],
                 params[4], params[5]]
            cmds.append(c)
        elif cmd == "Q":
            c = [CMD_QUAD_TO, params[0], params[1], params[2], params[3],
                 0.0, 0.0]
            cmds.append(c)
        elif cmd == "Z":
            c = [CMD_CLOSE, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0]
            cmds.append(c)
    return cmds


def parse_points(s):
    nums = re.findall(r"[+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?", s)
    points = []
    for i in range(0, len(nums) - 1, 2):
        points.append((float(nums[i]), float(nums[i + 1])))
    return points


KAPPA = 0.5522847498


def parse_svg(filepath):
    tree = ET.parse(filepath)
    root = tree.getroot()
    all_cmds = []

    for el in root:
        tag = el.tag
        if "}" in tag:
            tag = tag.split("}", 1)[1]

        if tag == "path":
            d = el.get("d", "")
            raw = parse_svg_path(d)
            abs_cmds = to_absolute(raw)
            all_cmds.extend(commands_to_cmds(abs_cmds))

        elif tag == "line":
            x1 = float(el.get("x1", "0"))
            y1 = float(el.get("y1", "0"))
            x2 = float(el.get("x2", "0"))
            y2 = float(el.get("y2", "0"))
            all_cmds.append([CMD_MOVE_TO, x1, y1, 0.0, 0.0, 0.0, 0.0])
            all_cmds.append([CMD_LINE_TO, x2, y2, 0.0, 0.0, 0.0, 0.0])

        elif tag == "circle":
            cx = float(el.get("cx", "0"))
            cy = float(el.get("cy", "0"))
            r = float(el.get("r", "0"))
            all_cmds.append([CMD_ADD_CIRCLE, cx, cy, r, 0.0, 0.0, 0.0])

        elif tag == "rect":
            x = float(el.get("x", "0"))
            y = float(el.get("y", "0"))
            w = float(el.get("width", "0"))
            h = float(el.get("height", "0"))
            rx = float(el.get("rx", "0") or "0")
            ry = float(el.get("ry", "0") or "0")
            r = max(rx, ry)
            if r > 0:
                all_cmds.append([CMD_MOVE_TO, x + r, y, 0.0, 0.0, 0.0, 0.0])
                all_cmds.append([CMD_LINE_TO, x + w - r, y, 0.0, 0.0, 0.0, 0.0])
                all_cmds.append([CMD_CUBIC_TO,
                    x + w - r + r * KAPPA, y,
                    x + w, y + r - r * KAPPA,
                    x + w, y + r])
                all_cmds.append([CMD_LINE_TO, x + w, y + h - r, 0.0, 0.0, 0.0, 0.0])
                all_cmds.append([CMD_CUBIC_TO,
                    x + w, y + h - r + r * KAPPA,
                    x + w - r + r * KAPPA, y + h,
                    x + w - r, y + h])
                all_cmds.append([CMD_LINE_TO, x + r, y + h, 0.0, 0.0, 0.0, 0.0])
                all_cmds.append([CMD_CUBIC_TO,
                    x + r - r * KAPPA, y + h,
                    x, y + h - r + r * KAPPA,
                    x, y + h - r])
                all_cmds.append([CMD_LINE_TO, x, y + r, 0.0, 0.0, 0.0, 0.0])
                all_cmds.append([CMD_CUBIC_TO,
                    x, y + r - r * KAPPA,
                    x + r - r * KAPPA, y,
                    x + r, y])
                all_cmds.append([CMD_CLOSE, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0])
            else:
                all_cmds.append([CMD_ADD_RECT, x, y, w, h, 0.0, 0.0])

        elif tag == "polyline":
            points = parse_points(el.get("points", ""))
            if points:
                all_cmds.append([CMD_MOVE_TO, points[0][0], points[0][1],
                                 0.0, 0.0, 0.0, 0.0])
                for px, py in points[1:]:
                    all_cmds.append([CMD_LINE_TO, px, py, 0.0, 0.0, 0.0, 0.0])

        elif tag == "polygon":
            points = parse_points(el.get("points", ""))
            if points:
                all_cmds.append([CMD_MOVE_TO, points[0][0], points[0][1],
                                 0.0, 0.0, 0.0, 0.0])
                for px, py in points[1:]:
                    all_cmds.append([CMD_LINE_TO, px, py, 0.0, 0.0, 0.0, 0.0])
                all_cmds.append([CMD_CLOSE, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0])

        elif tag == "ellipse":
            ecx = float(el.get("cx", "0"))
            ecy = float(el.get("cy", "0"))
            erx = float(el.get("rx", "0"))
            ery = float(el.get("ry", "0"))
            all_cmds.append([CMD_MOVE_TO, ecx, ecy - ery, 0.0, 0.0, 0.0, 0.0])
            all_cmds.append([CMD_CUBIC_TO,
                ecx + KAPPA * erx, ecy - ery,
                ecx + erx, ecy - KAPPA * ery,
                ecx + erx, ecy])
            all_cmds.append([CMD_CUBIC_TO,
                ecx + erx, ecy + KAPPA * ery,
                ecx + KAPPA * erx, ecy + ery,
                ecx, ecy + ery])
            all_cmds.append([CMD_CUBIC_TO,
                ecx - KAPPA * erx, ecy + ery,
                ecx - erx, ecy + KAPPA * ery,
                ecx - erx, ecy])
            all_cmds.append([CMD_CUBIC_TO,
                ecx - erx, ecy - KAPPA * ery,
                ecx - KAPPA * erx, ecy - ery,
                ecx, ecy - ery])
            all_cmds.append([CMD_CLOSE, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0])

    return all_cmds


def svg_render_mode(filepath, fallback):
    root = ET.parse(filepath).getroot()
    fill = (root.get("fill") or "").strip().lower()
    stroke = (root.get("stroke") or "").strip().lower()
    if fill == "none" and stroke and stroke != "none":
        return ICON_RENDER_STROKE
    return fallback


def icon_enum_name(filename):
    stem = Path(filename).stem
    return stem.upper().replace("-", "_")


def icon_array_name(filename):
    stem = Path(filename).stem
    return stem.replace("-", "_")


def fmt_float(f):
    if f == 0.0:
        return "0.0f"
    s = f"{f:.6f}"
    if "." in s:
        s = s.rstrip("0")
        if s.endswith("."):
            s += "0"
    return s + "f"


def generate_icon_h(icons):
    lines = []
    lines.append("#ifndef LENS_ICON_H")
    lines.append("#define LENS_ICON_H")
    lines.append("")
    lines.append("#include <stdint.h>")
    lines.append("")
    lines.append("typedef enum lens_icon_id {")
    for i, (name, _, _) in enumerate(icons):
        lines.append(f"\tLENS_ICON_{name} = {i},")
    lines.append("\tLENS_ICON_COUNT,")
    lines.append("} lens_icon_id;")
    lines.append("")
    lines.append("typedef struct lens_icon_cmd {")
    lines.append("\tuint8_t type;")
    lines.append("\tfloat params[6];")
    lines.append("} lens_icon_cmd;")
    lines.append("")
    lines.append("typedef struct lens_icon_desc {")
    lines.append("\tconst lens_icon_cmd *cmds;")
    lines.append("\tuint32_t count;")
    lines.append("} lens_icon_desc;")
    lines.append("")
    lines.append("extern const lens_icon_desc lens_icon_table[LENS_ICON_COUNT];")
    lines.append("")
    lines.append("#endif")
    return "\n".join(lines) + "\n"


def generate_icon_data_c(icons):
    lines = []
    lines.append('#include "../include/lens/icon.h"')
    lines.append("")

    for name, cmds, _ in icons:
        aname = icon_array_name(name.lower().replace("_", "-"))
        lines.append(f"static const lens_icon_cmd lens_icon_{aname}_cmds[] = {{")
        for cmd in cmds:
            type_id = cmd[0]
            params = cmd[1:]
            params_str = ", ".join(fmt_float(p) for p in params)
            lines.append(f"\t{{{type_id}, {{{params_str}}}}},")
        lines.append("};")
        lines.append("")

    lines.append("const lens_icon_desc lens_icon_table[LENS_ICON_COUNT] = {")
    for name, cmds, _ in icons:
        aname = icon_array_name(name.lower().replace("_", "-"))
        count = len(cmds)
        lines.append(f"\t[LENS_ICON_{name}] = {{ lens_icon_{aname}_cmds, {count} }},")
    lines.append("};")
    lines.append("")

    lines.append("const uint8_t lens_icon_render_modes[LENS_ICON_COUNT] = {")
    for name, _, render_mode in icons:
        if render_mode != ICON_RENDER_STROKE:
            lines.append(f"\t[LENS_ICON_{name}] = {render_mode},")
    lines.append("};")
    return "\n".join(lines) + "\n"


def main():
    icons = []
    for source_dir, render_mode in ICON_SOURCES:
        svg_files = sorted(f for f in os.listdir(source_dir) if f.endswith(".svg"))
        for svg_file in svg_files:
            name = icon_enum_name(svg_file)
            filepath = os.path.join(source_dir, svg_file)
            try:
                cmds = parse_svg(filepath)
                icons.append((name, cmds, svg_render_mode(filepath, render_mode)))
            except Exception as e:
                print(f"Warning: failed to parse {svg_file}: {e}", file=sys.stderr)
                icons.append((name, [], render_mode))

    ICON_H.write_text(generate_icon_h(icons))
    ICON_DATA_C.write_text(generate_icon_data_c(icons))

    print(f"Generated {ICON_H} ({len(icons)} icons)")
    print(f"Generated {ICON_DATA_C}")


if __name__ == "__main__":
    main()
