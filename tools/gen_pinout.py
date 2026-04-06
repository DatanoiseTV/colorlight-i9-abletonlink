#!/usr/bin/env python3
"""
gen_pinout.py — generate docs/pinout.svg directly from the live
platform definitions.

The diagram is laid out to mirror the actual physical Colorlight i9
v7.2 PCB:

      ┌────────── P3 (pmode/pmodf) ──┬─ HDMI ─┬── P2 (pmodc/pmodd) ────┐
      │  pin labels above the board                                     │
      │         ┌──────────── board outline ────────────┐               │
      │         │   ECP5 FPGA           SDRAM           │               │
      │  P4     │                       SPI flash       │      P1       │
      │ labels  │                       JTAG header     │   labels      │
      │         │           ETH PHY 0 / 1                │               │
      │         └────────────────────────────────────────┘               │
      │  pin labels below the board                                     │
      └──────── P5 (pmodi/pmodj) ──────────── P6 (pmodk/pmodl) ─────────┘

Reads:
  - litex_boards.platforms.colorlight_i5
        upstream PMOD pin lists for the i9 v7.2 board.
  - litex_soc.platform_i9
        our function-to-pin map: TDM, MIDI, Eurorack, pulses.

Writes:
  - docs/pinout.svg (light theme)

Run from inside the Docker shell:

    ./docker-build.sh --shell
    python3 tools/gen_pinout.py

or via the wrapper:

    ./docker-build.sh --gen-pinout
"""

import argparse
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(ROOT))

from litex.build.generic_platform import Pins, Subsignal
from litex_boards.platforms import colorlight_i5

from litex_soc import platform_i9


# ---------------------------------------------------------------------------
# Live platform introspection
# ---------------------------------------------------------------------------

def get_v72_connectors():
    out = {}
    for entry in colorlight_i5._connectors_v7_2:
        name, pins_str = entry[0], entry[1]
        out[name] = pins_str.split()
    return out


def get_v72_io():
    return colorlight_i5._io_v7_2


def find_pin_in_connectors(connectors, fpga_pin):
    for name, pins in connectors.items():
        for idx, p in enumerate(pins):
            if p == fpga_pin:
                return (name, idx)
    return None


def extract_subsignal_pin_refs(extension_records):
    """Walk a list returned by _pulse_outputs / _midi_uart /
    _eurorack_inputs and return {subsignal_name: (pmod_name, slot)}."""
    out = {}
    for entry in extension_records:
        if not isinstance(entry, Subsignal):
            continue
        for c in entry.constraints:
            if not isinstance(c, Pins):
                continue
            for ident in c.identifiers:
                if ":" in ident:
                    pmod, slot = ident.split(":")
                    out[entry.name] = (pmod, int(slot))
                    break
    return out


# ---------------------------------------------------------------------------
# Function-to-pin map for the diagram
# ---------------------------------------------------------------------------

LEGEND = [
    ("TDM port 0",     "tdm0"),
    ("TDM port 1",     "tdm1"),
    ("TDM port 2-7",   "tdmn"),
    ("MIDI 31250",     "midi"),
    ("Eurorack input", "euro"),
    ("Beat / pulses",  "pulse"),
    ("on-board fn",    "reserved"),
    ("free GPIO",      "free"),
]

PRETTY = {
    "bck":          "BCK",
    "fs":           "FS",
    "sdout":        "SDOUT",
    "sdin":         "SDIN",
    "mclk":         "MCLK",
    "tx":           "MIDI TX",
    "rx":           "MIDI RX",
    "clk_in":       "EURO CLK",
    "rst_in":       "EURO RST",
    "run_in":       "EURO RUN",
    "clk_24ppqn":   "24 PPQN",
    "clk_48ppqn":   "48 PPQN",
    "clk_96ppqn":   "96 PPQN",
    "beat_clk":     "BEAT CLK",
    "bar_clk":      "BAR CLK",
    "start":        "START",
    "stop":         "STOP",
    "run_level":    "RUN LVL",
    "sync_led":     "SYNC LED",
    "peer_led":     "PEER LED",
    "pps":          "PPS",
}


def build_function_map(num_physical_tdm_ports):
    fmap = {}

    # TDM ports
    indices = platform_i9._TDM_PIN_INDICES
    for port_idx in range(num_physical_tdm_ports):
        if port_idx >= len(platform_i9._TDM_CONNECTORS):
            break
        conn = platform_i9._TDM_CONNECTORS[port_idx]
        css = "tdm0" if port_idx == 0 else "tdm1" if port_idx == 1 else "tdmn"
        for fname, pin_idx in indices.items():
            fmap[(conn, pin_idx)] = (
                PRETTY.get(fname, fname.upper()), css, f"port {port_idx}")

    # MIDI
    for sname, (conn, idx) in extract_subsignal_pin_refs(
            platform_i9._midi_uart()).items():
        fmap[(conn, idx)] = (PRETTY.get(sname, sname.upper()), "midi", "MIDI")

    # Eurorack
    for sname, (conn, idx) in extract_subsignal_pin_refs(
            platform_i9._eurorack_inputs()).items():
        fmap[(conn, idx)] = (PRETTY.get(sname, sname.upper()), "euro", "EURO")

    # Beat / transport pulses
    for sname, (conn, idx) in extract_subsignal_pin_refs(
            platform_i9._pulse_outputs()).items():
        fmap[(conn, idx)] = (PRETTY.get(sname, sname.upper()), "pulse", "GPIO")

    return fmap


def build_reserved_map(connectors, io_records):
    out = {}

    def mark(fpga_pin, label):
        slot = find_pin_in_connectors(connectors, fpga_pin)
        if slot is not None:
            out[slot] = (label, "reserved", "on-board")

    for entry in io_records:
        if len(entry) < 3:
            continue
        sig_name = entry[0]
        if sig_name == "user_led_n":
            for c in entry[2:]:
                if isinstance(c, Pins):
                    for ident in c.identifiers:
                        mark(ident, "USER LED")
        elif sig_name == "cpu_reset_n":
            for c in entry[2:]:
                if isinstance(c, Pins):
                    for ident in c.identifiers:
                        mark(ident, "cpu_reset")
        elif sig_name == "serial":
            for c in entry[2:]:
                if isinstance(c, Subsignal):
                    for inner in c.constraints:
                        if isinstance(inner, Pins):
                            for ident in inner.identifiers:
                                label = "UART TX" if c.name == "tx" else \
                                        "UART RX" if c.name == "rx" else c.name
                                mark(ident, label)
    return out


# ---------------------------------------------------------------------------
# SVG renderer (light theme, board-centric layout)
# ---------------------------------------------------------------------------

CANVAS_W = 1620
CANVAS_H = 1320

# Board outline (centred, room for outside pin strips)
BOARD_X  = 320
BOARD_Y  = 280
BOARD_W  = 980
BOARD_H  = 660

# Pin cell sizes — H_CELL_W is tuned so that two 8-pin strips fit
# inside the board width with healthy gaps and an HDMI marker between
# them.
H_CELL_W = 48
H_CELL_H = 30
V_CELL_W = 124
V_CELL_H = 26
GAP      = 4

# Vertical spacing inside a connector strip card.
TITLE_TO_SUBTITLE   = 16
SUBTITLE_TO_LABEL   = 18
LABEL_TO_FIRST_CELL = 4
ROW_BETWEEN_LABELS  = 18
COL_LABEL_GAP       = 10

PALETTE = {
    "bg":             "#fbfcfe",
    "title":          "#9a4400",
    "subtitle":       "#5b6473",
    "footer":         "#8893a4",
    "board":          "#eef2f9",
    "board_border":   "#c7d0dd",
    "header_strip":   "#d4dbe6",
    "header_label":   "#1c2230",

    "conn_title":     "#1c2230",
    "conn_sub":       "#6b7280",

    "free_bg":        "#eef1f6",
    "free_border":    "#cdd5e0",
    "free_pin":       "#7a8597",
    "free_func":      "#a3acbb",

    "reserved_bg":    "#fde8e6",
    "reserved_border":"#e0473f",
    "reserved_pin":   "#a8201a",
    "reserved_func":  "#a8201a",

    "tdm0_bg":        "#ff8c1a",
    "tdm0_text":      "#ffffff",
    "tdm1_bg":        "#ffae57",
    "tdm1_text":      "#1c2230",
    "tdmn_bg":        "#ffd28a",
    "tdmn_text":      "#1c2230",
    "midi_bg":        "#1f9c5a",
    "midi_text":      "#ffffff",
    "euro_bg":        "#1f7fb5",
    "euro_text":      "#ffffff",
    "pulse_bg":       "#cf2c7a",
    "pulse_text":     "#ffffff",

    "chip_bg":        "#ffffff",
    "chip_border":    "#475063",
    "chip_label":     "#1c2230",
    "chip_sub":       "#6b7280",
    "chip_accent":    "#9a4400",
}


def svg_header():
    p = PALETTE
    return f'''<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {CANVAS_W} {CANVAS_H}"
     font-family="ui-monospace, SFMono-Regular, Menlo, Consolas, monospace">
  <style>
    .bg          {{ fill: {p["bg"]}; }}
    .board       {{ fill: {p["board"]}; stroke: {p["board_border"]}; stroke-width: 2; }}
    .header-strip{{ fill: {p["header_strip"]}; stroke: {p["board_border"]}; stroke-width: 1; }}
    .header-pin  {{ fill: {p["header_label"]}; }}

    .chip        {{ fill: {p["chip_bg"]}; stroke: {p["chip_border"]}; stroke-width: 1.0; }}
    .chip-label  {{ font-size: 12px; fill: {p["chip_label"]}; font-weight: 600; }}
    .chip-sub    {{ font-size: 10px; fill: {p["chip_sub"]}; }}
    .chip-accent {{ font-size: 11px; fill: {p["chip_accent"]}; font-weight: 600; }}

    .title       {{ font-size: 24px; fill: {p["title"]}; font-weight: 700; }}
    .subtitle    {{ font-size: 12px; fill: {p["subtitle"]}; }}
    .footer      {{ font-size: 10px; fill: {p["footer"]}; }}
    .conn-title  {{ font-size: 13px; fill: {p["conn_title"]}; font-weight: 700; }}
    .conn-sub    {{ font-size: 10px; fill: {p["conn_sub"]}; }}

    .pin-fpga    {{ font-size:  9px; font-weight: 700; }}
    .pin-func    {{ font-size:  9px; }}
    .pin-fpga-h  {{ font-size:  9px; font-weight: 700; }}
    .pin-func-h  {{ font-size:  9px; }}

    .pin-cell.free rect          {{ fill: {p["free_bg"]}; stroke: {p["free_border"]}; stroke-width: 0.6; }}
    .pin-cell.free .pin-fpga,
    .pin-cell.free .pin-fpga-h   {{ fill: {p["free_pin"]}; }}
    .pin-cell.free .pin-func,
    .pin-cell.free .pin-func-h   {{ fill: {p["free_func"]}; }}

    .pin-cell.reserved rect      {{ fill: {p["reserved_bg"]}; stroke: {p["reserved_border"]}; stroke-width: 1.2; }}
    .pin-cell.reserved .pin-fpga,
    .pin-cell.reserved .pin-fpga-h{{ fill: {p["reserved_pin"]}; }}
    .pin-cell.reserved .pin-func,
    .pin-cell.reserved .pin-func-h{{ fill: {p["reserved_func"]}; }}

    .pin-cell.tdm0 rect          {{ fill: {p["tdm0_bg"]}; }}
    .pin-cell.tdm0 .pin-fpga,
    .pin-cell.tdm0 .pin-fpga-h,
    .pin-cell.tdm0 .pin-func,
    .pin-cell.tdm0 .pin-func-h   {{ fill: {p["tdm0_text"]}; }}

    .pin-cell.tdm1 rect          {{ fill: {p["tdm1_bg"]}; }}
    .pin-cell.tdm1 .pin-fpga,
    .pin-cell.tdm1 .pin-fpga-h,
    .pin-cell.tdm1 .pin-func,
    .pin-cell.tdm1 .pin-func-h   {{ fill: {p["tdm1_text"]}; }}

    .pin-cell.tdmn rect          {{ fill: {p["tdmn_bg"]}; }}
    .pin-cell.tdmn .pin-fpga,
    .pin-cell.tdmn .pin-fpga-h,
    .pin-cell.tdmn .pin-func,
    .pin-cell.tdmn .pin-func-h   {{ fill: {p["tdmn_text"]}; }}

    .pin-cell.midi rect          {{ fill: {p["midi_bg"]}; }}
    .pin-cell.midi .pin-fpga,
    .pin-cell.midi .pin-fpga-h,
    .pin-cell.midi .pin-func,
    .pin-cell.midi .pin-func-h   {{ fill: {p["midi_text"]}; }}

    .pin-cell.euro rect          {{ fill: {p["euro_bg"]}; }}
    .pin-cell.euro .pin-fpga,
    .pin-cell.euro .pin-fpga-h,
    .pin-cell.euro .pin-func,
    .pin-cell.euro .pin-func-h   {{ fill: {p["euro_text"]}; }}

    .pin-cell.pulse rect         {{ fill: {p["pulse_bg"]}; }}
    .pin-cell.pulse .pin-fpga,
    .pin-cell.pulse .pin-fpga-h,
    .pin-cell.pulse .pin-func,
    .pin-cell.pulse .pin-func-h  {{ fill: {p["pulse_text"]}; }}
  </style>

  <rect class="bg" width="{CANVAS_W}" height="{CANVAS_H}"/>
'''


def svg_footer():
    return "</svg>\n"


# ---- pin renderers --------------------------------------------------------

def lookup_pin(pmod_name, slot, fmap, reserved_map):
    """Return (label, css_class) for a (pmod, slot)."""
    key = (pmod_name, slot)
    if key in fmap:
        return fmap[key][0], fmap[key][1]
    if key in reserved_map:
        return reserved_map[key][0], reserved_map[key][1]
    return ("free", "free")


def horiz_pin_cell(x, y, fpga, label, css):
    return (
        f'    <g class="pin-cell {css}" transform="translate({x},{y})">\n'
        f'      <rect width="{H_CELL_W}" height="{H_CELL_H}" rx="3"/>\n'
        f'      <text class="pin-fpga-h" x="{H_CELL_W/2}" y="11" text-anchor="middle">{fpga}</text>\n'
        f'      <text class="pin-func-h" x="{H_CELL_W/2}" y="22" text-anchor="middle">{label}</text>\n'
        f'    </g>\n'
    )


def vert_pin_cell(x, y, fpga, label, css):
    return (
        f'    <g class="pin-cell {css}" transform="translate({x},{y})">\n'
        f'      <rect width="{V_CELL_W}" height="{V_CELL_H}" rx="3"/>\n'
        f'      <text class="pin-fpga" x="6"  y="17">{fpga}</text>\n'
        f'      <text class="pin-func" x="42" y="17">{label}</text>\n'
        f'    </g>\n'
    )


def render_horizontal_strip(pmods, fmap, reserved_map, x0, y0, title, subtitle):
    """Render a horizontal connector strip with two PMOD rows of 8 cells.

    Layout (top of strip = y0):

        title          ← y0 + 12
        subtitle       ← y0 + 28
        pmod_a label   ← y0 + 50
        row 0 cells    ← y0 + 54  (height 26)
        pmod_b label   ← y0 + 102
        row 1 cells    ← y0 + 106
        last cell ends ← y0 + 132

    Total card height ≈ 132 px.
    """
    out = []
    title_y    = y0 + 12
    subtitle_y = title_y + TITLE_TO_SUBTITLE
    out.append(
        f'  <text class="conn-title" x="{x0}" y="{title_y}">{title}</text>\n'
        f'  <text class="conn-sub"   x="{x0}" y="{subtitle_y}">{subtitle}</text>\n'
    )
    first_label_y = subtitle_y + SUBTITLE_TO_LABEL
    for row, (pmod_name, pins) in enumerate(pmods):
        label_y = first_label_y + row * (H_CELL_H + LABEL_TO_FIRST_CELL + ROW_BETWEEN_LABELS)
        ry      = label_y + LABEL_TO_FIRST_CELL
        out.append(
            f'  <text class="conn-sub" x="{x0}" y="{label_y}">{pmod_name}</text>\n'
        )
        for col, fpga in enumerate(pins):
            cx = x0 + col * (H_CELL_W + GAP)
            if fpga == "-":
                label, css = "—", "free"
            else:
                label, css = lookup_pin(pmod_name, col, fmap, reserved_map)
            out.append(horiz_pin_cell(cx, ry, fpga, label, css))
    return "".join(out)


def render_vertical_strip(pmods, fmap, reserved_map, x0, y0, title, subtitle):
    """Render a vertical connector strip with two PMOD columns of 8 cells.

    Layout (top of card = y0):

        title          ← y0 + 12
        subtitle       ← y0 + 28
        col labels     ← y0 + 50
        cells          ← y0 + 54 .. y0 + 54 + 8 * (V_CELL_H + GAP) - GAP

    Total card height ≈ 290 px.
    """
    out = []
    title_y    = y0 + 12
    subtitle_y = title_y + TITLE_TO_SUBTITLE
    out.append(
        f'  <text class="conn-title" x="{x0}" y="{title_y}">{title}</text>\n'
        f'  <text class="conn-sub"   x="{x0}" y="{subtitle_y}">{subtitle}</text>\n'
    )
    label_y     = subtitle_y + SUBTITLE_TO_LABEL
    first_cell_y = label_y + LABEL_TO_FIRST_CELL
    for col, (pmod_name, pins) in enumerate(pmods):
        cx = x0 + col * (V_CELL_W + COL_LABEL_GAP)
        out.append(
            f'  <text class="conn-sub" x="{cx}" y="{label_y}">{pmod_name}</text>\n'
        )
        for row, fpga in enumerate(pins):
            ry = first_cell_y + row * (V_CELL_H + GAP)
            if fpga == "-":
                label, css = "—", "free"
            else:
                label, css = lookup_pin(pmod_name, row, fmap, reserved_map)
            out.append(vert_pin_cell(cx, ry, fpga, label, css))
    return "".join(out)


def render_header_marker(x, y, w, h):
    """Tiny visual marker on the board edge for a PCB header strip."""
    return (
        f'  <rect class="header-strip" x="{x}" y="{y}" '
        f'width="{w}" height="{h}" rx="2"/>\n'
    )


def render_chip(x, y, w, h, label, sub, accent=False):
    accent_cls = "chip-accent" if accent else "chip-label"
    return (
        f'  <rect class="chip" x="{x}" y="{y}" width="{w}" height="{h}" rx="4"/>\n'
        f'  <text class="{accent_cls}" x="{x + w/2}" y="{y + 18}" '
        f'text-anchor="middle">{label}</text>\n'
        f'  <text class="chip-sub"     x="{x + w/2}" y="{y + 32}" '
        f'text-anchor="middle">{sub}</text>\n'
    )


def render_svg(num_physical_tdm_ports=2):
    connectors    = get_v72_connectors()
    io_records    = get_v72_io()
    fmap          = build_function_map(num_physical_tdm_ports)
    reserved_map  = build_reserved_map(connectors, io_records)

    out = [svg_header()]

    # ---- title ---------------------------------------------------
    out.append(
        f'  <text class="title" x="{CANVAS_W//2}" y="40" text-anchor="middle">'
        f'LinkFPGA — Colorlight i9 v7.2 pinout</text>\n'
        f'  <text class="subtitle" x="{CANVAS_W//2}" y="62" text-anchor="middle">'
        f'auto-generated from the live platform definition · '
        f'num_physical_tdm_ports = {num_physical_tdm_ports} · '
        f'coloured cells = used by LinkFPGA, grey cells = free GPIO'
        f'</text>\n'
    )

    # ---- board outline -------------------------------------------
    out.append(
        f'  <rect class="board" x="{BOARD_X}" y="{BOARD_Y}" '
        f'width="{BOARD_W}" height="{BOARD_H}" rx="14"/>\n'
    )

    # ---- header markers (visual hint of the actual PCB headers) --
    # Strip width matches the marker width below so the cells line up
    # vertically with the silkscreen.
    strip_w = 8 * H_CELL_W + 7 * GAP                  # = 8*48 + 7*4 = 412
    side_margin = 30
    hdmi_w = max(60, BOARD_W - 2 * (side_margin + strip_w) - 4)
    hdmi_w = 60                                       # fixed for the HDMI marker
    inner_gap = (BOARD_W - 2 * side_margin - 2 * strip_w - hdmi_w) / 2

    p3_x = BOARD_X + side_margin
    hdmi_x = p3_x + strip_w + inner_gap
    p2_x = hdmi_x + hdmi_w + inner_gap

    out.append(render_header_marker(p3_x, BOARD_Y + 16, strip_w, 22))
    out.append(
        f'  <text class="conn-sub" x="{p3_x + strip_w/2}" y="{BOARD_Y + 32}" '
        f'text-anchor="middle">P3 header</text>\n'
    )
    out.append(render_header_marker(hdmi_x, BOARD_Y + 16, hdmi_w, 22))
    out.append(
        f'  <text class="chip-accent" x="{hdmi_x + hdmi_w/2}" y="{BOARD_Y + 32}" '
        f'text-anchor="middle">HDMI</text>\n'
    )
    out.append(render_header_marker(p2_x, BOARD_Y + 16, strip_w, 22))
    out.append(
        f'  <text class="conn-sub" x="{p2_x + strip_w/2}" y="{BOARD_Y + 32}" '
        f'text-anchor="middle">P2 header</text>\n'
    )

    # P5 + P6 along the bottom edge
    p5_x = p3_x
    p6_x = p2_x
    bot_y = BOARD_Y + BOARD_H - 38
    out.append(render_header_marker(p5_x, bot_y, strip_w, 22))
    out.append(
        f'  <text class="conn-sub" x="{p5_x + strip_w/2}" y="{bot_y + 16}" '
        f'text-anchor="middle">P5 header</text>\n'
    )
    out.append(render_header_marker(p6_x, bot_y, strip_w, 22))
    out.append(
        f'  <text class="conn-sub" x="{p6_x + strip_w/2}" y="{bot_y + 16}" '
        f'text-anchor="middle">P6 header</text>\n'
    )

    # P4 along the left edge, P1 along the right edge
    out.append(render_header_marker(BOARD_X + 16, BOARD_Y + 80, 22, 460))
    out.append(
        f'  <text class="conn-sub" x="{BOARD_X + 27}" y="{BOARD_Y + 320}" '
        f'text-anchor="middle" transform="rotate(-90 {BOARD_X + 27} {BOARD_Y + 320})">'
        f'P4 header</text>\n'
    )
    out.append(render_header_marker(BOARD_X + BOARD_W - 38, BOARD_Y + 80, 22, 460))
    out.append(
        f'  <text class="conn-sub" x="{BOARD_X + BOARD_W - 27}" y="{BOARD_Y + 320}" '
        f'text-anchor="middle" transform="rotate(90 {BOARD_X + BOARD_W - 27} {BOARD_Y + 320})">'
        f'P1 header</text>\n'
    )

    # ---- chip markers inside the board ---------------------------
    # FPGA roughly in the centre, biased toward the lower half (matches
    # the actual PCB silkscreen).
    fpga_x = BOARD_X + (BOARD_W - 260) / 2
    fpga_y = BOARD_Y + 280
    out.append(
        f'  <rect class="chip" x="{fpga_x}" y="{fpga_y}" width="260" height="160" rx="8"/>\n'
        f'  <text class="chip-label" x="{fpga_x + 130}" y="{fpga_y + 60}" '
        f'text-anchor="middle">Lattice ECP5</text>\n'
        f'  <text class="chip-label" x="{fpga_x + 130}" y="{fpga_y + 78}" '
        f'text-anchor="middle">LFE5U-45F-6BG381C</text>\n'
        f'  <text class="chip-sub"   x="{fpga_x + 130}" y="{fpga_y + 100}" '
        f'text-anchor="middle">VexRiscv + lwIP</text>\n'
        f'  <text class="chip-sub"   x="{fpga_x + 130}" y="{fpga_y + 116}" '
        f'text-anchor="middle">+ Link / Link-Audio / MidiCore / Eurorack</text>\n'
        f'  <text class="chip-sub"   x="{fpga_x + 130}" y="{fpga_y + 136}" '
        f'text-anchor="middle">45 K LUT4 · 108 KiB BRAM · 50 MHz sys</text>\n'
    )

    # ETH PHY 0 / 1 (right side, near P1)
    eth_x = BOARD_X + BOARD_W - 200
    out.append(render_chip(eth_x, BOARD_Y + 100, 140, 44,
                           "ETH PHY 0", "B50612D · used"))
    out.append(render_chip(eth_x, BOARD_Y + 156, 140, 44,
                           "ETH PHY 1", "B50612D · idle"))

    # SDRAM + SPI flash (left/upper)
    sdram_x = BOARD_X + 60
    out.append(render_chip(sdram_x, BOARD_Y + 100, 140, 44,
                           "SDRAM 8 MB", "M12L64322A"))
    out.append(render_chip(sdram_x, BOARD_Y + 156, 140, 44,
                           "SPI flash", "W25Q · 16 Mbit"))

    # JTAG / clocks / LED / reset row in the lower middle
    util_y = BOARD_Y + BOARD_H - 100
    util_x = BOARD_X + 60
    util_specs = [
        ("JTAG header", "TCK TMS TDI TDO"),
        ("USER LED",    "L2 · active-low"),
        ("cpu_reset",   "K18 · pull-up"),
        ("25 MHz xtal", "P3 → 2 PLLs"),
        ("UART console","J17/H18 · 115200"),
    ]
    util_cell_w = 142
    util_cell_h = 44
    util_gap = 8
    total_w = len(util_specs) * util_cell_w + (len(util_specs) - 1) * util_gap
    util_x0 = BOARD_X + (BOARD_W - total_w) / 2
    for i, (lab, sub) in enumerate(util_specs):
        cx = util_x0 + i * (util_cell_w + util_gap)
        out.append(render_chip(cx, util_y, util_cell_w, util_cell_h, lab, sub))

    # ---- TOP horizontal connector strips (P3, P2) -----------------
    # Strips sit just above the board so the cells line up with the
    # silkscreen header markers.
    top_strip_y = BOARD_Y - 152
    out.append(render_horizontal_strip(
        [("pmode", connectors["pmode"]),
         ("pmodf", connectors["pmodf"])],
        fmap, reserved_map,
        x0=p3_x, y0=top_strip_y,
        title="P3", subtitle="pmode → TDM port 0   ·   pmodf → TDM port 1"
    ))
    out.append(render_horizontal_strip(
        [("pmodc", connectors["pmodc"]),
         ("pmodd", connectors["pmodd"])],
        fmap, reserved_map,
        x0=p2_x, y0=top_strip_y,
        title="P2", subtitle="pmodc → MIDI + Eurorack   ·   pmodd → free"
    ))

    # ---- BOTTOM horizontal connector strips (P5, P6) -------------
    bot_strip_y = BOARD_Y + BOARD_H + 30
    out.append(render_horizontal_strip(
        [("pmodi", connectors["pmodi"]),
         ("pmodj", connectors["pmodj"])],
        fmap, reserved_map,
        x0=p5_x, y0=bot_strip_y,
        title="P5", subtitle="pmodi → free   ·   pmodj → free  (J17/H18 = UART)"
    ))
    out.append(render_horizontal_strip(
        [("pmodk", connectors["pmodk"]),
         ("pmodl", connectors["pmodl"])],
        fmap, reserved_map,
        x0=p6_x, y0=bot_strip_y,
        title="P6", subtitle="pmodk → free   ·   pmodl → free"
    ))

    # ---- LEFT vertical connector strip (P4) ----------------------
    out.append(render_vertical_strip(
        [("pmodg", connectors["pmodg"]),
         ("pmodh", connectors["pmodh"])],
        fmap, reserved_map,
        x0=18, y0=BOARD_Y + 28,
        title="P4", subtitle="pmodg = free  ·  pmodh = beat/transport"
    ))

    # ---- RIGHT vertical connector strip (P1) ---------------------
    # P1 is the dual ETH header — show ETH PHY signal info as cells
    # in the same visual style for consistency.
    eth_pmods = [
        ("PHY0", [
            ("U19", "TX clk", "free"),
            ("U20", "TXD0",  "free"),
            ("T19", "TXD1",  "free"),
            ("T20", "TXD2",  "free"),
            ("R20", "TXD3",  "free"),
            ("P19", "TX_EN", "free"),
            ("L19", "RX clk","free"),
            ("M20", "RX_DV", "free"),
        ]),
        ("PHY1", [
            ("G1",  "TX clk", "free"),
            ("G2",  "TXD0",  "free"),
            ("H1",  "TXD1",  "free"),
            ("J1",  "TXD2",  "free"),
            ("J3",  "TXD3",  "free"),
            ("K1",  "TX_EN", "free"),
            ("H2",  "RX clk","free"),
            ("P2",  "RX_DV", "free"),
        ]),
    ]
    eth_x0 = CANVAS_W - 18 - (V_CELL_W * 2 + COL_LABEL_GAP)
    eth_y0 = BOARD_Y + 28
    eth_title_y    = eth_y0 + 12
    eth_subtitle_y = eth_title_y + TITLE_TO_SUBTITLE
    eth_label_y    = eth_subtitle_y + SUBTITLE_TO_LABEL
    eth_first_cell = eth_label_y + LABEL_TO_FIRST_CELL
    out.append(
        f'  <text class="conn-title" x="{eth_x0}" y="{eth_title_y}">P1</text>\n'
        f'  <text class="conn-sub"   x="{eth_x0}" y="{eth_subtitle_y}">'
        f'dual GbE  ·  PHY0 used by LiteEth  ·  PHY1 idle</text>\n'
    )
    for col, (label, pins) in enumerate(eth_pmods):
        cx = eth_x0 + col * (V_CELL_W + COL_LABEL_GAP)
        out.append(
            f'  <text class="conn-sub" x="{cx}" y="{eth_label_y}">{label}</text>\n'
        )
        for row, (fpga, fn, css) in enumerate(pins):
            ry = eth_first_cell + row * (V_CELL_H + GAP)
            # ETH PHY 0 is "used" by LiteEth — gentle highlight.
            use_css = "tdm1" if label == "PHY0" else "free"
            out.append(vert_pin_cell(cx, ry, fpga, fn, use_css))

    # ---- legend --------------------------------------------------
    legend_y = CANVAS_H - 90
    cell_w = 134
    legend_total_w = len(LEGEND) * cell_w + (len(LEGEND) - 1) * GAP
    legend_x0 = (CANVAS_W - legend_total_w) / 2
    out.append(
        f'  <text class="conn-title" x="{CANVAS_W//2}" y="{legend_y - 16}" '
        f'text-anchor="middle">legend</text>\n'
    )
    for i, (label, css) in enumerate(LEGEND):
        lx = legend_x0 + i * (cell_w + GAP)
        out.append(
            f'    <g class="pin-cell {css}" transform="translate({lx},{legend_y})">\n'
            f'      <rect width="{cell_w}" height="{V_CELL_H}" rx="3"/>\n'
            f'      <text class="pin-fpga" x="{cell_w/2}" y="17" text-anchor="middle">{label}</text>\n'
            f'    </g>\n'
        )

    out.append(
        f'  <text class="footer" x="{CANVAS_W//2}" y="{CANVAS_H - 16}" '
        f'text-anchor="middle">'
        f'generated by tools/gen_pinout.py — re-run with `./docker-build.sh --gen-pinout`'
        f'</text>\n'
    )

    out.append(svg_footer())
    return "".join(out)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    p = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--num-physical-tdm-ports", "-n", type=int, default=2,
        help="Number of physical TDM16 ports the diagram should reflect "
             "(matches your `--num-physical-tdm-ports` build flag).")
    p.add_argument("--out", "-o", default=str(ROOT / "docs" / "pinout.svg"),
        help="Output file (default: docs/pinout.svg).")
    args = p.parse_args()

    svg = render_svg(num_physical_tdm_ports=args.num_physical_tdm_ports)
    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(svg)
    print(f"wrote {out_path} ({len(svg)} bytes, {svg.count(chr(10))} lines)")


if __name__ == "__main__":
    main()
