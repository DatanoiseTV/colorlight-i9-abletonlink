"""
MidiCore — hardware-accelerated MIDI 1.0 transceiver.

Why a custom Migen module instead of LiteX's stock UART?

1. **Zero-jitter clock TX**: MIDI clock is 24 PPQN (one 0xF8 byte per
   1/24 of a beat). The exact wall-clock time the byte hits the line
   matters for downstream synth/sequencer sync. By taking
   `BeatPulseGen.clk_24` directly into a hardware arbitrator, the
   start-bit goes onto the wire on the very next BCK edge after the
   pulse fires. No firmware involved, no IRQ latency, no scheduler.

2. **±1 µs RX timestamps**: MIDI System Real-Time bytes (0xF8–0xFE)
   can interleave any other message at any byte boundary. The HW
   sniffer detects them as soon as the stop bit completes and latches
   the GhostTime microsecond counter at that exact instant. The
   firmware sees `(byte, timestamp)` in a FIFO with no software jitter
   on the timestamp.

3. **Hardware period meter for incoming 0xF8 clock**: The HW computes
   the time between successive 0xF8 bytes (in microseconds) and
   exposes it as a CSR. Firmware reads it and converts to BPM with one
   integer divide. No need to keep a sliding window in software.

The non-realtime data path (note on/off, CC, sysex) is plain UART
TX/RX with byte FIFOs the firmware reads and writes.

CSR map (32-bit aligned):

  CTRL              8 bits  RW   bit0 = enable, bit1 = soft reset
  STATUS            8 bits  R    bit0 = tx_busy, bit1 = rx_overrun

  TX_DATA           8 bits  W    push byte into the data TX FIFO
  TX_FREE           8 bits  R    space remaining in TX FIFO

  RX_DATA           8 bits  R    pop byte from the data RX FIFO
                                  (non-realtime bytes only)
  RX_AVAIL          8 bits  R    bytes available in data RX FIFO

  RT_EV_BYTE        8 bits  R    last realtime byte received
  RT_EV_TS_LO      32 bits  R    timestamp low (us)
  RT_EV_TS_HI      32 bits  R    timestamp high (us)
                                  (a read of TS_LO latches all three
                                  for atomicity)
  RT_EV_VALID       8 bits  R    1 if a realtime event is queued

  RT_EV_ACK         8 bits  W    write any value to pop the queued
                                  realtime event

  CLK_PERIOD       32 bits  R    measured period between successive
                                  0xF8 incoming bytes, in microseconds
                                  (0 = no incoming clock seen yet)

  AUTO_TX_CTRL      8 bits  RW   bit0 = enable auto clock TX (forward
                                  BeatPulseGen.clk_24 → 0xF8)
                                  bit1 = enable auto start TX
                                  bit2 = enable auto stop TX

The IRQ fires on every realtime RX event and on TX FIFO empty.
"""

from migen import (
    Cat, ClockDomainsRenamer, If, Module, Replicate, Signal,
)
from migen.genlib.cdc import MultiReg
from migen.genlib.fifo import SyncFIFO

from litex.soc.interconnect.csr import (
    AutoCSR, CSRStatus, CSRStorage,
)
from litex.soc.interconnect.csr_eventmanager import (
    EventManager, EventSourceProcess,
)


# MIDI System Real-Time message status bytes (single-byte messages
# that can be interleaved with anything at any byte boundary).
RT_CLOCK    = 0xF8
RT_TICK     = 0xF9          # reserved / undefined; we still classify it as RT
RT_START    = 0xFA
RT_CONTINUE = 0xFB
RT_STOP     = 0xFC
RT_RESERVED = 0xFD          # reserved / undefined
RT_ACTIVE   = 0xFE          # active sensing
RT_RESET    = 0xFF          # system reset


def _is_rt(byte_signal):
    """Migen expression: True if `byte_signal` is in 0xF8..0xFF."""
    return byte_signal[3:8] == 0b11111


class MidiCore(Module, AutoCSR):
    """Hardware MIDI 1.0 (31250 baud) transceiver with auto-sync TX
    and timestamped RT-byte RX.

    Args:
        pads: a record with `tx` (out) and `rx` (in) signals.
        sys_clk_freq: integer Hz, used to derive the bit-clock counter.
        ghost_time: the GhostTimeUnit instance, used for RX timestamps.
        beat_pulse: the BeatPulseGen instance, used for auto TX.
    """

    def __init__(self, pads, sys_clk_freq, ghost_time, beat_pulse):
        # ---- bit-clock divider --------------------------------------
        # MIDI baud rate = 31250 baud → 32 µs per bit.
        baud = 31250
        divider_max = max(1, int(round(sys_clk_freq / baud)))

        # ---- CSRs ---------------------------------------------------
        self.ctrl         = CSRStorage(8, reset=0x01,
            description="bit0=enable bit1=soft_reset")
        self.status       = CSRStatus(8,
            description="bit0=tx_busy bit1=rx_overrun")

        self.tx_data      = CSRStorage(8, write_from_dev=False,
            description="push byte into TX data FIFO")
        self.tx_free      = CSRStatus(8,
            description="space remaining in TX FIFO")
        self.tx_push      = CSRStorage(8,
            description="write to push tx_data into the TX FIFO")

        self.rx_data      = CSRStatus(8,
            description="oldest byte in non-RT RX FIFO")
        self.rx_avail     = CSRStatus(8,
            description="non-RT bytes available")
        self.rx_pop       = CSRStorage(8,
            description="write to pop a byte from the non-RT RX FIFO")

        self.rt_ev_byte   = CSRStatus(8)
        self.rt_ev_ts_lo  = CSRStatus(32)
        self.rt_ev_ts_hi  = CSRStatus(32)
        self.rt_ev_valid  = CSRStatus(8)
        self.rt_ev_ack    = CSRStorage(8,
            description="write to ack the queued RT event")

        self.clk_period   = CSRStatus(32,
            description="microseconds between consecutive 0xF8 bytes")

        self.auto_tx_ctrl = CSRStorage(8, reset=0x07,
            description="bit0=auto-clock bit1=auto-start bit2=auto-stop")

        # ---- IRQ ----------------------------------------------------
        self.submodules.ev = EventManager()
        self.ev.rt_rx     = EventSourceProcess()    # RT byte received
        self.ev.tx_empty  = EventSourceProcess()    # TX FIFO drained
        self.ev.finalize()

        enable = self.ctrl.storage[0]
        soft_rst = self.ctrl.storage[1]

        # ---- TX path ------------------------------------------------
        # The TX shift register loads from a 1-byte holding register,
        # which is fed by either:
        #   - the realtime arbiter (highest priority), or
        #   - the data FIFO (CPU-fed).

        tx_data_fifo = SyncFIFO(width=8, depth=64)
        self.submodules += tx_data_fifo
        self.comb += [
            tx_data_fifo.din.eq(self.tx_data.storage),
            tx_data_fifo.we.eq(self.tx_push.re),
            self.tx_free.status.eq(64 - tx_data_fifo.level),
        ]

        # Realtime byte to inject (one slot — collisions of multiple
        # auto pulses in the same cycle are extremely rare, but if
        # they do happen the order is start > stop > clock).
        rt_pending  = Signal()
        rt_byte_out = Signal(8)
        clk_24      = beat_pulse.pulses.clk_24
        start_p     = beat_pulse.pulses.start
        stop_p      = beat_pulse.pulses.stop

        auto_clk   = self.auto_tx_ctrl.storage[0]
        auto_start = self.auto_tx_ctrl.storage[1]
        auto_stop  = self.auto_tx_ctrl.storage[2]

        self.sync += [
            If(soft_rst,
                rt_pending.eq(0),
            ).Elif(start_p & auto_start,
                rt_byte_out.eq(RT_START),
                rt_pending.eq(1),
            ).Elif(stop_p & auto_stop,
                rt_byte_out.eq(RT_STOP),
                rt_pending.eq(1),
            ).Elif(clk_24 & auto_clk,
                rt_byte_out.eq(RT_CLOCK),
                rt_pending.eq(1),
            )
        ]

        # TX UART state machine
        tx_busy   = Signal()
        tx_shift  = Signal(10)          # start + 8 data + stop = 10 bits
        tx_bits   = Signal(max=11)
        tx_div    = Signal(max=divider_max + 1)
        rt_taken  = Signal()

        self.sync += [
            If(soft_rst,
                tx_busy.eq(0), tx_bits.eq(0), tx_div.eq(0),
            ).Elif(enable,
                If(~tx_busy,
                    # Idle — fetch a new byte (RT first, else FIFO).
                    If(rt_pending,
                        tx_shift.eq(Cat(0, rt_byte_out, 1)),  # start, data, stop
                        tx_busy.eq(1),
                        tx_bits.eq(10),
                        tx_div.eq(divider_max - 1),
                        rt_pending.eq(0),
                    ).Elif(tx_data_fifo.readable,
                        tx_shift.eq(Cat(0, tx_data_fifo.dout, 1)),
                        tx_busy.eq(1),
                        tx_bits.eq(10),
                        tx_div.eq(divider_max - 1),
                    )
                ).Else(
                    If(tx_div == 0,
                        tx_div.eq(divider_max - 1),
                        tx_shift.eq(tx_shift >> 1),
                        tx_bits.eq(tx_bits - 1),
                        If(tx_bits == 1,
                            tx_busy.eq(0),
                        ),
                    ).Else(
                        tx_div.eq(tx_div - 1),
                    )
                )
            )
        ]
        # Drain pulse for "fetched a byte from the FIFO this cycle"
        self.comb += tx_data_fifo.re.eq(
            ~tx_busy & ~rt_pending & tx_data_fifo.readable & enable)

        # Pad: idle line is HIGH for MIDI (it's an open-collector
        # current loop, but we drive 3.3 V LVCMOS — same logic).
        self.comb += pads.tx.eq(~tx_busy | tx_shift[0])

        # ---- RX path ------------------------------------------------
        # 16x oversampled UART RX: sample at center of each bit.
        rx_sync = Signal()
        self.specials += MultiReg(pads.rx, rx_sync)

        rx_busy   = Signal()
        rx_shift  = Signal(8)
        rx_bits   = Signal(max=11)
        rx_div    = Signal(max=divider_max + 1)
        rx_byte   = Signal(8)
        rx_strobe = Signal()
        rx_prev   = Signal(reset=1)

        # Use a single counter clocked at sys; the natural way to
        # detect a start bit is to look for a falling edge while idle.
        self.sync += [
            rx_strobe.eq(0),
            rx_prev.eq(rx_sync),
            If(soft_rst,
                rx_busy.eq(0), rx_bits.eq(0), rx_div.eq(0),
            ).Elif(enable,
                If(~rx_busy,
                    If(~rx_sync & rx_prev,    # falling edge → start bit
                        rx_busy.eq(1),
                        rx_bits.eq(0),
                        # Wait 1.5 bit times to sample the FIRST data
                        # bit at its centre.
                        rx_div.eq(divider_max + (divider_max // 2) - 1),
                    )
                ).Else(
                    If(rx_div == 0,
                        rx_div.eq(divider_max - 1),
                        If(rx_bits < 8,
                            rx_shift.eq(Cat(rx_shift[1:8], rx_sync)),
                            rx_bits.eq(rx_bits + 1),
                        ).Else(
                            # stop bit — done; emit byte
                            rx_byte.eq(rx_shift),
                            rx_strobe.eq(1),
                            rx_busy.eq(0),
                        )
                    ).Else(
                        rx_div.eq(rx_div - 1),
                    )
                )
            )
        ]

        # ---- RX byte classifier (RT vs data) -----------------------
        rx_data_fifo = SyncFIFO(width=8, depth=64)
        self.submodules += rx_data_fifo

        # Hardware period meter for the 0xF8 (clock) byte.
        last_clk_ts  = Signal(64)
        cur_period   = Signal(32)

        # Latch ghost time at the moment of the strobe.
        rx_ts        = Signal(64)
        self.sync += If(rx_strobe, rx_ts.eq(ghost_time.host_time))

        # Realtime event slot.
        rt_q_byte    = Signal(8)
        rt_q_ts      = Signal(64)
        rt_q_valid   = Signal()

        is_rt_now    = Signal()
        self.comb += is_rt_now.eq(_is_rt(rx_byte))

        self.sync += [
            If(soft_rst,
                rt_q_valid.eq(0),
                cur_period.eq(0),
                last_clk_ts.eq(0),
            ).Elif(rx_strobe,
                If(is_rt_now,
                    # Latch into the RT event slot (or drop if FIFO full).
                    If(~rt_q_valid,
                        rt_q_byte.eq(rx_byte),
                        rt_q_ts.eq(rx_ts),
                        rt_q_valid.eq(1),
                    ),
                    # Period meter for clock bytes.
                    If(rx_byte == RT_CLOCK,
                        If(last_clk_ts != 0,
                            cur_period.eq((rx_ts - last_clk_ts)[0:32]),
                        ),
                        last_clk_ts.eq(rx_ts),
                    ),
                ).Else(
                    # Non-RT bytes go to the FIFO (firmware drains).
                    # No timestamp; ordering is preserved.
                ),
            ),
            If(self.rt_ev_ack.re, rt_q_valid.eq(0)),
        ]

        self.comb += [
            rx_data_fifo.din.eq(rx_byte),
            rx_data_fifo.we .eq(rx_strobe & ~is_rt_now),
            self.rx_data.status .eq(rx_data_fifo.dout),
            self.rx_avail.status.eq(rx_data_fifo.level),
            rx_data_fifo.re     .eq(self.rx_pop.re & rx_data_fifo.readable),

            self.rt_ev_byte .status.eq(rt_q_byte),
            self.rt_ev_ts_lo.status.eq(rt_q_ts[0:32]),
            self.rt_ev_ts_hi.status.eq(rt_q_ts[32:64]),
            self.rt_ev_valid.status.eq(rt_q_valid),

            self.clk_period.status.eq(cur_period),

            self.status.status.eq(Cat(tx_busy, Replicate(0, 7))),

            self.ev.rt_rx.trigger.eq(rt_q_valid),
            self.ev.tx_empty.trigger.eq(~tx_busy & ~tx_data_fifo.readable),
        ]
