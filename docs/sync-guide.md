# Sync sources guide

> Practical wiring and configuration for the three independent sync
> sources LinkFPGA can listen to or drive: **Ableton Link** (over the
> network), **MIDI clock** (over the UART), and **Eurorack-level
> CLK / RST / RUN** (over GPIO).

This is the practical companion to the
[`README.md`](../README.md) "Sync sources" section. The README
explains the architecture; this document tells you how to actually
plug it in.

---

## 1. The big picture

LinkFPGA shares **one hardware microsecond counter** between all
three sync sources. That counter is `GhostTimeUnit` — a 64-bit free-
running register clocked at 1 MHz. Every event timestamp the firmware
sees comes from the same counter, latched in hardware at the moment
of the event:

* The Link ping/pong measurement uses it for the `__ht` and `__gt`
  payload bytes.
* The MIDI RX byte sniffer latches it on the stop bit of every
  incoming `0xF8` clock byte.
* The Eurorack edge detector latches it on every rising edge of
  `EURO_CLK_IN`, `EURO_RST_IN`, and `EURO_RUN_IN`.
* The beat-pulse GPIO outputs are driven from the same counter via
  down-counters in `BeatPulseGen`.

So if you wire all three sync sources at once, the device knows the
exact time relationship between them with sub-microsecond accuracy.
That's the foundation that makes follower modes actually work.

---

## 2. Picking a sync mode

LinkFPGA can be in one of three sync modes at any time. Set them via
the web UI or the JSON API at `/api/sync`.

### Mode A — **Leader (default)**

* The device is the source of truth for the local Link session.
* It generates 24 PPQN beat pulses on `pmodh:1`.
* It drives MIDI clock / start / stop on `pmodc:0` (`MIDI_TX`) with
  zero software jitter (the `MidiCore` auto-injector reads the
  `BeatPulseGen` pulses directly).
* It still **measures** the incoming MIDI clock and the Eurorack
  CLK_IN, but does nothing with the measurements except expose them
  in the web UI.

Use this mode when you want the device to be the master clock for
your studio.

### Mode B — **MIDI follower**

* The local Link tempo is **set from the incoming MIDI clock period**
  (measured by the hardware period meter).
* MIDI start / continue → `link_set_play(true)`.
* MIDI stop → `link_set_play(false)`.
* The MIDI TX path **also** auto-emits clock bytes (from
  `BeatPulseGen.clk_24`), so the device transparently passes through
  the clock — handy for MIDI thru chains.
* Eurorack inputs are still measured but ignored.

Use this mode when an external MIDI clock source is the master and
you want LinkFPGA + the Link mesh to follow it.

### Mode C — **Eurorack follower**

* The local Link tempo is **set from the `EURO_CLK_IN` period**
  (measured by the hardware period meter, with the configured PPQN
  divisor applied).
* `EURO_RST_IN` rising edge → snap the Link beat origin to "now",
  effectively resetting to bar 1 / beat 1.
* `EURO_RUN_IN` level → `link_set_play(level)`.
* MIDI TX still auto-emits clock from `BeatPulseGen`, so the device
  transparently bridges Eurorack to MIDI.

Use this mode when a modular system is the master and you want
LinkFPGA + the Link mesh + downstream MIDI gear to follow it.

---

## 3. Practical wiring

### 3.1 MIDI

You need two pins from `pmodc`:

| LinkFPGA pin | Direction | Connect to |
|---|---|---|
| `pmodc:0` (FPGA pin `P17`) | OUT | The `tip` of a TRS-A MIDI cable, or pin 5 of a 5-pin DIN |
| `pmodc:1` (FPGA pin `R18`) | IN  | The `tip` of a TRS-A MIDI input, or pin 4 of a 5-pin DIN, **via an opto-isolator** |

> **MIDI 1.0 hardware safety note.** The MIDI standard expects a
> 5 mA current loop with the receiver opto-isolated. The FPGA pin is
> 3.3 V LVCMOS, not a current loop, so:
>
> * For TX (`P17` → external device IN), most modern MIDI gear
>   accepts 3.3 V LVCMOS directly, but a 220 Ω series resistor is
>   recommended to limit current into the receiver opto.
> * For RX (`R18` ← external device OUT), you must put an opto-
>   isolator (6N138 / H11L1 / similar) in front of the FPGA pin to
>   protect against ground loops and inverted polarity. Wire the opto
>   output to `R18` with a 1 kΩ pull-up to 3.3 V.
>
> A "good enough" MIDI input circuit is documented in the original
> MIDI 1.0 spec; any 5-pin DIN → TRS adapter you can buy will have
> the same circuit on its IN side and you can copy it.

After you've wired the cable, set the device to one of the modes
above and you should immediately see the BPM in the web UI when an
external MIDI clock source is sending.

### 3.2 Eurorack

You need three pins from `pmodc` plus an external 5 V → 3.3 V
interface board:

| LinkFPGA pin | Direction | Connect to |
|---|---|---|
| `pmodc:2` (FPGA pin `C18`) | IN | Eurorack clock output (CLK_IN) |
| `pmodc:4` (FPGA pin `M17`) | IN | Eurorack reset trigger (RST_IN) |
| `pmodc:5` (FPGA pin `R17`) | IN | Eurorack run gate (RUN_IN) |

> **Eurorack hardware safety note.** Eurorack signals are typically
> 0–5 V or 0–10 V. The FPGA pin is 3.3 V LVCMOS — driving it above
> 3.6 V will damage the part.
>
> A simple level-shifter circuit per input:
>
>     EURORACK ──┬── 10 kΩ ──┬── 74HC14 ──── FPGA pin
>                │           │
>                ⏚          ⏚ via 3.3 V Zener clamp
>
> A 74HC14 Schmitt-trigger inverter pair (one stage to clamp, one to
> re-invert) gives a clean 3.3 V LVCMOS signal with hysteresis and
> some protection against the occasional negative spike from a CV
> cable. Add a 1N5817 Schottky from the input to ground for negative
> spike clamping if your modular feeds long unbalanced cables.

### 3.3 Beat clock outputs (DIN-sync, modular triggers)

The beat-clock and transport pulses are output as 3.3 V LVCMOS on
`pmodh`. They're suitable for direct connection to:

| LinkFPGA output | Pin | Goes to |
|---|---|---|
| `CLK_24PPQN` | `pmodh:1` (`E19`) | TR-808 / 909 / 606 / DR-110 DIN-SYNC clock input |
| `START_PULSE`| `pmodh:2` (`B3`)  | DIN-SYNC start input |
| `STOP_PULSE` | `pmodh:3` (`K5`)  | (most DIN-SYNC devices auto-stop on absence of start) |
| `BEAT_CLK`   | `pmodh:5` (`B2`)  | Modular trigger input — one pulse per beat |
| `RUN_LEVEL`  | `pmodh:6` (`K4`)  | Modular gate input — high = playing |
| `SYNC_LED`   | `pmodh:7` (`A2`)  | Drive a status LED through a 1 kΩ resistor |

> **DIN-sync polarity.** Roland DIN-SYNC is a 5-pin DIN with pin 1 =
> ground, pin 3 = clock (24 PPQN), pin 5 = run/stop. LinkFPGA emits
> the clock as 3.3 V active-high pulses — most DIN-SYNC inputs are
> happy with that, but vintage TR-808s expect 5 V. A 74HCT04 buffer
> stage (which has TTL-compatible inputs) between the FPGA pin and
> the DIN connector is recommended for vintage gear.

The pulse width defaults to 1 ms; if your downstream device wants
shorter pulses, write a smaller value (in microseconds) to the
`pulse_width` CSR via the firmware (or expose it in the web UI).

---

## 4. Configuration

### Eurorack PPQN divisor

Modular clocks vary in their PPQN: some output one trigger per
quarter note, some 2, 4, 8, 16, or 24. Configure the divisor in the
web UI or via:

```
POST /api/euro/ppqn   body: 4
```

The firmware divides the measured clock period by the PPQN to get
the per-beat period, then converts to BPM.

Common settings:

| Eurorack source | PPQN |
|---|---|
| One pulse per beat (most simple modulars)        | 1   |
| One pulse per 8th note                           | 2   |
| One pulse per 16th note                          | 4   |
| Pamela's NEW Workout / similar, 16 PPQN setting  | 16  |
| MIDI-style 24 PPQN (rare in modular)             | 24  |

### MIDI sync mode

```
POST /api/midi/sync   body: off
POST /api/midi/sync   body: observe
POST /api/midi/sync   body: follower
```

`observe` measures incoming clock without changing the local Link
tempo. Useful for debugging.

### MIDI auto-TX flags

The MIDI TX side has three independent enable bits in the
`MidiCore.auto_tx_ctrl` register:

* bit 0 — auto-emit `0xF8` (clock) on every `BeatPulseGen.clk_24` pulse
* bit 1 — auto-emit `0xFA` (start) on every transport start pulse
* bit 2 — auto-emit `0xFC` (stop) on every transport stop pulse

All three are on by default. Turn them off if you want to drive the
MIDI clock from the firmware instead, or if you have an external
clock source that's already broadcasting on the same MIDI bus and
you don't want the device to fight it.

---

## 5. Troubleshooting

**The web UI shows incoming MIDI BPM but the synth is silent.**
Check that the MIDI receiver is set to "external clock" — many
synths default to internal clock. Also check the polarity of your
TRS-A vs TRS-B cable.

**Eurorack BPM reading wobbles.** The hardware period meter measures
**every** edge, so jitter on the modular's clock output shows up
directly. If your modular clock is intentionally noisy (analog clock
divider, etc.), set a higher PPQN so each individual edge contributes
less to the BPM estimate.

**Eurorack RST snaps the beat but the GPIO clock keeps phasing.**
That's expected: `RST_IN` resets the beat phase but doesn't realign
the existing pulse-train counters. The next pulse will fire at the
new phase. If you need an immediate hard reset of the pulse outputs
too, also write 1 to the `BeatPulseGen.soft_reset` CSR.

**MIDI TX bytes are missing.** Check the `tx_busy` bit and the
`tx_free` register in the `MidiCore` CSRs. If `tx_free` is 0, the
TX FIFO is full — that means the firmware is pushing too fast OR
the auto-injector is filling it. The auto-injector has byte priority
so a stuck firmware loop won't lock out the realtime bytes, but a
firmware loop sending sysex faster than 31250 baud will fill the
FIFO.

**MIDI RX shows real-time bytes but data bytes are missing.** The
hardware parser routes RT bytes to the `rt_ev_*` slot (one at a
time, drained by `midi_tick()`) and data bytes to a 64-byte FIFO. If
the firmware doesn't drain the data FIFO fast enough — `midi_tick()`
calls `midi_recv_byte()` once per main-loop pass — then incoming
sysex can fill the FIFO. The fix is to drain it in a tighter loop;
typical workloads don't hit this.

**Two devices fight over the Link session.** This is normal during
startup: the spec's `SESSION_EPS` tie-break (500 ms) means two newly-
booted devices will agree on a session within ~1 s. If the fight
continues, check that all peers see each other's `mep4` endpoint
(visible in the web UI peer table) so measurements actually complete.
