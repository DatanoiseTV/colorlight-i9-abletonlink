#!/usr/bin/env python3
"""
LinkFPGA — top-level build entry.

Targets: Colorlight i9 v7.2 (Lattice ECP5 LFE5U-45F-6BG381C, dual GbE,
4× HUB75 connectors).

Usage:
    python3 build.py --build              # synthesise + place & route
    python3 build.py --build --load       # also flash via SRAM (volatile)
    python3 build.py --build --flash      # write to SPI flash (persistent)
    python3 build.py --no-compile-gateware --load-firmware
                                          # just rebuild & reload firmware
    python3 build.py --build --num-tdm-ports 4 --num-physical-tdm-ports 2
                                          # 2 physical + 2 virtual TDM16 ports
"""

import argparse
import os

from litex_boards.platforms import colorlight_i5
# NB: import the parser via litex.build.tools to avoid the circular
# import in litex 2024.04 (litex.build.parser → cpu collection → soc.py
# → litex.build.parser).
from litex.soc.integration.builder import Builder
from litex.build.parser import LiteXArgumentParser

from litex_soc.soc import LinkFPGASoC
from litex_soc.platform_i9 import attach_link_io, MAX_PHYSICAL_TDM_PORTS


def main():
    parser = LiteXArgumentParser(
        platform=colorlight_i5.Platform,
        description="LinkFPGA: Ableton Link in hardware on Colorlight i9 v7.2.",
    )
    parser.add_target_argument(
        "--sys-clk-freq", default=50e6, type=float,
        help="System clock frequency in Hz (default 50 MHz). The current "
             "design closes timing comfortably at 50 MHz with all custom "
             "IP enabled (16-ch TDM serdes, beat pulse, ghost time, "
             "VexRiscv standard+debug). Higher frequencies fail STA on "
             "the SDRAM path.")
    parser.add_target_argument(
        "--num-tdm-ports", default=2, type=int,
        help="Total number of TDM16 ports (physical + virtual). Default 2.")
    parser.add_target_argument(
        "--num-physical-tdm-ports", default=2, type=int,
        help=f"How many TDM16 ports map to real HUB75 pins "
             f"(0..{MAX_PHYSICAL_TDM_PORTS}). Extras become virtual ports.")
    parser.add_target_argument(
        "--load-firmware", action="store_true",
        help="Reload firmware over UART without resynth.")
    parser.add_target_argument(
        "--with-second-eth", action="store_true",
        help="Bring up the second GbE PHY (default off).")

    args = parser.parse_args()

    platform = colorlight_i5.Platform(board="i9", revision="7.2",
                                      toolchain="trellis")
    attach_link_io(platform,
                   num_physical_tdm_ports=args.num_physical_tdm_ports)

    soc = LinkFPGASoC(
        platform,
        sys_clk_freq           = int(args.sys_clk_freq),
        num_tdm_ports          = args.num_tdm_ports,
        num_physical_tdm_ports = args.num_physical_tdm_ports,
        with_second_eth        = args.with_second_eth,
    )

    builder = Builder(soc, **parser.builder_argdict)

    # Wire our firmware (lwIP + Link/Link-Audio + HTTP UI) in. The
    # `add_software_package` call gets it compiled, and the matching
    # `add_software_library` call adds it to the BIOS link line so
    # `link_app_main` gets resolved (the BIOS main has been
    # sed-patched to call it after `boot_sequence()`).
    # Compile our firmware as a separate package (lwIP + Link +
    # Link-Audio + HTTP). The Makefile produces a standalone ELF that
    # links into main_ram (0x40000000). The user serialboots it via:
    #   litex_term --kernel link_firmware.bin /dev/ttyUSB0
    fw_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "firmware")
    builder.add_software_package("link_firmware", src_dir=fw_dir)

    if args.build or args.load_firmware:
        builder.build(
            run=args.build,
            **parser.toolchain_argdict,
        )

    if getattr(args, "load", False):
        prog = soc.platform.create_programmer()
        prog.load_bitstream(os.path.join(builder.gateware_dir,
                                         soc.platform.name + ".bit"))

    if getattr(args, "flash", False):
        prog = soc.platform.create_programmer()
        prog.flash(0, os.path.join(builder.gateware_dir,
                                   soc.platform.name + ".bit"))


if __name__ == "__main__":
    main()
