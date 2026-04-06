"""
EurorackInput — hardware edge detector + timestamp + period meter for
three Eurorack-style TTL inputs:

  CLK_IN   rising-edge clock at a configurable PPQN
  RST_IN   rising-edge reset (snap to bar 1, beat 1)
  RUN_IN   rising-edge play / falling-edge stop  (level-sensitive too)

Each input is brought in through a 3-stage MultiReg synchronizer for
metastability hardening. On every detected edge the module:

  1. Latches the GhostTime microsecond counter into a per-input
     "last event timestamp" register.
  2. For CLK_IN, computes the period since the previous rising edge
     and stores it in a CSR — firmware divides 60_000_000 by this to
     get BPM.
  3. Maintains a free-running edge counter so firmware can detect
     missed events (e.g. if the main loop was busy).
  4. Raises an IRQ.

CSRs:

  CTRL                8 bits  RW   bit0 = enable, bit1 = soft reset
  STATUS              8 bits  R    bit0 = run_level (current value of RUN_IN)

  CLK_COUNT          32 bits  R    cumulative rising-edge count on CLK_IN
  CLK_PERIOD_US      32 bits  R    period of the most recent CLK_IN edge
                                    (microseconds, 0 if not yet measured)
  CLK_TS_LO          32 bits  R    timestamp of last edge (low)
  CLK_TS_HI          32 bits  R    timestamp of last edge (high)

  RST_COUNT          32 bits  R    cumulative rising-edge count on RST_IN
  RST_TS_LO          32 bits  R
  RST_TS_HI          32 bits  R

  RUN_COUNT          32 bits  R    cumulative edges (rising + falling) on RUN_IN
  RUN_TS_LO          32 bits  R
  RUN_TS_HI          32 bits  R

The IRQ aggregates "any new edge" — firmware reads CLK_COUNT,
RST_COUNT, RUN_COUNT and remembers the last seen value to detect
which input fired.
"""

from migen import If, Module, Signal
from migen.genlib.cdc import MultiReg

from litex.soc.interconnect.csr import (
    AutoCSR, CSRStatus, CSRStorage,
)
from litex.soc.interconnect.csr_eventmanager import (
    EventManager, EventSourceProcess,
)


class EurorackInput(Module, AutoCSR):
    def __init__(self, pads, ghost_time):
        self.ctrl   = CSRStorage(8, reset=0x01)
        self.status = CSRStatus(8)

        self.clk_count    = CSRStatus(32)
        self.clk_period   = CSRStatus(32)
        self.clk_ts_lo    = CSRStatus(32)
        self.clk_ts_hi    = CSRStatus(32)

        self.rst_count    = CSRStatus(32)
        self.rst_ts_lo    = CSRStatus(32)
        self.rst_ts_hi    = CSRStatus(32)

        self.run_count    = CSRStatus(32)
        self.run_ts_lo    = CSRStatus(32)
        self.run_ts_hi    = CSRStatus(32)

        self.submodules.ev = EventManager()
        self.ev.edge = EventSourceProcess()
        self.ev.finalize()

        enable   = self.ctrl.storage[0]
        soft_rst = self.ctrl.storage[1]

        # Synchronizers
        clk_s = Signal()
        rst_s = Signal()
        run_s = Signal()
        self.specials += [
            MultiReg(pads.clk_in, clk_s, n=3),
            MultiReg(pads.rst_in, rst_s, n=3),
            MultiReg(pads.run_in, run_s, n=3),
        ]

        clk_prev = Signal()
        rst_prev = Signal()
        run_prev = Signal()

        clk_count = Signal(32)
        rst_count = Signal(32)
        run_count = Signal(32)

        clk_period   = Signal(32)
        last_clk_ts  = Signal(64)
        clk_ts_full  = Signal(64)
        rst_ts_full  = Signal(64)
        run_ts_full  = Signal(64)

        any_edge = Signal()

        self.sync += [
            clk_prev.eq(clk_s),
            rst_prev.eq(rst_s),
            run_prev.eq(run_s),
            any_edge.eq(0),

            If(soft_rst,
                clk_count.eq(0), rst_count.eq(0), run_count.eq(0),
                clk_period.eq(0), last_clk_ts.eq(0),
            ).Elif(enable,
                # Rising edge on CLK_IN
                If(clk_s & ~clk_prev,
                    clk_count.eq(clk_count + 1),
                    clk_ts_full.eq(ghost_time.host_time),
                    If(last_clk_ts != 0,
                        clk_period.eq(
                            (ghost_time.host_time - last_clk_ts)[0:32]),
                    ),
                    last_clk_ts.eq(ghost_time.host_time),
                    any_edge.eq(1),
                ),
                # Rising edge on RST_IN
                If(rst_s & ~rst_prev,
                    rst_count.eq(rst_count + 1),
                    rst_ts_full.eq(ghost_time.host_time),
                    any_edge.eq(1),
                ),
                # Any edge on RUN_IN (rising = play, falling = stop)
                If(run_s ^ run_prev,
                    run_count.eq(run_count + 1),
                    run_ts_full.eq(ghost_time.host_time),
                    any_edge.eq(1),
                ),
            ),
        ]

        self.comb += [
            self.status.status.eq(run_s),
            self.clk_count.status.eq(clk_count),
            self.clk_period.status.eq(clk_period),
            self.clk_ts_lo.status.eq(clk_ts_full[ 0:32]),
            self.clk_ts_hi.status.eq(clk_ts_full[32:64]),

            self.rst_count.status.eq(rst_count),
            self.rst_ts_lo.status.eq(rst_ts_full[ 0:32]),
            self.rst_ts_hi.status.eq(rst_ts_full[32:64]),

            self.run_count.status.eq(run_count),
            self.run_ts_lo.status.eq(run_ts_full[ 0:32]),
            self.run_ts_hi.status.eq(run_ts_full[32:64]),

            self.ev.edge.trigger.eq(any_edge),
        ]
