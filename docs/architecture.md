# LinkFPGA architecture

> A deep dive on the SoC, the custom Migen modules, the firmware
> structure, and the design decisions behind them.

This document is the long-form companion to the
[`README.md`](../README.md). The README sells the project; this
document explains how it actually works under the hood. Read it if
you're going to modify the gateware or firmware, or if you just want
to understand the design rationale.

---

## 1. Top-level block diagram

```
                ┌─────────────────────────────────────────────────────┐
                │                  LinkFPGA SoC                       │
                │                                                     │
   GbE  ─>──────┤   RGMII  ┌───────────┐    ┌─────────────────────┐  │
   PHY0         |          │  LiteEth  │    │   VexRiscv softcore │  │
                |          │   MAC     │←──→│   @ 50 MHz, +debug  │  │
                |          └─────┬─────┘    │                     │  │
                |                │          │  Wishbone bus       │  │
                |        eth_rx CD          │  + CSR + IRQ        │  │
                |                ▼          │                     │  │
                |       ┌────────────────┐  │                     │  │
                |       │  ethmac slot   │  │  ┌──────────────┐   │  │
                |       │  RAMs (BRAM)   │←─┼─→│ link_firmware│   │  │
                |       └────────────────┘  │  │  in main_ram │   │  │
                |                           │  │  (SDRAM 8MB) │   │  │
                |                           │  └──────────────┘   │  │
                |                           └──────┬──────────────┘  │
                |                                  │ CSR              │
                |                                  ▼                  │
                |   ┌─────────────────────────────────────────────┐  │
                |   │                  CSR bus                    │  │
                |   └──┬────────┬─────────┬────────────┬──────────┘  │
                |      │        │         │            │             │
                |   ┌──┴────┐ ┌─┴────┐  ┌─┴────────┐ ┌─┴─────────┐  │
                |   │Ghost  │ │Beat  │  │  TDM16   │ │ MidiCore  │  │
                |   │Time   │ │Pulse │  │  ×N      │ │ + Eurorack│  │
                |   │Unit   │ │ Gen  │  │  Core    │ │ Input     │  │
                |   └───────┘ └──┬───┘  └────┬─────┘ └─┬─────┬───┘  │
                |       │        │           │         │     │      │
                +───────┼────────┼───────────┼─────────┼─────┼──────+
                        │        │           │         │     │
                        ▼        ▼           ▼         ▼     ▼
                  (no pin)   pmodh:        pmode/f/d/g  pmodc pmodc
                             beat clock    /i/j/k/l:    [0-1] [2,4,5]
                             + transport   TDM16 audio  MIDI  Euro
                             pulses
```

The principle: **everything sits on the same CSR bus, everything
shares the same `GhostTimeUnit` time base**. There's no other clock
domain crossing the protocol boundary. Whether a beat is "now" is
exactly the same question for the GPIO pulse generator, the MIDI
auto-injector, the Eurorack edge timestamper, and the firmware's Link
session election.

---

## 2. Custom Migen modules

These all live in [`litex_soc/`](../litex_soc/) and follow the same
pattern: subclass `Module`, inherit `AutoCSR`, expose state via
`CSRStorage`/`CSRStatus` so the firmware can poke them, raise an IRQ
for asynchronous events.

### 2.1 `GhostTimeUnit` ([`ghost_time.py`](../litex_soc/ghost_time.py))

The heart of the SoC. A 64-bit microsecond counter clocked at 1 MHz
(via a `sys_clk_freq / 1e6` divider) plus a software-loaded
`GhostXForm` intercept register that turns the local host time into
the shared session ghost time:

```
ghost_time = host_time + intercept     (slope is hardwired to 1)
```

The Ableton Link spec uses `slope = 1` in the reference C++ once a
measurement converges, so we don't bother with a multiplier — drift
is absorbed by the 30 s remeasurement schedule in `session.c`.

**CSR map** (8-bit register width, big-endian word order):

| Reg | Bits | RW | Purpose |
|---|---|---|---|
| `host_lo` | 32 | R | Lower 32 bits of `host_time`. Reading this **latches** all four host/ghost words for atomicity. |
| `host_hi` | 32 | R | Upper 32 bits, latched. |
| `ghost_lo` | 32 | R | Lower 32 bits of `host_time + intercept`, latched. |
| `ghost_hi` | 32 | R | Upper 32 bits, latched. |
| `inter_lo` | 32 | RW | Intercept low word. |
| `inter_hi` | 32 | RW | Intercept high word. |
| `ctrl` | 8 | RW | bit0 = freeze, bit1 = soft reset. |

The latching trick is critical: a read of `host_lo` snapshots all
four shadow registers in the same sys clock cycle, so the firmware can
read a coherent 64-bit timestamp pair without race-condition
stitching.

### 2.2 `BeatPulseGen` ([`beat_pulse.py`](../litex_soc/beat_pulse.py))

Generates the beat-clock and transport pulses on GPIO from the
`GhostTimeUnit` 1 MHz tick and a small set of firmware-programmed
period registers. Each pulse output is driven by its own 32-bit
microsecond down-counter that auto-reloads from a CSR.

The firmware programs the period (in microseconds) of each output
whenever the active timeline or tempo changes. Since timeline updates
arrive at most once every few hundred milliseconds, this design has
bounded latency and avoids any runtime integer divide in gateware
(which is expensive on FPGA).

Pulse outputs:
* `clk_24` / `clk_48` / `clk_96` — N pulses per quarter note
* `beat`, `bar` — 1 pulse per beat / per N beats
* `start`, `stop` — one-shot on play/stop transitions
* `run` — high while playing
* `sync_led` — high while locked to a Link session
* `peer_led` — high while ≥ 1 peer is present
* `pps` — 1 pulse per second from the ghost-time counter

### 2.3 `TDM16Core` ([`tdm.py`](../litex_soc/tdm.py))

A 16-channel × 16-bit TDM serialiser/deserialiser, with a CPU-visible
double-buffered sample register file. One TDM frame = 16 slots ×
16 bits = 256 BCK cycles. At fs = 48 kHz that's BCK = 12.288 MHz.

The serialiser is clocked from the `audio` clock domain (24.576 MHz
MCLK / 2 = 12.288 MHz BCK by toggling each cycle), while the sample
register file lives in the `sys` domain and uses tiny gray-coded
handshakes to safely cross domains. The firmware reads each channel's
latest sample via `tdm_read_rx(port, ch)` and writes the next
sample via `tdm_write_tx(port, ch, value)`. A per-frame IRQ tells
the firmware when a fresh frame has been latched.

**Virtual mode**: when instantiated with `virtual=True`, no BCK / FS
/ SDIN / SDOUT signals are generated; the IRQ is driven from a
sys-clock-domain `fs`-divider so the firmware still sees a regular
48 kHz tick. The RX side reads zero (or, optionally, loopback from
TX). Useful for software-only audio routing endpoints with no
physical pins.

### 2.4 `MidiCore` ([`midi.py`](../litex_soc/midi.py))

A from-scratch hardware MIDI 1.0 transceiver running at 31250 baud.
The whole point of building this from scratch instead of reusing
LiteX's stock UART is so we can sniff individual RX bytes (for
hardware MIDI System Real-Time detection) and inject TX bytes with
zero firmware involvement (for sample-accurate clock-out).

**TX path:** Two byte sources arbitrated in HW:

* The auto-injector takes `BeatPulseGen.clk_24` / `start` / `stop`
  pulses as inputs and pushes the corresponding MIDI System Real-Time
  bytes (`0xF8` / `0xFA` / `0xFC`) into the TX shift register. **Zero
  firmware jitter** — the byte hits the wire on the very next BCK
  edge after the pulse fires.
* A 64-byte FIFO collects firmware-pushed bytes (note on/off, CC,
  sysex, …). The HW arbiter ensures real-time bytes always have
  priority and can interleave any other queued byte at byte
  boundaries (per MIDI spec).

**RX path:** Standard UART receiver, plus a hardware MIDI parser:

* Each completed byte is classified as System Real-Time
  (`0xF8`–`0xFE`) or data.
* RT bytes are timestamped with the `GhostTimeUnit` µs counter at the
  exact instant the stop bit completes, and queued into a single-slot
  RT event with an IRQ.
* Non-RT bytes go into a 64-byte data FIFO for the firmware to drain
  via `midi_recv_byte()`.
* A hardware period meter measures the microseconds between
  successive `0xF8` bytes and exposes it as a CSR — the firmware
  divides 60 000 000 by it to get BPM.

### 2.5 `EurorackInput` ([`eurorack.py`](../litex_soc/eurorack.py))

Hardware edge detector + timestamp + period meter for three
Eurorack-style TTL inputs:

* `CLK_IN` — rising-edge clock at a configurable PPQN
* `RST_IN` — rising-edge reset (snap to bar 1, beat 1)
* `RUN_IN` — rising-edge play / falling-edge stop (level-sensitive too)

Each input is brought in through a 3-stage `MultiReg` synchronizer
for metastability hardening. On every detected edge the module:

1. Latches the `GhostTime` µs counter into a per-input "last event
   timestamp" register.
2. For `CLK_IN`, computes the period since the previous rising edge
   and stores it in a CSR — the firmware divides 60 000 000 by this
   to get BPM.
3. Maintains a free-running edge counter so the firmware can detect
   missed events (e.g. if the main loop was busy).
4. Raises a shared "edge" IRQ.

---

## 3. Firmware structure

### 3.1 Main loop

The firmware ([`firmware/main.c`](../firmware/main.c)) is a single
cooperative loop. There's no RTOS, no preemption, no threading — just
polled state machines and the `GhostTimeUnit` µs counter as the
authoritative timebase.

```c
int link_app_main(void) {
    net_init();           // lwIP + DHCP + IPv6 SLAAC
    beat_pulse_init();
    tdm_init();
    link_init();          // discovery + peer table
    link_audio_init();    // Link-Audio extension
    midi_init();          // MIDI 31250 baud + auto-TX
    euro_init();          // Eurorack inputs

    http_init(LINK_HTTP_PORT);
    webui_init();

    while (1) {
        net_poll();         // lwIP timers + ethmac RX
        link_tick();        // ALIVE schedule, peer prune
        session_tick();     // measurement + remeasurement
        link_audio_tick();  // PeerAnnouncement schedule
        midi_tick();        // drain RT events
        euro_tick();        // poll edge counters

        for (int port = 0; port < LINK_NUM_TDM_PORTS; port++) {
            if (tdm_frame_pending(port)) {
                link_audio_on_tdm_frame(port);
                tdm_clear_frame_pending(port);
            }
        }
    }
}
```

The main loop runs at >>10 kHz on a 50 MHz VexRiscv with no caches
contended, which is much faster than any of the periodic events it
serves (Link broadcasts at 250 ms, MIDI clock at ~50 Hz, TDM frames
at 48 kHz — but TDM frames are handled via per-frame IRQ in addition
to polling). Nothing is time-critical at the main-loop level because
all the timing-sensitive work is in hardware.

### 3.2 Link protocol stack

| File | Purpose |
|---|---|
| `link.c` / `link.h` | Discovery (ALIVE / RESPONSE / BYEBYE), peer table, heartbeat schedule, mep4/mep6/aep4/aep6 emit + parse |
| `link_audio.c` / `link_audio.h` | Link-Audio extension: PeerAnnouncement, channel catalog, AudioBuffer encode/decode, kChannelRequest state machine |
| `measurement.c` | Ping/pong measurement initiator |
| `measurement_responder.c` | Ping/pong responder |
| `session.c` | Session election (`SESSION_EPS` tie-break) + 30 s remeasurement |
| `tlv.c` / `tlv.h` | Payload TLV parser/encoder shared across all message types |
| `median.c` | `nth_element` for the measurement filter |
| `ghost_time.c` / `ghost_time.h` | Thin wrapper around the GhostTime CSRs |
| `beat_pulse.c` / `beat_pulse.h` | Thin wrapper around the BeatPulse CSRs (load timeline, set play/stop, set sync LED) |
| `tdm.c` / `tdm.h` | Thin wrapper around the per-port TDM16 CSRs |
| `midi.c` / `midi.h` | MIDI sync state machine, RT event drain, BPM-from-period |
| `eurorack.c` / `eurorack.h` | Eurorack input handler + follower mode |

### 3.3 Network layer

The firmware ships **lwIP 2.2.0** in `NO_SYS=1` mode:

* `netif_litex.c` is a hand-written netif driver that talks straight
  to LiteEth's `ethmac` CSRs and TX/RX slot RAMs.
* `net.c` / `net.h` wrap lwIP's raw UDP / TCP API for the rest of
  the firmware (`net_udp_send_v4_unicast`, `net_udp_send_v4_mcast`,
  `net_udp_send_v6_mcast`, `net_set_rx_callback`).
* IPv4 multicast (224.76.78.75) is joined via `igmp_joingroup()` at
  init.
* IPv6 link-local multicast (`ff12::8080`) is joined via
  `mld6_joingroup_netif()` at init.
* DHCPv4 + AutoIP fallback is enabled. IPv6 SLAAC is enabled.

`http_server.c` is a tiny HTTP/1.0 server built on lwIP's raw TCP
API: it accumulates per-connection request bytes until a `Content-
Length`-bounded body has arrived, then dispatches to a route table
populated by `webui.c`. Both GET and POST work; multi-segment
requests are supported. No keep-alive, no chunked, no TLS.

---

## 4. Boot flow

```
   power on
       │
       ▼
┌────────────────┐  1. CPU reset → 0x00000000 (BRAM)
│  LiteX BIOS    │  2. SDRAM init via litedram (M12L64322A model)
│  (BRAM, 64K)   │  3. Ethernet PHY init
│                │  4. Wait for serialboot / spiboot
│  bios.elf      │  5. Load link_firmware.bin → main_ram
│                │  6. Jump to MAIN_RAM_BOOT_ADDRESS
└────────┬───────┘
         │
         ▼
┌────────────────┐  1. crt0.S zeroes .bss, sets sp + gp
│   Firmware     │  2. Calls link_app_main()
│  (SDRAM, 132K) │  3. lwip_init() + DHCP + SLAAC
│                │  4. link_init(), link_audio_init()
│                │  5. midi_init(), euro_init()
│  link_firmware │  6. http_init()
│      .elf      │  7. Main loop: net_poll, ticks, TDM
└────────────────┘
```

### Why split the BIOS and firmware?

Because the firmware doesn't fit in BRAM. With lwIP + Link +
Link-Audio + HTTP + the custom drivers, our `link_firmware.elf` is
~133 KB of `.text` and ~180 KB of `.bss`. The integrated ROM is
BRAM-backed and capped at ~108 KiB on the LFE5U-45F part — there's
no way to put that much code in there.

So we use the **standard production-grade LiteX pattern**: tiny BIOS
in BRAM (~35 KB) that initialises SDRAM and loads a separate
firmware ELF into `main_ram` via serialboot/spiboot, then jumps. This
is exactly how every non-trivial LiteX project on a small ECP5 boots.

The `MAIN_RAM_BOOT_ADDRESS` SoC constant tells the BIOS to auto-jump
to `0x40000000` after `boot_sequence()` completes (whether by
serialboot or spiboot). The custom `crt0.S` zeroes BSS, sets up the
stack pointer + global pointer, and calls into our `link_app_main()`
entry point.

---

## 5. Build pipeline

The Docker image bundles the entire toolchain:

```
linkfpga-builder
├── Ubuntu 22.04 base
├── YosysHQ OSS CAD Suite (yosys + nextpnr-ecp5 + prjtrellis + ecpprog)
├── RISC-V bare-metal GCC (gcc-riscv64-unknown-elf)
├── LiteX 2024.04 + litex-boards + litedram + liteeth + migen
└── lwIP 2.2.0 vendored at /opt/lwip
```

The `Dockerfile` patches a few upstream files in place to work with
the current picolibc layout (the `newlib/libc/tinystdio` include
path moved, `clzdi2.o` was missing from `libcompiler_rt`, etc.). All
patches are documented at the top of the Dockerfile so they can be
upstreamed.

`build.py` is a thin LiteX argparse shell. It:

1. Imports `colorlight_i5.Platform(board="i9", revision="7.2")` from
   litex-boards as the base platform.
2. Calls `attach_link_io()` from `litex_soc.platform_i9` to add the
   `link_tdm`, `link_pulse`, `midi_uart`, and `eurorack_in` IO groups
   on the appropriate PMOD pins.
3. Constructs a `LinkFPGASoC` (subclass of `SoCCore`) which adds the
   custom Migen modules to the CSR bus.
4. Builds the gateware via the standard LiteX → Yosys → nextpnr-ecp5
   → ecppack pipeline.
5. Builds the firmware as a standalone ELF + flat binary in
   `build/colorlight_i5/software/link_firmware/`.

`tools/gen_pinout.py` is the live-introspection SVG generator: it
imports `colorlight_i5._connectors_v7_2` for the actual PMOD pin
lists and `litex_soc.platform_i9` for the function-to-pin map, walks
every `Subsignal`/`Pins` record, and renders the diagram with one
cell per pin. Re-running it after any platform-file change keeps the
docs in sync with the gateware.

---

## 6. Performance and resource usage

### Resources (default `--num-physical-tdm-ports 2`)

| Resource              | Used  | Available | %     |
|-----------------------|------:|----------:|------:|
| LUT4                  | 10 206 | 43 848   | 23.3 % |
| BRAM (DP16KD)         |     57 |     108   | 52.8 % |
| MULT18X18D            |      4 |      72   |  5.6 % |
| EHXPLLL               |      2 |       4   |   50 % |
| TRELLIS_IO            |     90 |     245   |   37 % |

Cost breakdown for the LinkFPGA-specific gateware:

| Module | LUTs | Notes |
|---|---|---|
| `GhostTimeUnit`     | ~400  | 64-bit add/sub + CSR bridge |
| `BeatPulseGen`      | ~600  | 4 phase comparators + 1 PPS |
| `TDM16Core` × 2     | ~1 600| 16 slot regs + serdes each |
| `MidiCore`          | ~600  | UART + arbiter + parser + period meter |
| `EurorackInput`     | ~300  | 3 sync chains + edge detect + period meter |
| **Total custom**    | **~3 500** | rest is VexRiscv + LiteEth + LiteDRAM + LiteX glue |

### Timing (default `--sys-clk-freq 50e6`)

| Clock | Target | Achieved | Margin |
|---|--:|--:|--:|
| `sys`     |  50.0 MHz |  66.7 MHz | +33.4 % |
| `audio`   |  24.6 MHz | 136.0 MHz |   +453 % |
| `eth_rx`  | 125.0 MHz |  99.0 MHz |    −20 % * |

`*` The `eth_rx` STA pessimism is a known LiteEth/ECP5 quirk on this
board. The actual RGMII path is source-synchronous on the Broadcom
B50612D PHY with a programmable skew, so it works in practice — the
upstream `colorlight_i5` LiteX target reports the same warning.

### Latency budget

```
sender DAW commit
   ↓ ~100 µs   (DAW audio thread)
sender NIC TX
   ↓ ~50 µs    (1 GbE switch)
i9 PHY RX
   ↓ ~10 µs    (LiteEth MAC + lwIP raw UDP dispatch)
firmware AudioBuffer parse + schedule
   ↓ <1 sample (TDM serialiser, 21 µs at 48 kHz)
ADC line out
```

Target end-to-end: **≤ 1 ms**, hard ceiling ~2 ms, dominated by the
DAW side. The hardware itself adds well under 100 µs.

For the sync sources (which is what this device is really for):

| Source | Mechanism | Jitter / latency |
|---|---|---|
| Link → 24/48/96 PPQN GPIO | `BeatPulseGen` HW down-counters tick at 1 MHz | ±1 µs |
| Link → MIDI clock TX | `MidiCore` auto-injector latches `clk_24` directly | **zero firmware jitter**; one bit-cell (32 µs) of fixed UART offset |
| MIDI clock RX → BPM | HW samples the stop-bit edge and latches the µs counter | ±1 µs |
| Eurorack CLK_IN → BPM | 3-stage MultiReg + edge detector + HW period meter | 60 ns + ±1 µs |
| External RST_IN → bar 1 | HW edge detect → IRQ → main-loop pass | ≤10 µs |

The firmware never has to time-stamp anything itself — every event
arrives with a hardware timestamp from the same 1 MHz counter that
drives the Link ghost-time, so all four time bases stay coherent at
the µs level.

---

## 7. Adding a new Migen module

If you want to add a new peripheral (say, an extra GPIO bank, or a
SPI master, or another sync source), the recipe is:

1. Create `litex_soc/<your_module>.py` with a `Module` subclass that
   inherits `AutoCSR`. Expose state via `CSRStorage`/`CSRStatus`.
2. If it generates events, add an `EventManager` with one or more
   `EventSourceProcess` / `EventSourceLevel` entries and call
   `self.ev.finalize()`.
3. Import it from `litex_soc/soc.py`, instantiate it as
   `self.submodules.<name>`, call `self.add_csr("<name>")`, and (if
   it has events) `self.irq.add("<name>", use_loc_if_exists=True)`.
4. If it needs platform pads, add them to `litex_soc/platform_i9.py`
   via `platform.add_extension()` and request them in `soc.py` via
   `platform.request("<group_name>", N)`.
5. Add a thin C wrapper in `firmware/<your_module>.c` / `.h` that
   reads/writes the CSRs via the auto-generated accessors in
   `generated/csr.h`.
6. Add `<your_module>.o` to `firmware/Makefile`.
7. Initialise it from `firmware/main.c`'s `link_app_main()` and add a
   `<your_module>_tick()` call to the main loop if it needs polling.
8. Re-run `./docker-build.sh --gen-pinout` to update the SVG diagram
   if you added physical pins.

The TDM16 / Midi / Eurorack modules are good templates — pick the
one that's structurally closest to what you're building.

---

## 8. References

* [`LINK_PROTOCOL_SPEC.md`](../LINK_PROTOCOL_SPEC.md) — the
  wire-format spec, with byte-by-byte references back into the
  original Ableton/link C++ source.
* [Ableton/link](https://github.com/Ableton/link) — the reference
  C++ implementation.
* [LiteX](https://github.com/enjoy-digital/litex) — the SoC
  framework.
* [Migen](https://m-labs.hk/migen/) — the Python HDL we use for the
  custom modules.
* [VexRiscv](https://github.com/SpinalHDL/VexRiscv) — the softcore.
* [lwIP](https://savannah.nongnu.org/projects/lwip/) — the TCP/IP
  stack.
