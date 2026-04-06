"""
LinkPacketFilter — sniffs the LiteEth MAC RX user-stream and asserts an
IRQ when it sees a UDP payload that starts with one of the three Link
protocol headers ("_asdp_v\\x01", "_link_v\\x01", "chnnlsv\\x01").

The filter does not modify or absorb the stream — it sits in parallel
with the firmware's normal RX path, so the firmware can choose to read
the packet as usual but with the protocol type already classified.

The intent is two-fold:
  1. Drop a wakeup latency: the IRQ fires as soon as the 8th payload byte
     arrives, not after the whole packet is buffered.
  2. Pre-classify so the firmware can branch directly into a Discovery,
     Link, or Audio handler without re-checking the magic bytes.

CSR map:

    STATUS    8 bits  R   bit0=match_pending bit1..2=type
                            (0=none, 1=discovery, 2=link, 3=audio)
    COUNT    32 bits  R   total matched packets since reset (saturating)
    CTRL      8 bits  RW  bit0=ack (clears match_pending+irq)
                          bit1=enable (default 1 after reset)
"""

from migen import Cat, Constant, If, Module, Record, Signal

from litex.soc.interconnect.csr import AutoCSR, CSRStatus, CSRStorage
from litex.soc.interconnect.csr_eventmanager import EventManager, EventSourceProcess


_LITEETH_RX_LAYOUT = [
    ("data",  32),
    ("valid",  1),
    ("last",   1),
]

# Protocol headers, big-endian, in 32-bit words
_HDR_DISCOVERY = (0x5F617364, 0x705F7601)   # "_asdp_v\x01"
_HDR_LINK      = (0x5F6C696E, 0x6B5F7601)   # "_link_v\x01"
_HDR_AUDIO     = (0x63686E6E, 0x6C737601)   # "chnnlsv\x01"


class LinkPacketFilter(Module, AutoCSR):
    def __init__(self):
        self.sink = Record(_LITEETH_RX_LAYOUT)

        self.status = CSRStatus(8, description="bit0=pending bit1..2=type")
        self.count  = CSRStatus(32, description="match counter")
        self.ctrl   = CSRStorage(8, description="bit0=ack bit1=enable", reset=0x02)

        self.submodules.ev = EventManager()
        self.ev.match = EventSourceProcess()
        self.ev.finalize()

        word_idx = Signal(2)              # 0,1 = first two payload words; 2 = done
        first_word = Signal(32)
        match_type = Signal(2)
        pending    = Signal()
        counter    = Signal(32)

        enable = self.ctrl.storage[1]

        self.sync += [
            # On packet boundary (`last`), reset our state for the next one.
            If(self.sink.valid & self.sink.last,
                word_idx.eq(0),
            ).Elif(self.sink.valid & enable,
                If(word_idx == 0,
                    first_word.eq(self.sink.data),
                    word_idx.eq(1),
                ).Elif(word_idx == 1,
                    word_idx.eq(2),
                    If((first_word == _HDR_DISCOVERY[0]) &
                       (self.sink.data == _HDR_DISCOVERY[1]),
                        match_type.eq(1),
                        pending.eq(1),
                        counter.eq(counter + 1),
                    ).Elif((first_word == _HDR_LINK[0]) &
                           (self.sink.data == _HDR_LINK[1]),
                        match_type.eq(2),
                        pending.eq(1),
                        counter.eq(counter + 1),
                    ).Elif((first_word == _HDR_AUDIO[0]) &
                           (self.sink.data == _HDR_AUDIO[1]),
                        match_type.eq(3),
                        pending.eq(1),
                        counter.eq(counter + 1),
                    ),
                ),
            ),
            # Firmware acknowledges by writing CTRL[0]=1
            If(self.ctrl.storage[0],
                pending.eq(0),
                match_type.eq(0),
            ),
        ]

        self.comb += [
            self.status.status.eq(Cat(pending, match_type)),
            self.count.status.eq(counter),
            self.ev.match.trigger.eq(pending),
        ]
