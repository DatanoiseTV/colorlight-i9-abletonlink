"""
BeatPulseGen — generates beat-clock and transport pulses on GPIO from
the GhostTimeUnit's 1 MHz tick and a small set of firmware-programmed
period registers.

The hardware is intentionally trivial: each pulse output is driven by
its own 32-bit microsecond down-counter that auto-reloads from a CSR.
The firmware programs the period (in microseconds) of each output
whenever the active timeline or tempo changes. Since timeline updates
arrive at most once every few hundred ms, this design has bounded
latency and avoids any runtime integer divide in gateware.

For each divisor `D` (24, 48, 96 PPQN, 1 PPQN beat, beats-per-bar):

    period_us = round(60 * 1e6 / (bpm * D))

For 1 PPS:

    period_us = 1_000_000

CSR map:

    PERIOD_24PPQN     32 bits  RW  microseconds between successive 24-PPQN pulses
    PERIOD_48PPQN     32 bits  RW
    PERIOD_96PPQN     32 bits  RW
    PERIOD_BEAT       32 bits  RW  microseconds per beat (= tempo / 1)
    PERIOD_BAR        32 bits  RW  microseconds per bar  (= tempo * beats_per_bar)
    PULSE_WIDTH_US    16 bits  RW  one-shot width in microseconds (default 1000)
    FLAGS              8 bits  RW  bit0=run bit1=start_req bit2=stop_req
                                   bit3=sync_led bit4=peer_led
    SOFT_RESET         8 bits  RW  bit0=reset all counters to 0 (one-shot)
"""

from migen import If, Module, Record, Signal

from litex.soc.interconnect.csr import AutoCSR, CSRStorage


_PULSE_LAYOUT = [
    ("clk_24",  1),
    ("clk_48",  1),
    ("clk_96",  1),
    ("beat",    1),
    ("bar",     1),
    ("start",   1),
    ("stop",    1),
    ("run",     1),
    ("pps",     1),
    ("sync_led",1),
    ("peer_led",1),
]


class BeatPulseGen(Module, AutoCSR):
    def __init__(self, sys_clk_freq, ghost_time):
        self.pulses = Record(_PULSE_LAYOUT)

        # ---- CSR registers ------------------------------------------
        self.period_24ppqn = CSRStorage(32, reset=20833,  # 120 BPM default
            description="microseconds between successive 24-PPQN pulses")
        self.period_48ppqn = CSRStorage(32, reset=10417,
            description="microseconds between successive 48-PPQN pulses")
        self.period_96ppqn = CSRStorage(32, reset=5208,
            description="microseconds between successive 96-PPQN pulses")
        self.period_beat   = CSRStorage(32, reset=500000,
            description="microseconds per beat")
        self.period_bar    = CSRStorage(32, reset=2000000,
            description="microseconds per bar (4 beats default)")
        self.pulse_width   = CSRStorage(16, reset=1000,
            description="one-shot pulse width in microseconds")
        self.flags         = CSRStorage(8,
            description="bit0=run bit1=start_req bit2=stop_req "
                        "bit3=sync_led bit4=peer_led")
        self.soft_reset    = CSRStorage(8,
            description="bit0=reset all counters")

        tick = ghost_time.tick_us
        rst  = self.soft_reset.storage[0]

        def make_periodic(period, name):
            cnt    = Signal(32)
            pulse  = Signal()
            width  = Signal(16)
            self.sync += [
                If(rst,
                    cnt.eq(0),
                    pulse.eq(0),
                    width.eq(0),
                ).Elif(tick,
                    If(cnt == 0,
                        cnt.eq(period - 1),
                        pulse.eq(1),
                        width.eq(self.pulse_width.storage),
                    ).Else(
                        cnt.eq(cnt - 1),
                        If(width != 0,
                            width.eq(width - 1),
                            If(width == 1, pulse.eq(0)),
                        ),
                    ),
                ),
            ]
            return pulse

        clk_24 = make_periodic(self.period_24ppqn.storage, "clk_24")
        clk_48 = make_periodic(self.period_48ppqn.storage, "clk_48")
        clk_96 = make_periodic(self.period_96ppqn.storage, "clk_96")
        beat   = make_periodic(self.period_beat  .storage, "beat")
        bar    = make_periodic(self.period_bar   .storage, "bar")

        # ---- run / start / stop -------------------------------------
        run         = Signal()
        run_prev    = Signal()
        start_pulse = Signal()
        stop_pulse  = Signal()
        sp_w        = Signal(16)
        tp_w        = Signal(16)
        self.comb += run.eq(self.flags.storage[0])
        self.sync += [
            run_prev.eq(run),
            If(run & ~run_prev,
                start_pulse.eq(1), sp_w.eq(self.pulse_width.storage),
            ).Elif(tick & (sp_w != 0),
                sp_w.eq(sp_w - 1),
                If(sp_w == 1, start_pulse.eq(0)),
            ),
            If(~run & run_prev,
                stop_pulse.eq(1), tp_w.eq(self.pulse_width.storage),
            ).Elif(tick & (tp_w != 0),
                tp_w.eq(tp_w - 1),
                If(tp_w == 1, stop_pulse.eq(0)),
            ),
        ]

        # ---- 1 PPS --------------------------------------------------
        pps_cnt = Signal(20)
        pps     = Signal()
        pps_w   = Signal(16)
        self.sync += If(tick,
            If(pps_cnt == 999_999,
                pps_cnt.eq(0),
                pps.eq(1),
                pps_w.eq(self.pulse_width.storage),
            ).Else(
                pps_cnt.eq(pps_cnt + 1),
                If(pps_w != 0,
                    pps_w.eq(pps_w - 1),
                    If(pps_w == 1, pps.eq(0)),
                ),
            ),
        )

        # ---- output muxing ------------------------------------------
        self.comb += [
            self.pulses.clk_24  .eq(clk_24),
            self.pulses.clk_48  .eq(clk_48),
            self.pulses.clk_96  .eq(clk_96),
            self.pulses.beat    .eq(beat),
            self.pulses.bar     .eq(bar),
            self.pulses.start   .eq(start_pulse),
            self.pulses.stop    .eq(stop_pulse),
            self.pulses.run     .eq(run),
            self.pulses.pps     .eq(pps),
            self.pulses.sync_led.eq(self.flags.storage[3]),
            self.pulses.peer_led.eq(self.flags.storage[4]),
        ]
