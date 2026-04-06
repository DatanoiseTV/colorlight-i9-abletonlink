# LinkFPGA build environment.
#
# Bundles everything needed to synthesise the gateware and cross-compile
# the VexRiscv firmware:
#
#   - YosysHQ OSS CAD Suite (yosys + nextpnr-ecp5 + prjtrellis + ecpprog)
#   - RISC-V bare-metal GCC (gcc-riscv64-unknown-elf)
#   - LiteX, litex-boards, migen, litedram, liteeth (pinned to known
#     working revisions for the Colorlight i9 v7.2 target)
#   - Python 3, build tools, git
#
# Usage:
#
#   docker build -t linkfpga-builder LinkFPGA
#   docker run --rm -v "$PWD/LinkFPGA":/src -w /src linkfpga-builder \
#       python3 build.py --build
#
# The wrapper script `LinkFPGA/docker-build.sh` does this for you.

FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=Etc/UTC
ENV PATH=/opt/oss-cad-suite/bin:$PATH

# ----- 1. Base system + RISC-V toolchain ---------------------------------
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        curl \
        git \
        gcc-riscv64-unknown-elf \
        libgmp-dev \
        libmpc-dev \
        libmpfr-dev \
        libusb-1.0-0 \
        ninja-build \
        pkg-config \
        python3 \
        python3-pip \
        python3-setuptools \
        python3-wheel \
        wget \
        xz-utils \
        zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

# ----- 2. YosysHQ OSS CAD Suite ------------------------------------------
# A single tarball that bundles yosys, nextpnr-ecp5, prjtrellis, ecpprog,
# ghdl, ghdl-yosys-plugin, etc. Picks the right tarball for the target
# architecture (linux-x64 or linux-arm64) so the image works natively
# on both Intel and Apple Silicon hosts.
ARG TARGETARCH
ARG OSS_CAD_DATE=2024-08-01
RUN set -eux; \
    case "${TARGETARCH:-amd64}" in \
        amd64|x86_64) ARCH=linux-x64 ;; \
        arm64|aarch64) ARCH=linux-arm64 ;; \
        *) echo "unsupported TARGETARCH=${TARGETARCH}" >&2; exit 1 ;; \
    esac; \
    DATESTR="$(echo ${OSS_CAD_DATE} | tr -d -)"; \
    URL="https://github.com/YosysHQ/oss-cad-suite-build/releases/download/${OSS_CAD_DATE}/oss-cad-suite-${ARCH}-${DATESTR}.tgz"; \
    echo "Fetching $URL"; \
    curl -fsSL "$URL" -o /tmp/oss-cad.tgz; \
    mkdir -p /opt; \
    tar -xzf /tmp/oss-cad.tgz -C /opt; \
    rm /tmp/oss-cad.tgz

# ----- 3. LiteX + ecosystem ---------------------------------------------
RUN python3 -m pip install --no-cache-dir --upgrade pip \
    && python3 -m pip install --no-cache-dir \
        meson==1.5.1 \
        pyserial==3.5

# Use the official litex_setup.py to grab a coherent snapshot of LiteX
# and its sub-modules into /opt/litex.
RUN mkdir -p /opt/litex && cd /opt/litex \
    && curl -sSL https://raw.githubusercontent.com/enjoy-digital/litex/master/litex_setup.py \
       -o litex_setup.py \
    && python3 litex_setup.py --init --install --user --tag=2024.04 || \
       python3 litex_setup.py --init --install --user

# Make sure pip-installed scripts are in PATH for non-root work too.
ENV PATH=/root/.local/bin:$PATH

# ----- 4. Patch LiteX for current picolibc layout -----------------------
# Newer picolibc places `libc.a` directly in the build root, not under
# `newlib/`, and the include layout moved from `newlib/libc/tinystdio`
# and `newlib/libc/include` to `libc/include`. LiteX 2024.04's
# Makefiles still expect the old paths.
RUN sed -i 's|cp newlib/libc.a __libc.a|cp libc.a __libc.a|' \
    /opt/litex/litex/litex/soc/software/libc/Makefile && \
    sed -i \
      -e 's|-I$(PICOLIBC_DIRECTORY)/newlib/libc/tinystdio|-I$(PICOLIBC_DIRECTORY)/libc/include|' \
      -e 's|-I$(PICOLIBC_DIRECTORY)/newlib/libc/include|-I$(PICOLIBC_DIRECTORY)/libc/include/sys|' \
      /opt/litex/litex/litex/soc/software/common.mak && \
    sed -i '4a #include <inttypes.h>' \
      /opt/litex/litex/litex/soc/software/liblitedram/utils.c && \
    : "picolibc's stdio float parser uses __clzdi2 (64-bit clz) which" \
    : "the LiteX libcompiler_rt Makefile does NOT compile by default. Add it." && \
    sed -i \
      -e 's|clzsi2.o ctzsi2.o|clzsi2.o clzdi2.o ctzsi2.o|' \
      /opt/litex/litex/litex/soc/software/libcompiler_rt/Makefile

# ----- 4b. lwIP TCP/IP stack (vendored into the image) -----------------
# We use lwIP for the firmware's network stack: it gives us TCP (so the
# admin web UI is real HTTP, served from the device), IPv6 with MLD
# (the §4.1 IPv6 multicast discovery requirement), and a clean raw API
# for UDP that replaces libliteeth's single-slot ARP cache.
ARG LWIP_VERSION=STABLE-2_2_0_RELEASE
RUN curl -fsSL "https://github.com/lwip-tcpip/lwip/archive/refs/tags/${LWIP_VERSION}.tar.gz" \
        -o /tmp/lwip.tgz \
    && mkdir -p /opt/lwip \
    && tar -xzf /tmp/lwip.tgz -C /opt/lwip --strip-components=1 \
    && rm /tmp/lwip.tgz \
    && ls /opt/lwip/src/include/lwip/ | head -5

# ----- 5. Sanity check --------------------------------------------------
RUN yosys -V && \
    nextpnr-ecp5 --version && \
    riscv64-unknown-elf-gcc --version | head -1 && \
    python3 -c "import litex, litex_boards, migen, liteeth, litedram; \
                print('LiteX', litex.__file__)"

WORKDIR /src

# Default command shows the build help.
CMD ["python3", "build.py", "--help"]
