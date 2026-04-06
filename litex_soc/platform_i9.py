"""
Maps the Colorlight i9 v7.2 PMOD pinout to LinkFPGA's TDM16 ports and
pulse-output signals so the rest of the SoC can request them by name.

The i9 v7.2 board exposes its user GPIO via PMOD-style connectors,
defined by `litex_boards.platforms.colorlight_i5` as:
    pmode, pmodf, pmodd, pmodg, pmodh, pmodi, pmodj, pmodk, pmodl
(`pmodc` shares pins with `cpu_reset_n` and `user_led_n` on the v7.2
revision and is therefore avoided here.)

Each PMOD provides 8 LVCMOS3.3 pins, except `pmodh` which has 6 usable
pins (the 1st and 5th are GND/VCC).

LinkFPGA supports a configurable number of **TDM16** ports — 16 channels
per port, 16-bit linear PCM, single serial data line per direction:

    BCK   = 16 * 16 * fs  (= 12.288 MHz at 48 kHz)
    FS    = fs            (= 48 kHz, frame-sync, falling edge marks slot 0)
    SDOUT = 1 line, 16 channels TDM-multiplexed
    SDIN  = 1 line, 16 channels TDM-multiplexed
    MCLK  = 24.576 MHz master clock (optional, for codecs that need it)

Each physical TDM16 port therefore needs 5 pins. We allocate one full
PMOD per TDM16 port (5 used + 3 spare), giving up to **8 physical TDM16
ports** on the i9 v7.2.

Default allocation:
    pmode  → TDM16 port 0
    pmodf  → TDM16 port 1
    pmodd  → TDM16 port 2
    pmodg  → TDM16 port 3
    pmodi  → TDM16 port 4
    pmodj  → TDM16 port 5
    pmodk  → TDM16 port 6
    pmodl  → TDM16 port 7
    pmodh  → beat / transport pulse outputs (6 usable pins)

If the user requests more ports than the 8 physical slots above, the
extras are instantiated as **virtual** TDM16 ports — they exist as real
network endpoints (Link-Audio channels) and as DMA buffers in SDRAM,
but they have no associated physical pins. They are useful for
software-only audio processing (e.g. fan-out from one network input to
several DAW outputs without leaving the FPGA).
"""

from litex.build.generic_platform import IOStandard, Pins, Subsignal


# Per-PMOD pin slots used by one TDM16 port. Indices into the
# connector's pin list (0..7).
_TDM_PIN_INDICES = {
    "bck":   0,
    "fs":    1,
    "sdout": 2,
    "sdin":  3,
    "mclk":  4,
}

# Ordered list of PMODs available for TDM16 ports.
_TDM_CONNECTORS = [
    "pmode", "pmodf", "pmodd", "pmodg",
    "pmodi", "pmodj", "pmodk", "pmodl",
]

#: How many physical TDM16 ports the board can host.
MAX_PHYSICAL_TDM_PORTS = len(_TDM_CONNECTORS)


def _tdm_port(slot_index):
    """Return Subsignal records for one physical TDM16 port mapped to
    PMOD `_TDM_CONNECTORS[slot_index]`."""
    if slot_index >= MAX_PHYSICAL_TDM_PORTS:
        raise ValueError(
            f"TDM16 slot {slot_index} is beyond the {MAX_PHYSICAL_TDM_PORTS} "
            "physical slots — request a virtual port instead.")
    conn = _TDM_CONNECTORS[slot_index]
    return [
        Subsignal("bck",   Pins(f"{conn}:{_TDM_PIN_INDICES['bck']}")),
        Subsignal("fs",    Pins(f"{conn}:{_TDM_PIN_INDICES['fs']}")),
        Subsignal("sdout", Pins(f"{conn}:{_TDM_PIN_INDICES['sdout']}")),
        Subsignal("sdin",  Pins(f"{conn}:{_TDM_PIN_INDICES['sdin']}")),
        Subsignal("mclk",  Pins(f"{conn}:{_TDM_PIN_INDICES['mclk']}")),
        IOStandard("LVCMOS33"),
    ]


def _pulse_outputs():
    """Beat / transport pulse outputs on `pmodh`.

    `pmodh` has the layout `"- E19 B3 K5 - B2 K4 A2"`, so usable pin
    indices are 1, 2, 3, 5, 6, 7 (= 6 pins). We map the six most
    critical pulses; the less critical clk_48/96PPQN, bar_clk and
    secondary LEDs are not pinned and are still computed by
    `BeatPulseGen` for use by external monitoring."""
    return [
        Subsignal("clk_24ppqn", Pins("pmodh:1")),
        Subsignal("start",      Pins("pmodh:2")),
        Subsignal("stop",       Pins("pmodh:3")),
        Subsignal("beat_clk",   Pins("pmodh:5")),
        Subsignal("run_level",  Pins("pmodh:6")),
        Subsignal("sync_led",   Pins("pmodh:7")),
        IOStandard("LVCMOS33"),
    ]


def attach_link_io(platform, num_physical_tdm_ports=2):
    """Attach the LinkFPGA IO groups to the colorlight_i5 platform
    (configured for board=i9, revision=7.2).

    Args:
        platform: a `litex_boards.platforms.colorlight_i5.Platform` built
            with `board="i9", revision="7.2"`.
        num_physical_tdm_ports: how many physical TDM16 ports to expose
            on the PMOD connectors. Must be in 0..MAX_PHYSICAL_TDM_PORTS.

    After this call:
        platform.request("link_tdm",   i)   # i in 0..num_physical_tdm_ports-1
        platform.request("link_pulse", 0)
    """
    if not 0 <= num_physical_tdm_ports <= MAX_PHYSICAL_TDM_PORTS:
        raise ValueError(
            f"num_physical_tdm_ports must be 0..{MAX_PHYSICAL_TDM_PORTS}, "
            f"got {num_physical_tdm_ports}")

    extensions = []
    for i in range(num_physical_tdm_ports):
        extensions.append(("link_tdm", i, *_tdm_port(i)))
    extensions.append(("link_pulse", 0, *_pulse_outputs()))
    platform.add_extension(extensions)
