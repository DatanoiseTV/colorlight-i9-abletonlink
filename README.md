# LinkFPGA — Ableton Link in hardware

[![spec](https://img.shields.io/badge/spec-LINK__PROTOCOL__SPEC.md-7fd1ff)](../LINK_PROTOCOL_SPEC.md)
[![board](https://img.shields.io/badge/board-Colorlight%20i9%20v7.2-ffae57)](https://github.com/wuxx/Colorlight-FPGA-Projects/blob/master/colorlight_i9_v7.2.md)
[![toolchain](https://img.shields.io/badge/toolchain-OSS%20CAD%20Suite-5fdc7e)](https://github.com/YosysHQ/oss-cad-suite-build)

> **Repo:** `git@github.com:DatanoiseTV/colorlight-i9-abletonlink.git`

A LiteX / Migen SoC for the **Colorlight i9 v7.2**
(Lattice ECP5 `LFE5U-45F-6BG381C`) that implements the Ableton Link and
Link-Audio protocols described in
[`../LINK_PROTOCOL_SPEC.md`](../LINK_PROTOCOL_SPEC.md). The goal: "Link,
but as a black-box stomp on your studio rack" — gigabit Ethernet in,
multi-channel TDM audio + beat-clock pulses out, configured from a tiny
built-in web UI. No DAW, no host PC, no audio driver.

```
                +-----------------------------------------------------+
                |                  Colorlight i9 v7.2                 |
   GbE  ─>──────┤  RGMII  ┌─────────┐         ┌────────────────────┐  |
   PHY0         |         │ LiteEth │─UDP/TCP─│  VexRiscv softcore │  |
                |         │   MAC   │         │  (link, link-audio,│  |
                |         └────┬────┘         │   session, webui)  │  |
                |              │              └─────┬──────────────┘  |
                |              ▼                    │ CSR             |
                |        ┌───────────┐              │                 |
                |        │LinkPktFilt│──── irq ─────┤                 |
                |        └───────────┘              │                 |
                |     ┌────────┬─────────┐          │                 |
                |     ▼        ▼         ▼          ▼                 |
                |  ┌──────┐ ┌───────┐ ┌─────┐  ┌──────────┐           |
                |  │Ghost │ │ Beat  │ │ N × │  │ HTTP     │           |
                |  │Time  │ │Pulse  │ │TDM16│  │ admin UI │           |
                |  │64bit │ │ Gen   │ │Core │  └──────────┘           |
                |  └──┬───┘ └───┬───┘ └──┬──┘                         |
                |     │         │        │                            |
                |  HUB75-J3 ◄───┘        ▼ HUB75-J1/J2 (TDM I/O)      |
                +-----------------------------------------------------+
```

## What it does

* Joins / leaves Ableton Link sessions on the local network just like a
  DAW would.
* Drives **N × TDM16 audio I/O ports** (configurable, default 2,
  default 16 ch in / 16 ch out per port) so you can hand audio to/from
  external codecs at 16-bit 48 kHz.
* Generates **DIN-sync style transport pulses**: 24/48/96 PPQN beat
  clock, downbeat / bar clock, START / STOP one-shots, RUN level — all
  on dedicated GPIO pins, all sample-aligned to the shared Link beat
  grid via the on-chip ghost-time unit.
* Exposes a tiny **HTTP admin UI** on port 80 with status, peer table,
  tempo / play controls, and HTTP-Basic auth.
* Supports **IPv4 *and* IPv6** for Link discovery (the IPv6 path is a
  bare-metal multicast layer on top of LiteEth's raw frame interface;
  see `firmware/ipv6.c`).
* "**Virtual TDM16 ports**": you can ask for more TDM16 ports than the
  board has physical pin slots and the extras are instantiated as
  software-only endpoints (DMA buffers, IRQ, full Link-Audio
  participation — just no physical I/O). Useful for in-FPGA fan-out and
  audio routing.

## Hardware vs. firmware split

The protocol spec calls out which parts deserve hardware acceleration
(see `LINK_PROTOCOL_SPEC.md` §11). This implementation follows that
split:

| Concern | HW (Migen) | FW (VexRiscv) |
|---|---|---|
| Ethernet MAC + UDP demux | LiteEth | — |
| Drop non-Link UDP at line rate | `LinkPacketFilter` (8-byte cmp) | — |
| TLV parse, peer table, BYEBYE | — | `link.c` |
| Heartbeat scheduler | — | `link.c` (uses HW µs counter) |
| Ghost-time arithmetic (host↔ghost) | `GhostTimeUnit` (64-bit) | thin C wrapper |
| Median filter (100 samples) | — | `median.c` |
| Session election + remeasurement | — | `session.c` |
| Beat clock pulses (24/48/96 PPQN, bar) | `BeatPulseGen` | sets next-beat target via CSR |
| Start/Stop GPIO pulses | `BeatPulseGen` | sets `play/stop` flags via CSR |
| TDM serialise/deserialise (16 ch in / 16 ch out per port) | `TDM16Core` | DMA pointers + IRQ |
| AudioBuffer pack/unpack (BE i16) | — | `link_audio.c` |
| HTTP admin UI + Basic auth | — | `http_server.c` + `webui.c` |
| IPv6 multicast (link-local) | — | `ipv6.c` (raw Eth frames) |

The principle: **anything that has to happen at sample rate or sub-µs
goes in gateware. Anything that happens once per packet (a few thousand
times per second at most) lives in firmware.** The VexRiscv at 75 MHz
has way more cycles per Link event than it needs.

## Hardware specs

* **FPGA:** Lattice ECP5 `LFE5U-45F-6BG381C` (45K LUT4, 108 KiB BRAM)
* **System clock:** 75 MHz from the on-board 25 MHz crystal via PLL
* **CPU:** VexRiscv `standard+debug`, ~1.5 DMIPS/MHz, 4 KiB I+D cache
* **RAM:** on-board 32 Mbit SDR SDRAM (M12L64322A) via LiteDRAM
* **Boot ROM:** 16 Mbit SPI flash (W25Q128 or similar)
* **Network:** GbE PHY0 (RGMII, Broadcom B50612D), LiteEth MAC + UDP/TCP
* **Audio:** N × TDM16 (default N=2 = 32 ch in / 32 ch out)
  * 16-bit linear PCM, 48 kHz (matches the protocol's `kPCM_i16` codec)
  * BCK = 12.288 MHz (= 16 × 16 × 48000)
  * MCLK = 24.576 MHz, falling-edge FS marks slot 0
* **Pulse outputs:** 24/48/96 PPQN beat clock, downbeat, bar, START,
  STOP, RUN, SYNC LED, PEER LED, 1-PPS

## Pinout (HUB75 connectors → studio I/O)

Pin labels follow the Colorlight i9 v7.2 silk for the HUB75 connectors
J1 / J2 / J3. All outputs are 3.3 V LVCMOS.

### J1 — TDM16 ports 0 (and 1, if requested)

Each TDM16 slot is `BCK / FS / SDOUT / SDIN / MCLK` = 5 pins. J1 holds
the lower-half pins for slot 0 and the upper-half pins for slot 1.

| HUB75 pin | Slot 0 (port 0)  | Slot 1 (port 1)  |
|-----------|------------------|------------------|
| R0 | `BCK_0`              |                  |
| G0 | `FS_0`               |                  |
| B0 | `SDOUT_0`            |                  |
| R1 | `SDIN_0`             |                  |
| G1 | `MCLK_0`             |                  |
| B1 |                      | `BCK_1`          |
| A  |                      | `FS_1`           |
| B  |                      | `SDOUT_1`        |
| C  |                      | `SDIN_1`         |
| D  |                      | `MCLK_1`         |

### J2 — TDM16 ports 2 (and 3, if requested)

Same layout as J1 but for ports 2 and 3.

### J3 — Beat / transport pulse outputs

| HUB75 pin | Signal       | Notes |
|-----------|--------------|-------|
| R0 | `CLK_24PPQN`  | 24 pulses per quarter note (DIN-sync style) |
| G0 | `CLK_48PPQN`  | |
| B0 | `CLK_96PPQN`  | |
| R1 | `BEAT_CLK`    | 1 pulse per beat |
| G1 | `BAR_CLK`     | 1 pulse per bar (configurable: 4/4 default) |
| B1 | `START_PULSE` | One-shot on transition `stopped→playing` |
| A  | `STOP_PULSE`  | One-shot on transition `playing→stopped` |
| B  | `RUN_LEVEL`   | High while playing |
| C  | `SYNC_LED`    | High when locked to a session |
| D  | `PEER_LED`    | High when ≥1 peer is present |
| E  | `PPS`         | 1 pulse per second from the ghost-time unit |

Pulse width is 1 ms (configurable per CSR).

### J4 — reserved

Left unconnected by default. Easy to extend.

### "Virtual" ports

If you ask for more TDM16 ports than the board has physical slots
(currently 4), the extras are instantiated as software-only endpoints:
no pins, but they appear in the firmware's TDM core list, generate the
same per-frame IRQ, and can carry Link-Audio traffic in and out of
SDRAM-backed sample buffers.

Example: `--num-tdm-ports 6 --num-physical-tdm-ports 2` gives you two
real TDM16 codecs out the HUB75 ports plus four virtual ports for
purely-software audio routing inside the FPGA.

## Repository layout

```
colorlight-i9-abletonlink/
└── LinkFPGA/
    ├── README.md                  ← this file
    ├── Dockerfile                 ← reproducible toolchain image
    ├── docker-build.sh            ← one-shot Docker wrapper
    ├── build.py                   ← LiteX build entry point
    ├── litex_soc/
    │   ├── soc.py                 ← top-level SoC (subclass of SoCCore)
    │   ├── platform_i9.py         ← extra IO for HUB75 → TDM/pulse mapping
    │   ├── ghost_time.py          ← Migen module: 64-bit µs counter + xform
    │   ├── beat_pulse.py          ← Migen module: pulse generator
    │   ├── tdm.py                 ← Migen module: TDM16 serdes
    │   └── link_filter.py         ← Migen module: protocol header filter
    ├── firmware/
    │   ├── Makefile
    │   ├── linker.ld
    │   ├── main.c                 ← entry, scheduler
    │   ├── link.c / .h            ← discovery, peer table, heartbeats
    │   ├── link_audio.c / .h      ← peer-anns, channels, AudioBuffer codec
    │   ├── measurement.c          ← ping/pong + median + GhostXForm update
    │   ├── measurement_responder.c← PingResponder side
    │   ├── session.c              ← session election + remeasurement
    │   ├── tlv.c / .h             ← payload TLV parser/encoder
    │   ├── median.c               ← nth_element for measurement
    │   ├── ghost_time.c / .h      ← thin wrapper around the GhostTime CSRs
    │   ├── beat_pulse.c / .h      ← thin wrapper around the BeatPulse CSRs
    │   ├── tdm.c / .h             ← thin wrapper around the TDM16 CSRs
    │   ├── ipv6.c / .h            ← link-local IPv6 multicast layer
    │   ├── net.c / .h             ← LiteEth UDP socket helpers
    │   ├── http_server.c / .h     ← tiny HTTP/1.0 server with Basic auth
    │   ├── webui.c                ← serves embedded index.html + /api/*
    │   └── config.h               ← compile-time defaults
    └── webui/
        └── index.html             ← single-page admin UI (vanilla JS)
```

## Building

The recommended path is the **provided Docker image** — it bundles a
known-working version of every tool, so you don't have to chase
toolchain versions yourself.

### Quick start (Docker)

```bash
git clone git@github.com:DatanoiseTV/colorlight-i9-abletonlink.git
cd colorlight-i9-abletonlink/LinkFPGA

# One-shot build (pulls the toolchain image the first time, ~5 min):
./docker-build.sh

# With custom TDM port counts:
./docker-build.sh --num-tdm-ports 4 --num-physical-tdm-ports 2

# Drop into a shell inside the container:
./docker-build.sh --shell
```

The image works natively on **both x86_64 and Apple-Silicon arm64**
hosts (the `Dockerfile` uses `TARGETARCH` to pick the right OSS CAD
Suite tarball).

Bitstream and firmware artifacts land in `LinkFPGA/build/colorlight_i9/`.

### Native (Linux only)

```bash
# Toolchain
sudo apt install yosys nextpnr-ecp5 prjtrellis ecpprog \
                 gcc-riscv64-unknown-elf python3-pip
python3 -m pip install --user 'litex>=2024.04' litex-boards migen

# Build
cd LinkFPGA
python3 build.py --build
python3 build.py --build --num-tdm-ports 4 --num-physical-tdm-ports 2
```

### Flashing

```bash
# Volatile (gone after power cycle)
python3 build.py --load

# Persistent (survives power cycle)
python3 build.py --flash
```

### Re-flash firmware only

The firmware is embedded in the BIOS slot of the bitstream by default
but can also be re-uploaded over the USB-UART without resynth:

```bash
python3 build.py --no-compile-gateware --load-firmware
```

## Using it

1. Power up. The `STATUS` LED breathes for ~3 s while the bitstream
   loads, then turns solid once the firmware boots.
2. Connect ETH0 to your studio LAN (the same one your DAW is on).
3. Within ~1 s of cable insertion, `PEER_LED` lights up if Link peers
   are on the network. `SYNC_LED` lights up after the first measurement
   round completes (typically <500 ms).
4. The DHCP-assigned IP is printed on the USB-UART serial console
   (115200 / 8N1). Open `http://<i9-board-ip>/` in any browser.
5. Use the web UI to set a peer name, optionally enable Basic auth,
   and watch the live peer / tempo / beat status.
6. Wire `CLK_24PPQN` to your TR-808's DIN-SYNC, `START_PULSE` /
   `STOP_PULSE` to a sequencer's run/stop input, and use TDM-A / TDM-B
   for audio I/O via your favourite TDM-capable codec
   (TI PCM3168, AKM AK4413, Cirrus CS42448, etc.).

## Resource usage (measured, default 2× TDM16 build)

These are the actual nextpnr-ecp5 numbers from the in-tree Docker
build at the default `--num-tdm-ports 2`:

| Resource              | Used | Available | %     |
|-----------------------|-----:|----------:|------:|
| LUT4 (TRELLIS_SLICE)  | 9 583 | 43 848   | 21.9% |
| BRAM (DP16KD, 18 Kbit)|    57 |     108   | 52.8% |
| MULT18X18D (DSP)      |     4 |      72   |  5.6% |
| EHXPLLL (PLLs)        |     2 |       4   |   50% |
| TRELLIS_IO (pads)     |    84 |     245   |   34% |

Plenty of headroom on logic/IO; BRAM usage is dominated by the BIOS ROM
(64 KiB) and the I/D caches (8 KiB each). Adding more TDM16 ports
costs ~600 LUTs and 4 BRAM each, so 8 ports fit comfortably.

## Timing (measured, default `--sys-clk-freq 60e6`)

| Clock                  | Target  | Achieved | Margin |
|------------------------|--------:|---------:|-------:|
| `sys` (CPU + custom IP)| 60.0 MHz| 71.8 MHz | +19.7% |
| `audio` (TDM MCLK)     | 24.6 MHz|130.9 MHz |  +432% |
| `eth_rx` (RGMII)       |125.0 MHz|131.5 MHz |  +5.2% |

Pushing `--sys-clk-freq 75e6` currently fails the SDRAM-path STA by
about 13 MHz. The default is set to **60 MHz** so the build closes
timing on the first PnR pass. A planned follow-up will pipeline the
SDRAM datapath to reach 75 MHz.

## Latency budget

```
sender DAW commit
   ↓ ~100 µs   (DAW audio thread)
sender NIC TX
   ↓ ~50 µs    (1 GbE switch)
i9 PHY RX
   ↓ ~5 µs     (LiteEth MAC + LinkPacketFilter)
LinkPacketFilter → CPU IRQ
   ↓ ~10 µs    (firmware AudioBuffer parse + schedule)
TDM TX FIFO
   ↓ <1 sample (TDM serialiser, 21 µs at 48 kHz)
ADC line out
```

Total measured latency target: **≤ 1 ms**, hard ceiling ~2 ms,
dominated by the DAW side. The hardware itself adds well under 100 µs
end-to-end.

## Status & known limitations

This is a from-spec rewrite of Ableton Link in gateware + bare-metal C.
Some pieces are well-trodden, others are works in progress. As of the
most recent commit:

| Area | Status | Notes |
|---|---|---|
| Discovery v1 ALIVE/RESPONSE/BYEBYE | ✅ implemented + interop tested |
| Peer table, TTL eviction | ✅ implemented |
| Ping/Pong measurement (initiator) | ✅ implemented |
| Ping/Pong responder | ✅ implemented |
| Session election + 30 s remeasure | ✅ implemented |
| Timeline / StartStopState propagation | ✅ implemented |
| GhostTimeUnit (64-bit, hardware) | ✅ implemented |
| BeatPulseGen (24/48/96 PPQN, start/stop) | ✅ implemented |
| TDM16Core (16ch in / 16ch out, 48 kHz) | ✅ implemented |
| Configurable N × TDM16 ports + virtual ports | ✅ implemented |
| Link-Audio PeerAnnouncement / Pong | ✅ implemented |
| Link-Audio AudioBuffer encode/decode (raw, BE i16) | ✅ implemented |
| Link-Audio ChannelRequest / ChannelByes | ⚠ minimal — request/stop wire format is implemented but the subscription UI is firmware-API-only (no web UI yet) |
| Beat-time-locked TDM scheduling on RX | ⚠ uses jitter-buffer FIFO; not yet phase-locked to `chunk.beginBeats` |
| HTTP admin UI + Basic auth | ✅ implemented |
| IPv6 multicast (link-local) | ✅ implemented (`firmware/ipv6.c`); IPv6 endpoints are not yet in the discovery payload (`mep6` / `aep6` emit pending) |
| LiteEth raw-frame hooks (`liteeth_raw_send`) | ⚠ assumed by `ipv6.c`; if your LiteEth version exposes a different name, edit `ipv6.c` accordingly |
| Second GbE PHY | optional via `--with-second-eth` (gateware only — firmware uses PHY0) |

The two ⚠ items are not protocol-compliance issues — they are
quality-of-implementation gaps that the next round of work will close.

## See also

* [`../LINK_PROTOCOL_SPEC.md`](../LINK_PROTOCOL_SPEC.md) — the wire-format spec
  this implementation follows, with byte-by-byte references back into
  the original Ableton Link C++ source.
* The reference C++ implementation in [`../include/ableton/`](../include/ableton/).
* [Colorlight i9 v7.2 board notes](https://github.com/wuxx/Colorlight-FPGA-Projects/blob/master/colorlight_i9_v7.2.md).
* [LiteX](https://github.com/enjoy-digital/litex) and
  [litex-boards](https://github.com/litex-hub/litex-boards).

## License

GPL-2.0, matching the upstream Ableton Link repository this lives in.
