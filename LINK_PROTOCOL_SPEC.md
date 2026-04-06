# Ableton Link Wire Protocol Specification

**Status:** Informational — derived by reverse-engineering the reference
implementation at https://github.com/Ableton/link (this tree).
**Version:** Discovery v1 · Link v1 · Link-Audio v1
**Intended use:** Independent reimplementations on any platform (software,
RTOS, bare-metal, FPGA / LiteX). The reference implementation is the source
of truth; this document tracks it byte-for-byte.

---

## 0. Conventions

The key words **MUST**, **MUST NOT**, **SHOULD**, **SHOULD NOT**, and **MAY**
are to be interpreted as in RFC 2119.

- All multi-byte integers are transmitted in **network byte order (big-endian)**.
- Unless stated otherwise, all timestamps are signed 64-bit **microseconds**.
  Sender and receiver clocks are independent; alignment is performed by the
  ping/pong measurement procedure (§7).
- `u8`, `u16`, `u32`, `u64`, `i16`, `i32`, `i64` denote fixed-width integers.
- `uN[K]` denotes a contiguous array of K `uN` values, transmitted without
  padding.
- `bytes(N)` denotes N raw octets, transmitted verbatim.
- Byte offsets in field tables are relative to the start of the enclosing
  structure.
- "4CC" refers to a 32-bit identifier constructed from four ASCII characters
  and transmitted in network byte order. For example, `'tmln'` is the 4CC
  `0x746D6C6E` — on the wire, bytes `74 6D 6C 6E`, which spell "tmln" in
  ASCII. Every 4CC in this document is self-checking: deserialisers MAY
  verify that the 32-bit value parsed in network byte order matches the
  ASCII representation.
- The reference code marks each 4CC with a `static_assert(key == 0x...)` that
  pins byte order; those values are reproduced in Appendix A.

---

## 1. Introduction

Ableton Link is a peer-to-peer protocol that keeps a set of hosts on a local
IP network agreed on three pieces of state:

1. **Tempo** — the current beats-per-minute.
2. **Beat phase** — where "beat 0" lies on a shared virtual timeline.
3. **Start/stop state** — whether playback is currently running, and at
   which beat it last changed.

Peers discover each other via IP multicast, measure the offset between their
local clocks and a shared "ghost time" via unicast ping/pong, and then
exchange compact state updates. There is no central server, no leader, and
no persistent configuration. A peer may join or leave at any time.

The **Link-Audio** extension (§9) reuses the same peer discovery and
ghost-time infrastructure to stream short PCM audio packets between peers,
annotated with the beat position at which each packet should play.

The protocol is split into three sub-protocols, each with its own 8-byte
protocol header:

| Sub-protocol    | Protocol header bytes     | Transport         | Max UDP payload |
|-----------------|---------------------------|-------------------|----------------|
| Discovery v1    | `5F 61 73 64 70 5F 76 01` (`_asdp_v\x01`) | IPv4/IPv6 multicast + unicast replies | 512 B |
| Link v1         | `5F 6C 69 6E 6B 5F 76 01` (`_link_v\x01`) | Unicast only      | 512 B |
| Link-Audio v1   | `63 68 6E 6E 6C 73 76 01` (`chnnlsv\x01`) | Unicast only      | 1200 B |

The last byte of each protocol header is the binary version number **0x01**,
**not** the ASCII character `'1'`. A parser MUST reject messages whose first
8 bytes do not match one of the headers it implements.

---

## 2. Core Time Model

### 2.1 Host time

Every peer maintains a local monotonic clock measured in microseconds. The
reference uses a 64-bit signed counter and makes no assumption about its
epoch. A peer's host time is only meaningful to that peer; it is **not**
transmitted directly to other peers for synchronisation purposes.

### 2.2 Ghost time and `GhostXForm`

Every peer also maintains an affine transformation from host time to a
shared **ghost time**:

```
ghostTime = round(slope * hostTime) + intercept
hostTime  = round((ghostTime - intercept) / slope)
```

`slope` is a `float64`, `intercept` is `i64` microseconds.
The pair `(slope, intercept)` is called a `GhostXForm`.

- On startup, a peer initialises its own `GhostXForm` to
  `{ slope = 1.0, intercept = -currentHostTime }`, i.e. "ghost time equals
  my host time minus the host time at boot" — so ghost time starts near zero.
- When a peer joins an existing session, it measures the founder's ghost
  time (§7), derives a fresh `intercept` as the median of measured offsets,
  and holds `slope` at 1.0. The reference never adjusts `slope` — clock
  drift is absorbed by periodic remeasurement (§8.3).

`GhostXForm` is **never** serialised directly. It is a per-peer secret used
to translate between the peer's local clock and the session-wide shared
time axis.

### 2.3 Beats

Beats are stored and transmitted as `i64` **micro-beats** (fixed-point,
10⁶ units per beat). Conversion:

```
beats_float  = micro_beats / 1_000_000
micro_beats  = round(beats_float * 1_000_000)
```

### 2.4 Tempo

Tempo is transmitted as `i64` **microseconds-per-beat**. Conversion:

```
microsPerBeat = round(60_000_000 / bpm)
bpm           = 60_000_000 / microsPerBeat
```

Given a tempo T (in microseconds-per-beat), conversion between micro-beats
and host-time microseconds is:

```
micros_from_beats(mb) = round(mb * T / 1_000_000)
beats_from_micros(us) = round(us * 1_000_000 / T)
```

Implementations on platforms without hardware FPUs (FPGAs, microcontrollers)
MAY use the integer form shown above; no floating-point is required on the
wire.

### 2.5 Timeline

A `Timeline` is a triple

```
(tempo, beatOrigin, timeOrigin)
```

where `beatOrigin` (micro-beats) and `timeOrigin` (microseconds of the
owning peer's host time) are anchors, and `tempo` gives the slope.
To compute the beat at an arbitrary host time `t`:

```
beats(t) = beatOrigin + beats_from_micros(t - timeOrigin)
```

A peer's locally-authoritative timeline lives in its own host-time frame.
When transmitting to other peers, the `timeOrigin` must be understood as a
**ghost-time** instant — the receiver converts it through its own
`GhostXForm` to get a local host-time anchor.

### 2.6 StartStopState

A triple `(isPlaying: bool, beats: i64 microbeats, timestamp: i64 us)`
indicating whether playback is running, the beat position at which the
change took effect, and the **ghost-time** timestamp of the change.

### 2.7 NodeId and SessionId

Both are **8-byte opaque identifiers**. They MUST be generated with a
cryptographically adequate pseudo-random source at startup so that
collisions are astronomically unlikely.

```
NodeId     ::= bytes(8)
SessionId  ::= bytes(8)      -- identical wire format to NodeId
```

A session is identified by the NodeId of the peer that **founded** it. A
peer's current session is `sessionId` — this may be its own `NodeId`
(it founded the session) or another peer's NodeId (it joined).

Comparison (for tie-breaking, §8.2) is **lexicographic on the 8 bytes**,
treating each byte as unsigned.

---

## 3. Wire Primitives

All payloads in Discovery v1, Link v1, and every Link-Audio v1 message
type **except** `AudioBuffer` share the same TLV (Type-Length-Value)
encoding.

### 3.1 Fixed-width integer encoding

| Type | Size | Encoding |
|------|------|----------|
| `u8`, `i8`, `bool` | 1 B | single byte; `bool` sent as `0x00` (false) / `0x01` (true) |
| `u16`, `i16` | 2 B | big-endian (`htons`) |
| `u32`, `i32` | 4 B | big-endian (`htonl`) |
| `u64`, `i64` | 8 B | big-endian (`htonll`) |
| `microseconds` | 8 B | identical to `i64` |
| `NodeId`, `SessionId` | 8 B | verbatim, no byte swap |

### 3.2 String encoding

```
string    ::= length (u32, BE) || bytes(length)   -- UTF-8, no NUL terminator
```

### 3.3 Array encoding

A fixed-size array `T[N]` is encoded as N consecutive encodings of `T`,
with **no length prefix**. `NodeId` is specifically `u8[8]` encoded this way.

### 3.4 Vector (variable-length) encoding

```
vector<T> ::= count (u32, BE) || T[count]
```

### 3.5 Tuple encoding

Tuple elements are encoded in declaration order with no delimiters:

```
tuple(X, Y, Z) ::= encode(X) || encode(Y) || encode(Z)
```

### 3.6 PayloadEntryHeader (TLV entry)

```
PayloadEntryHeader (8 bytes):
  offset 0: u32 key    -- 4CC in network byte order
  offset 4: u32 size   -- number of value bytes that follow
```

A `PayloadEntry<T>` is a `PayloadEntryHeader` followed by `size` bytes of
value. Entries whose value size is zero **MUST** be omitted from the byte
stream by the sender (the reference does so in `Payload.hpp`); receivers
MUST tolerate this.

### 3.7 Payload parsing

A message body interpreted as a TLV payload is a **sequence** of
`PayloadEntry` records until the body ends. Order is **not significant**:
a compliant parser iterates entries, looks each `key` up in a handler
table, and ignores entries whose key it does not recognise. This provides
forward compatibility — new entry types can be introduced without breaking
existing peers.

A parser MUST detect:

1. A truncated `PayloadEntryHeader` (fewer than 8 bytes remaining) → error.
2. A `size` field that would extend past the end of the buffer → error.

Known keys SHOULD be parsed into structured values of the correct type; a
value whose encoded length does not match its declared `size` is an error.

---

## 4. Transport Layer

### 4.1 Multicast group and port

| Family | Address                     | Port (UDP) |
|--------|-----------------------------|------------|
| IPv4   | `224.76.78.75`              | `20808`    |
| IPv6   | `ff12::8080%<scope>` (link-local) | `20808` |

- Port **20808** is used for all three sub-protocols (discovery, Link v1
  measurement, Link-Audio v1). Each peer binds one unicast socket per
  network interface.
- On IPv4 the multicast TTL SHOULD be 1 (link-local scope). The reference
  also disables `IP_MULTICAST_ALL` on Linux so that the peer only receives
  on the interface it explicitly joined.
- On IPv6 a scope-id MUST be attached to the multicast address and the
  unicast socket MUST be bound to a link-local address on the same
  interface so that the scope is unambiguous.
- Dual-stack peers SHOULD run the discovery protocol on both families
  simultaneously.

### 4.2 Per-interface operation

The reference opens one multicast receive socket plus one unicast
send/receive socket **per network interface** (`IpInterface.hpp:55-60`).
Peer discovery and ping/pong are therefore scoped to individual
interfaces — a peer with two NICs appears as two independent "gateways"
to the rest of the subnet. A reimplementation with a single interface
does not need to replicate this duplication.

### 4.3 Unicast endpoints

Every peer publishes one or two **measurement endpoints** in its state
broadcasts (§5.2, keys `mep4` / `mep6`): an `(IP, port)` pair on which
it accepts Link v1 PING messages. A peer MAY also publish one or two
**audio endpoints** (keys `aep4` / `aep6`) — a second `(IP, port)` pair
that accepts Link-Audio v1 traffic. The reference binds measurement and
audio on separate sockets; a reimplementation MAY collapse them if it can
demultiplex by protocol header.

---

## 5. Discovery Sub-Protocol (Discovery v1)

Every Discovery v1 datagram is framed as:

```
+--------------------+--------------------+---------------------+
| ProtocolHeader (8) | MessageHeader (12) | Payload (variable)  |
+--------------------+--------------------+---------------------+
```

### 5.1 Message framing

**ProtocolHeader (8 bytes)** — literal bytes:

```
0x5F 0x61 0x73 0x64 0x70 0x5F 0x76 0x01   ( "_asdp_v\x01" )
```

**MessageHeader (12 bytes)** — fields in declaration/transmission order:

| Offset | Size | Field         | Type     | Notes |
|-------:|-----:|---------------|----------|-------|
| 0      | 1    | `messageType` | u8       | 1 = ALIVE, 2 = RESPONSE, 3 = BYEBYE |
| 1      | 1    | `ttl`         | u8       | seconds; see §5.4 |
| 2      | 2    | `groupId`     | u16 BE   | always `0x0000` in the reference |
| 4      | 8    | `ident`       | NodeId   | sender's NodeId |

**Maximum datagram size** (protocol header + message header + payload):
512 bytes. Senders MUST NOT exceed this; receivers MUST tolerate
datagrams that approach this limit. The reference `kMaxMessageSize = 512`.

The 16-bit `groupId` field exists to carve independent Link networks out
of a single multicast group. The reference always emits `0` and ignores
messages whose `groupId` differs from its own. Reimplementations SHOULD
keep it at 0 unless they have a specific need to partition traffic.

### 5.2 Payload for ALIVE / RESPONSE (NodeState + PeerState)

The payload carries the peer's full, currently-authoritative state,
encoded as a sequence of PayloadEntry records in any order.

| 4CC     | Hex (wire)    | Value type / layout | Value size | Meaning |
|---------|---------------|---------------------|-----------:|---------|
| `tmln`  | `74 6D 6C 6E` | tuple(Tempo, Beats, microseconds) = `i64 BE`, `i64 BE`, `i64 BE` | 24 B | Timeline: microseconds-per-beat, beat origin (micro-beats), time origin (ghost-time microseconds) |
| `sess`  | `73 65 73 73` | NodeId / 8 raw bytes | 8 B   | SessionId the peer currently belongs to |
| `stst`  | `73 74 73 74` | tuple(bool, Beats, microseconds) = `u8`, `i64 BE`, `i64 BE` | 17 B | StartStopState: `isPlaying`, beat position, ghost-time timestamp |
| `mep4`  | `6D 65 70 34` | tuple(u32 BE IPv4, u16 BE port) | 6 B | IPv4 measurement endpoint |
| `mep6`  | `6D 65 70 36` | tuple(u8[16] IPv6 addr, u16 BE port) | 18 B | IPv6 measurement endpoint |
| `aep4`  | `61 65 70 34` | tuple(u32 BE IPv4, u16 BE port) | 6 B | IPv4 audio endpoint (Link-Audio only) |
| `aep6`  | `61 65 70 36` | tuple(u8[16] IPv6 addr, u16 BE port) | 18 B | IPv6 audio endpoint (Link-Audio only) |

Notes:

- A peer with no IPv6 address SHOULD still send an `mep4` entry. If the
  reference sees a peer with an IPv4 measurement endpoint it omits `mep6`
  (its `sizeInByteStream` returns 0, so the entry is skipped, §3.6). The
  symmetric rule applies for `aep4` / `aep6`. Reimplementations SHOULD
  follow the same convention: **publish one measurement endpoint per
  address family you can respond on**, and omit the others.
- If the peer does not run the Link-Audio extension, it MUST omit both
  `aep4` and `aep6`.
- The `timeOrigin` inside `tmln` is a **ghost-time** value. Receivers
  that have not yet measured this peer's session (§7) cannot map it to
  local host time and therefore cannot yet render its timeline.

A minimal compliant ALIVE payload is `tmln + sess + stst + one of
{mep4, mep6}` — 62 bytes plus four 8-byte TLV headers = 94 bytes. Typical
messages fit comfortably inside the 512-byte cap.

### 5.3 Message types

#### ALIVE (type = 1)

- **Direction:** multicast → multicast group.
- **Trigger:** periodic heartbeat; also on state change (see §5.4).
- **Payload:** full PeerState per §5.2.
- **Purpose:** announces "I exist, here is my current state".

#### RESPONSE (type = 2)

- **Direction:** unicast → the sender of the ALIVE that provoked it.
- **Trigger:** on receipt of an ALIVE whose `ident` is not our own.
- **Payload:** identical to ALIVE (full PeerState).
- **Purpose:** so a newly-joined peer doesn't have to wait up to one
  heartbeat period for the rest of the group to re-broadcast; it learns
  every peer's state within one round-trip after its first ALIVE.

#### BYEBYE (type = 3)

- **Direction:** multicast → multicast group.
- **Trigger:** orderly shutdown.
- **Payload:** empty (zero bytes after the message header).
- **TTL field:** MUST be `0`.
- **Purpose:** lets receivers evict the peer immediately instead of
  waiting for its TTL to expire.

### 5.4 Heartbeat and liveness

Let `TTL_SEC` be the value carried in the `ttl` field. The reference uses:

```
TTL_SEC                  = 5            (seconds)
TTL_RATIO                = 20
NOMINAL_BROADCAST_PERIOD = TTL_SEC * 1000 / TTL_RATIO   = 250 ms
MIN_BROADCAST_PERIOD     = 50 ms         (flood guard)
```

A peer SHOULD broadcast an ALIVE every `NOMINAL_BROADCAST_PERIOD` and on
any state change, but MUST NOT broadcast two ALIVEs within
`MIN_BROADCAST_PERIOD` of each other.

On receipt of an ALIVE or RESPONSE from a peer `P`:

1. Record `P`'s state.
2. Set `P`'s expiry to `now + TTL_SEC`.
3. If `P` is already known, refresh its expiry; otherwise add it.

A peer MUST prune entries whose expiry has passed. The reference adds a
1-second grace period on top of `TTL_SEC` before the actual sweep
(`PeerGateway.hpp:164`). BYEBYE immediately purges the sender, bypassing
the timer.

---

## 6. Session Model

### 6.1 Founding

At startup a peer sets `sessionId := ownNodeId`, initialises its
`GhostXForm` to `{1.0, -nowHostTime}`, and picks a default Timeline
(120 BPM, `beatOrigin = 0`, `timeOrigin = ownGhostNow`). It is now the
sole member of a session it founded.

### 6.2 Observing other sessions

When an ALIVE arrives with `sess != ownSessionId`, the peer notes the
existence of this foreign session but **cannot** act on its timeline
until it has measured the session's ghost-time offset (§7). Until then
the peer simply tracks the foreign session in a "known sessions" set.

### 6.3 Measurement trigger

A peer launches a measurement against a foreign session as soon as:

1. It has seen at least one peer claiming that session.
2. No measurement against that session is already in flight.

The measurement target selection rule (`Sessions.hpp:116-124`):

1. **Prefer the founding peer**, i.e. a peer whose `NodeId` equals the
   session's `SessionId`.
2. Otherwise, the first peer seen in that session.

### 6.4 Session tie-break

After a successful measurement the peer computes, at the current host
time `now`:

```
curGhost = ownXForm.hostToGhost(now)
newGhost = measuredXForm.hostToGhost(now)
ghostDiff = newGhost - curGhost
```

The foreign session **wins** (the peer joins it) iff either:

```
ghostDiff > 500_000 us     (500 ms)
```

or

```
|ghostDiff| < 500_000 us  AND  foreignSessionId < ownSessionId
```

Otherwise the foreign session is recorded as "already measured, not
adopted" and is not re-measured until the peer sees new evidence for it.

The `SESSION_EPS = 500 ms` tolerance prevents chronic flapping between
two sessions whose ghost times are numerically identical.

### 6.5 Remeasurement

While a session is the peer's active session, the peer schedules a
remeasurement every **30 s** (`Sessions.hpp:186`) to absorb clock drift.
On measurement failure it reschedules another attempt.

### 6.6 Timeline merging within a session

Within a single session, all peers contribute timelines; the
authoritative timeline seen by a peer is the one whose `timeOrigin`
(after translation through that peer's ghost-time coordinate) is
**latest** — newer tempo/beat edits win. StartStopState merging follows
the same "latest timestamp wins" rule.

---

## 7. Link v1 Ping/Pong Measurement

The purpose of this sub-protocol is to construct, for a foreign session,
a `GhostXForm` that maps the local host time to that session's ghost
time. It runs over **unicast UDP** between the measurer's socket and the
target peer's advertised measurement endpoint (`mep4` / `mep6`).

### 7.1 Framing

```
+--------------------+-------------------+---------------------+
| ProtocolHeader (8) | MessageHeader (1) | Payload (TLV)       |
+--------------------+-------------------+---------------------+
```

**ProtocolHeader:** `5F 6C 69 6E 6B 5F 76 01` (`_link_v\x01`).

**MessageHeader (1 byte)** — *this is a different, shorter header than
Discovery v1*:

| Offset | Size | Field | Values |
|-------:|-----:|-------|--------|
| 0 | 1 | `messageType` | 1 = PING, 2 = PONG |

There is **no** ttl, groupId, or node id in Link v1's header — the
measurer relies on the unicast (IP, port) pair to identify the
responder. Max message size is 512 B.

### 7.2 Payload entries

In addition to the Discovery v1 keys, the ping/pong exchange uses three
new entries (defined in `PayloadEntries.hpp`):

| 4CC     | Hex (wire)    | Value type | Value size | Meaning |
|---------|---------------|------------|-----------:|---------|
| `__ht`  | `5F 5F 68 74` | `i64` microseconds (host time) | 8 B | A *host-time* instant from the measurer's frame |
| `__gt`  | `5F 5F 67 74` | `i64` microseconds (ghost time)| 8 B | A *ghost-time* instant from the responder's frame |
| `_pgt`  | `5F 70 67 74` | `i64` microseconds (ghost time)| 8 B | The responder's *previous* ghost-time sample |

The `sess` (SessionMembership) entry defined in §5.2 is also re-used in
PONG responses so the measurer can verify the responder still belongs to
the session being measured.

### 7.3 Exchange

Let `M` be the measurer and `R` be the responder.

```
round i = 0:
  M.hostTime_i := M.clock.micros()
  M -> R:  PING { HostTime = M.hostTime_i }

round i > 0:
  M.hostTime_i := M.clock.micros()
  M -> R:  PING { HostTime = M.hostTime_i,
                  PrevGHostTime = R.ghost_{i-1} }   -- R's last reply
```

On each PING:

```
R -> M:  PONG {
           SessionMembership = R.sessionId,
           GHostTime         = R.xform.hostToGhost(R.clock.micros()),
           -- plus the PING's own payload appended verbatim:
           HostTime          = <echoed M.hostTime_i>,
           PrevGHostTime     = <echoed from PING if present>
         }
```

The reference responder (`PingResponder.hpp:129-148`) literally
concatenates the received payload bytes onto its own reply. A
reimplementation MAY achieve the same effect by echoing the parsed
fields back as fresh TLV entries — receivers simply parse by key.

### 7.4 Sample computation

From a single PONG carrying
`(hostTime = t1, prevHostTime = t0, ghostTime = g, prevGHostTime = g0)`
the measurer appends up to two data points to its sample buffer:

```
   data += g - (t1 + t0) / 2
   if g0 != 0:
     data += (g + g0) / 2 - t0
```

(see `Measurement.hpp:185-195`). Both samples estimate
`ghost - host`, i.e. the intercept of a `slope = 1` `GhostXForm`.

### 7.5 Rounds, filtering, termination

```
PING_INTERVAL          = 50 ms
MAX_PINGS_PER_ROUND    = 5     (kNumberMeasurements)
REQUIRED_SAMPLE_COUNT  = 100   (kNumberDataPoints)
```

1. The measurer sends the first PING immediately, then arms a timer for
   `PING_INTERVAL`.
2. On each PONG it computes the samples above, then sends a fresh PING.
3. If it has collected **more than 100 samples**, the measurement
   succeeds and the result is:

   ```
   GhostXForm { slope = 1.0, intercept = median(samples) us }
   ```
4. If `MAX_PINGS_PER_ROUND` PINGs have been sent without yielding 100
   samples, the measurement fails and the session is discarded (or, if
   it was the active session, a retry is scheduled).
5. If a PONG arrives whose `SessionMembership` does not match the
   session under measurement, the measurement fails immediately.

A minimal reimplementation on constrained hardware MAY use a smaller
`REQUIRED_SAMPLE_COUNT` at the cost of noisier sync; it SHOULD keep the
median filter.

---

## 8. Peer State Machines (summary)

### 8.1 On packet receive (Discovery v1 listener)

```
loop:
    recv datagram
    if first 8 bytes != "_asdp_v\x01": discard
    parse MessageHeader
    if header.ident == ownNodeId: discard   -- our own broadcast
    if header.groupId != ownGroupId: discard
    switch header.messageType:
      case ALIVE:
          upsert peer state; refresh TTL; send RESPONSE unicast
      case RESPONSE:
          upsert peer state; refresh TTL
      case BYEBYE:
          evict peer immediately
```

### 8.2 On packet receive (Link v1 measurement responder)

```
loop:
    recv unicast datagram
    if first 8 bytes != "_link_v\x01": discard
    parse 1-byte header
    if type == PING:
       parse payload, extract HostTime (ht) and any PrevGHostTime (pgt)
       now_host = clock.micros()
       gt       = ghostXForm.hostToGhost(now_host)
       send PONG {
         sess = ownSessionId,
         __gt = gt,
         -- echo the ping payload verbatim:
         __ht = ht,
         _pgt = pgt   (if present)
       }
```

### 8.3 Session supervisor

```
every time a peer's sessionId changes in our view:
    if foreign session and no measurement active:
        pick target peer (founder if present, else any)
        launch Measurement
every 30s:
    relaunch Measurement against current active session
```

The measurement subsystem is intentionally decoupled from discovery:
measurement does not add or remove peers, and discovery does not drive
sample collection — it only informs the supervisor of sessionId changes.

### 8.4 Authoring local state updates

A peer that wants to change tempo, beat phase, or play/stop builds a new
local Timeline or StartStopState relative to **its own host time**, then:

1. Converts `timeOrigin` / `timestamp` to **ghost time** via its current
   `GhostXForm` before placing them in the `tmln` / `stst` entries.
2. Broadcasts an ALIVE immediately (subject to `MIN_BROADCAST_PERIOD`).

Peers in the same session reading that broadcast invert the conversion
via their own `GhostXForm` to recover a local host time.

---

## 9. Link-Audio Extension (Link-Audio v1)

The Link-Audio extension is an **optional** layer that uses Link's peer
discovery and ghost-time synchronisation to stream short, beat-tagged
PCM audio packets between peers. A peer that does not advertise `aep4`
/ `aep6` in its Discovery v1 state is not participating in this
extension and MUST NOT receive audio traffic.

### 9.1 Transport

Link-Audio v1 is **unicast-only**. Audio peers discover each other by
parsing the `aep4` / `aep6` entries in each other's Discovery v1 ALIVE
messages. Once a peer's audio endpoint becomes known, the local
Link-Audio engine opens a unicast relationship with it (implemented as
an entry in a receivers table; the reference `UdpMessenger.hpp:186-191`).

There is no audio multicast group. Every audio datagram is addressed to
exactly one receiver.

### 9.2 Framing

Every Link-Audio v1 datagram is framed as:

```
+--------------------+--------------------+----------------------+
| ProtocolHeader (8) | MessageHeader (12) | Body (variable)      |
+--------------------+--------------------+----------------------+
```

**ProtocolHeader:** `63 68 6E 6E 6C 73 76 01` (`chnnlsv\x01`).

**MessageHeader (12 bytes):** identical in layout to the Discovery v1
message header (§5.1):

| Offset | Size | Field         | Values |
|-------:|-----:|---------------|--------|
| 0 | 1 | `messageType` | see §9.3 |
| 1 | 1 | `ttl` | seconds; `0` for AudioBuffer |
| 2 | 2 | `groupId` | `0x0000` |
| 4 | 8 | `ident` | NodeId of the sender |

**Max datagram size:** 1200 B (chosen to fit inside IPv6's 1280 B path
MTU minus headers). Senders MUST NOT produce larger datagrams;
receivers MUST accept up to this size.

### 9.3 Message types

| Type | Value | Body                    | Transport | Sender → Receiver |
|------|------:|-------------------------|-----------|-------------------|
| `kPeerAnnouncement`    | 1 | TLV payload (§9.4) | Unicast to every known audio peer | advertise own channels + embed PING |
| `kChannelByes`         | 2 | TLV payload (`aucb`)| Unicast | retire channels |
| `kPong`                | 3 | TLV payload (`__ht`)| Unicast | reply to an embedded PING |
| `kChannelRequest`      | 4 | TLV payload (`chid`)| Unicast to source peer | subscribe to a channel |
| `kStopChannelRequest`  | 5 | TLV payload (`chid`)| Unicast to source peer | unsubscribe |
| `kAudioBuffer`         | 6 | **Raw** AudioBuffer (§9.8) | Unicast to every subscriber | stream audio |

**Important encoding difference:** message types 1 through 5 have a body
that is a Discovery-style TLV payload. Message type 6 (`kAudioBuffer`)
is the **only** message whose body is not TLV-wrapped: the `AudioBuffer`
struct is serialised directly after the message header. The struct's
own self-describing header acts in place of the `PayloadEntry` wrapper.

### 9.4 PeerAnnouncement (type 1)

Body is a TLV payload with the following entries:

| 4CC     | Hex (wire)    | Value | Meaning |
|---------|---------------|-------|---------|
| `sess`  | `73 65 73 73` | SessionId (8 B)                | Link session this audio announcement belongs to |
| `__pi`  | `5F 5F 70 69` | string (u32 length + UTF-8) | `PeerInfo.name` — human-readable peer name |
| `auca`  | `61 75 63 61` | vector<ChannelAnnouncement> | list of channels this peer offers |
| `__ht`  | `5F 5F 68 74` | `i64` microseconds             | **Optional** — present once per broadcast round, carries the sender's local host time (ping) |

**ChannelAnnouncement encoding:**

```
ChannelAnnouncement ::=
    string name            -- u32 length + UTF-8 bytes
    NodeId channelId       -- 8 raw bytes
```

**Multi-packet announcements.** If a peer has too many channels to fit
in a single 1200-byte datagram it splits them across several consecutive
`kPeerAnnouncement` messages (reference: `UdpMessenger.hpp:265-287`).
The split is transparent: each packet is a self-contained
PeerAnnouncement carrying a subset of `auca`. The `__ht` ping entry is
attached to **exactly one** packet per round (the first) to avoid
inflating the sample set.

**Broadcast cadence.** Identical to Discovery v1:

```
ttl                 = 5 s
nominalPeriod       = 250 ms      (ttl * 1000 / ttlRatio, ttlRatio = 20)
minPeriod           = 50 ms       (flood guard)
```

### 9.5 ChannelByes (type 2)

Body:

```
aucb : vector<ChannelBye>
    ChannelBye ::= NodeId channelId      -- 8 raw bytes
```

Sent unicast to every audio receiver when channels are removed (or at
peer shutdown, via the messenger's destructor, see
`UdpMessenger.hpp:126-133`). Reimplementations SHOULD emit it on
orderly shutdown; receivers MUST tolerate the case where it never
arrives (use the discovery TTL as a fallback).

If the byes list is larger than will fit in one datagram, the sender
splits it across multiple ChannelByes messages.

### 9.6 Audio Ping (`__ht` in PeerAnnouncement) and Pong (type 3)

Link-Audio has its own independent ping/pong for measuring
round-trip time between audio peers. It is **not** the same handshake
as Link v1 measurement (§7). Rules:

1. Audio PING is carried as a `__ht` TLV entry inside a regular
   `kPeerAnnouncement` datagram, once per broadcast round per receiver.
2. On receipt, the peer sends a **separate** `kPong` datagram back to
   the sender, whose body is a TLV payload containing a single `__ht`
   entry echoing the **same** `i64` value that was received.
3. The original sender computes:

   ```
   rtt = recvTime - echoedHostTime
   ```

   where `recvTime` is its own host time when the pong arrived.
4. RTTs are fed into a jitter filter (`NetworkMetrics.hpp`): a sliding
   window of the last 10 samples, from which mean and stddev are
   computed, yielding a quality score

   ```
   speed   = 1e6 / meanRTT
   jitter  = sqrt(var(RTTs))
   quality = speed / (1 + jitter)
   ```

   The reference uses the score for monitoring only; it does not drive
   adaptive bitrate or retransmission.

### 9.7 ChannelRequest (type 4) / ChannelStopRequest (type 5)

Body (both message types, same format):

```
TLV payload with exactly one entry:
    chid : NodeId channelId        (8 raw bytes)
```

The source peer (sender of `kPeerAnnouncement`) is identified by the
unicast destination. The requesting peer is identified by the
`MessageHeader.ident` field. TTL is carried in `MessageHeader.ttl` and
indicates how long the request is valid; the reference re-sends requests
every `kTtl = 5 s` until it receives the requested stream or gives up.

On receipt of `kChannelRequest`, the source peer MUST add the requester
to its subscriber set for that channel and start emitting
`kAudioBuffer` datagrams to it.

On receipt of `kStopChannelRequest` (or when the request's TTL lapses
without renewal), the source peer MUST remove the subscription.

### 9.8 AudioBuffer (type 6)

This is the only message whose body is **not** TLV-encoded. The
AudioBuffer struct is serialised directly after the 12-byte message
header, in the order shown below. All multi-byte fields are big-endian.

```
AudioBuffer (body of a kAudioBuffer datagram):

    NodeId  channelId                     -- 8 raw bytes
    NodeId  sessionId                     -- 8 raw bytes
    u32     chunkCount                    -- BE, >= 1
    Chunk   chunks[chunkCount]
    u8      codec                         -- see §9.9
    u32     sampleRate                    -- BE, Hz
    u8      numChannels                   -- 1 (mono) or 2 (stereo)
    u16     numBytes                      -- BE, length of sampleBytes in bytes
    u8      sampleBytes[numBytes]         -- raw PCM payload (see §9.9)

Chunk:

    u64     count                         -- BE, monotonic sequence number
    u16     numFrames                     -- BE, frames in this chunk
    i64     beginBeats                    -- BE, micro-beats at chunk start (see §9.10)
    i64     tempoMicrosPerBeat            -- BE, tempo valid during this chunk
```

Every chunk has 26 bytes of header. A parser MUST verify:

```
sum(chunks[i].numFrames)  *  numChannels  *  bytesPerSample(codec)  ==  numBytes
```

otherwise the message MUST be rejected (the reference throws
`range_error` on mismatch, `AudioBuffer.hpp:193-197`).

**`channelId`** is the 8-byte identifier of the channel this audio
belongs to; it MUST match a channel the recipient has previously
subscribed to via §9.7. **`sessionId`** is the Link session identifier
at the time of capture — if the recipient is currently in a different
session it MUST NOT attempt to map the chunk's `beginBeats` onto its own
beat grid.

**`kNonAudioBytes = 50`** is the reference's conservative bound for the
overhead of the AudioBuffer struct minus its sample bytes; combined with
`kMaxPayloadSize = 1176` (i.e. 1200 − 24) this yields an effective upper
bound of `1126` sample bytes per datagram. The `Resizer` in the
reference further tightens this to 502 bytes per datagram on the send
side to stay inside the RFC 791 576-byte safe-MTU envelope. A
reimplementation targeting a known network (e.g. a FPGA on an isolated
gigabit LAN) MAY use the full 1126 bytes; a reimplementation targeting
the open Internet SHOULD use the tighter 502-byte limit.

`MessageHeader.ttl` for AudioBuffer MUST be `0`: audio packets are never
cached by intermediaries and have no liveness semantics.

### 9.9 PCM codec

```
Codec ::= u8
    0 = kInvalid  -- MUST NOT appear on the wire
    1 = kPCM_i16  -- signed 16-bit PCM samples, big-endian,
                     interleaved if numChannels > 1
```

`kPCM_i16` is the only codec defined in Link-Audio v1. Future
revisions MAY add codec identifiers; receivers MUST reject unknown
codec values.

**Sample layout for kPCM_i16:**

- Each sample is `i16`, big-endian.
- Multi-channel data is **interleaved**: `[L, R, L, R, ...]` for stereo.
- `bytesPerSample = 2`.

### 9.10 Beat-time mapping of audio

Each Chunk carries `(beginBeats, tempo)`. The receiver uses them, plus
the common session's Timeline and the receiver's own GhostXForm, to
compute the exact local host time at which the chunk's first frame must
play. Let:

```
beat0  = chunk.beginBeats      -- micro-beats in the session's beat grid
bpmUs  = chunk.tempoMicrosPerBeat

-- Use the receiver's authoritative timeline for the session (§6.6)
-- to convert beat0 to a ghost-time instant:
ghost_t = timeline.fromBeats(beat0)

-- And through the receiver's GhostXForm to local host time:
host_t  = ownXForm.ghostToHost(ghost_t)
```

The audio should be scheduled at `host_t` in the receiver's audio
output buffer. If `host_t` is already in the past by more than the
audio system's slack budget, the frame MUST be dropped; if it is too
far in the future, the receiver SHOULD buffer it.

Subsequent frames within the chunk advance at `sampleRate` Hz **on the
sender's clock**; the receiver SHOULD either resample to its own output
rate, or use the Link time axis to interpolate / decimate as needed.
The protocol does not mandate a particular drift-compensation strategy.

### 9.11 Buffering guidelines (non-normative)

The reference uses:

- A 128-slot lock-free ring queue between network and audio worker
  (`Sink.hpp:41`).
- A ~4096 × 2 sample accumulator per source on the receive side
  (`SourceProcessor.hpp:80`).
- A ~10-second source-queue ceiling (`Source.hpp:35`).

These numbers are advisory, not normative. An FPGA/LiteX implementation
with bounded BRAM MAY use much smaller buffers, accepting a higher
underrun risk in exchange for lower latency.

### 9.12 Failure modes

Link-Audio v1 has **no** retransmission, FEC, or loss feedback. A
receiver MUST tolerate gaps in the `count` sequence of a channel's
chunks and either mute, interpolate, or drop. Networks with >~0.5%
packet loss will have audible artefacts; this is by design — the goal
is low latency, not reliable delivery.

---

## 10. Constants Summary

```
-- Transport
MCAST_V4                 = 224.76.78.75
MCAST_V6                 = ff12::8080 (link-local scope)
UDP_PORT                 = 20808

-- Protocol headers (8 bytes, literal)
HDR_DISCOVERY_V1         = "_asdp_v" 0x01
HDR_LINK_V1              = "_link_v" 0x01
HDR_LINK_AUDIO_V1        = "chnnlsv" 0x01

-- Discovery v1
DISCOVERY_MAX_MSG        = 512  bytes
DISCOVERY_TTL            = 5    seconds  (in header's ttl field)
DISCOVERY_TTL_RATIO      = 20
DISCOVERY_BCAST_PERIOD   = 250  ms       (nominal, = ttl*1000/ratio)
DISCOVERY_MIN_PERIOD     = 50   ms       (flood guard)
DISCOVERY_PRUNE_GRACE    = 1    s        (added to ttl before evicting)

-- Link v1 (measurement)
LINK_MAX_MSG             = 512  bytes
LINK_PING_INTERVAL       = 50   ms
LINK_MAX_PINGS           = 5    per measurement
LINK_SAMPLE_THRESHOLD    = 100  data points (strictly greater)
SESSION_EPS              = 500_000 us (500 ms tie-break window)
SESSION_REMEASURE        = 30   s

-- Link-Audio v1
AUDIO_MAX_MSG            = 1200 bytes (excluding IP+UDP headers)
AUDIO_HDR_SIZE_RESERVED  = 24   bytes (protocol header + message header reserve)
AUDIO_MAX_PAYLOAD        = 1176 bytes (1200 - 24)
AUDIO_NON_AUDIO_BYTES    = 50   bytes (AudioBuffer overhead, conservative)
AUDIO_MAX_SAMPLE_BYTES   = 1126 bytes (AUDIO_MAX_PAYLOAD - AUDIO_NON_AUDIO_BYTES)
AUDIO_RFC791_SAFE_BYTES  = 502  bytes (Encoder.hpp; 576 - 24 - 50)
AUDIO_TTL                = 5    s  (PeerAnnouncement)
AUDIO_BCAST_PERIOD       = 250  ms
AUDIO_MIN_PERIOD         = 50   ms
AUDIO_REQUEST_TTL        = 5    s  (ChannelRequest refresh)
AUDIO_CODEC_PCM_I16      = 1
AUDIO_METRICS_WINDOW     = 10   samples
AUDIOBUFFER_TTL          = 0    (always, in MessageHeader.ttl)
```

---

## 11. Implementation Notes for FPGA / LiteX

This section is **informational**.

### 11.1 Hardware suitability

The protocol is unusually friendly to gate-level implementation:

- **No fragmentation on read.** Every frame is ≤1200 B; a single BRAM
  buffer per RX queue suffices. Discovery frames fit in 512 B.
- **No integer wider than 64 bits** appears in any TLV value. Every
  arithmetic operation needed for parsing (byte-swap, length check) can
  be done in a single cycle on a 64-bit datapath, or trivially over
  multiple cycles on an 8/16/32-bit datapath.
- **No floating-point on the wire.** Tempo is `i64` microseconds-per-beat,
  Beats is `i64` micro-beats, time is `i64` microseconds. A soft-FP unit
  is optional; all beat/time conversions can be expressed as
  multiply-and-round on `i64`. The only floating-point value in the
  reference is the `slope` of `GhostXForm`, which is local state and
  may legitimately be held at a hard-coded 1.0 (drift is absorbed by
  remeasurement every 30 s).
- **No cryptography.** No TLS, no signatures, no key exchange. The
  protocol is designed for trusted LANs.
- **Parser is a state machine over a stream of `(u32 key, u32 size,
  bytes)` records.** Forward compatibility falls out naturally from
  "skip unknown keys".

### 11.2 Suggested LiteX decomposition

A minimal hardware Link peer can be split into:

1. **Ethernet / UDP MAC** — LiteEth can already do this; filter on UDP
   port 20808 and optionally on the multicast address.
2. **Protocol demux** — 8-byte comparator against the three protocol
   headers; tag packets as `DISCOVERY`, `LINK`, or `AUDIO`.
3. **Discovery parser** — reads MessageHeader, then walks the TLV
   stream, populating registers for `tmln`, `sess`, `stst`, `mep4`,
   `mep6` of the sending peer. A small peer table (CAM keyed on NodeId,
   with TTL counters driven by a millisecond tick) handles liveness.
4. **Ghost-time unit** — a 64-bit add/subtract unit implementing
   `hostToGhost`/`ghostToHost` with a hard-wired `slope = 1` and a
   software-loaded `intercept`.
5. **Measurement engine** — either a tiny soft CPU (VexRiscv / PicoRV32)
   running the median filter in firmware, or a direct HW implementation:
   sort-accumulate 100 × i64 samples with a Batcher sorter.
6. **Audio datapath (optional)** — DMA from a local codec into the
   accumulator, `kPCM_i16` byte-swap on write to the TX FIFO, beat
   tagging via the ghost-time unit.

### 11.3 Recommended firmware / gateware split

Keep the tight real-time paths (packet parse, ghost-time arithmetic,
audio DMA) in gateware. Put the following in a tiny soft CPU:

- Session election (§6.4) — runs at most every 30 s.
- Median computation on the 100-sample buffer.
- Heartbeat scheduling and peer table pruning.
- Start/stop state management.

The soft CPU needs neither dynamic allocation nor an RTOS. A flat
cooperative loop driven by a 1 kHz timer is sufficient.

### 11.4 Interoperability checklist

A reimplementation SHOULD be tested against the reference library with
at least the following scenarios:

1. Two peers started in sequence — new peer joins older session;
   tempo/beat origin match after one measurement cycle.
2. Cold start of three peers within <1 s — lowest-NodeId peer should
   "win" the tie-break (§6.4).
3. Tempo changes broadcast mid-session — other peers pick up the change
   within one heartbeat.
4. Orderly shutdown — BYEBYE evicts immediately; no stale peer for 5 s.
5. IPv4-only and IPv6-only subnets, plus dual-stack.
6. A peer with multiple interfaces (the reference treats them as
   independent gateways).
7. For Link-Audio: a mono 48 kHz channel and a stereo 44.1 kHz channel
   streaming simultaneously to the same receiver, with channel
   subscribe/unsubscribe cycling.

---

## Appendix A — 4CC Registry

All 4CCs are transmitted in network byte order, so the byte sequence on
the wire is the ASCII string read left-to-right.

| 4CC     | Hex (u32)    | Defined in              | Used in                               | Value type |
|---------|--------------|-------------------------|---------------------------------------|------------|
| `tmln`  | `0x746D6C6E` | Timeline.hpp            | Discovery v1 ALIVE/RESPONSE body      | tuple(Tempo, Beats, micros) = 24 B |
| `sess`  | `0x73657373` | SessionId.hpp           | Discovery v1, Link v1 PONG, Link-Audio v1 PeerAnnouncement | NodeId = 8 B |
| `stst`  | `0x73747374` | StartStopState.hpp      | Discovery v1                          | tuple(bool, Beats, micros) = 17 B |
| `mep4`  | `0x6D657034` | PeerState.hpp           | Discovery v1                          | (u32 v4, u16 port) = 6 B |
| `mep6`  | `0x6D657036` | PeerState.hpp           | Discovery v1                          | (u8[16] v6, u16 port) = 18 B |
| `aep4`  | `0x61657034` | PeerState.hpp           | Discovery v1                          | (u32 v4, u16 port) = 6 B |
| `aep6`  | `0x61657036` | PeerState.hpp           | Discovery v1                          | (u8[16] v6, u16 port) = 18 B |
| `__ht`  | `0x5F5F6874` | PayloadEntries.hpp      | Link v1 PING, Link-Audio Ping/Pong    | i64 microseconds = 8 B |
| `__gt`  | `0x5F5F6774` | PayloadEntries.hpp      | Link v1 PONG                          | i64 microseconds = 8 B |
| `_pgt`  | `0x5F706774` | PayloadEntries.hpp      | Link v1 PING/PONG                     | i64 microseconds = 8 B |
| `__pi`  | `0x5F5F7069` | link_audio/PeerInfo.hpp | Link-Audio v1 PeerAnnouncement        | string |
| `auca`  | `0x61756361` | link_audio/ChannelAnnouncements.hpp | Link-Audio v1 PeerAnnouncement | vector<ChannelAnnouncement> |
| `aucb`  | `0x61756362` | link_audio/ChannelAnnouncements.hpp | Link-Audio v1 ChannelByes      | vector<ChannelBye> |
| `chid`  | `0x63686964` | link_audio/ChannelId.hpp| Link-Audio v1 ChannelRequest / StopRequest | NodeId = 8 B |
| `_abu`  | `0x5F616275` | link_audio/AudioBuffer.hpp | (reserved; not currently used as a TLV key — AudioBuffer is sent without TLV framing) | — |

Every `key == 0xXX...` assertion in the table above is cross-checked by
a `static_assert` in the referenced header. Reimplementations can rely
on these exact values.

---

## Appendix B — Annotated wire examples

### B.1 Minimal Discovery v1 ALIVE (IPv4-only, no audio)

Assume NodeId `AA BB CC DD EE FF 00 11`, SessionId identical (the peer
just founded its own session), Timeline `(120 BPM, beat 0, t_origin 0)`,
StartStopState `(false, 0, 0)`, measurement endpoint `192.168.1.42:40001`.

Wire bytes (grouped for readability):

```
-- Protocol header
5F 61 73 64 70 5F 76 01

-- Message header
01                           -- messageType = ALIVE
05                           -- ttl = 5
00 00                        -- groupId
AA BB CC DD EE FF 00 11      -- ident (NodeId)

-- tmln entry
74 6D 6C 6E                  -- key 'tmln'
00 00 00 18                  -- size = 24
00 00 00 00 00 07 A1 20      -- tempo: micros/beat = 500_000 (120 BPM)
00 00 00 00 00 00 00 00      -- beatOrigin (micro-beats)
00 00 00 00 00 00 00 00      -- timeOrigin (us, ghost time)

-- sess entry
73 65 73 73                  -- key 'sess'
00 00 00 08                  -- size = 8
AA BB CC DD EE FF 00 11      -- sessionId

-- stst entry
73 74 73 74                  -- key 'stst'
00 00 00 11                  -- size = 17
00                           -- isPlaying = false
00 00 00 00 00 00 00 00      -- beats
00 00 00 00 00 00 00 00      -- timestamp

-- mep4 entry
6D 65 70 34                  -- key 'mep4'
00 00 00 06                  -- size = 6
C0 A8 01 2A                  -- 192.168.1.42
9C 41                        -- port 40001
```

Total: 8 + 12 + (8+24) + (8+8) + (8+17) + (8+6) = **107 bytes**.

### B.2 Link v1 PING (first round)

```
-- Protocol header
5F 6C 69 6E 6B 5F 76 01

-- Message header (1 byte!)
01                           -- messageType = PING

-- __ht entry
5F 5F 68 74                  -- key '__ht'
00 00 00 08                  -- size = 8
00 00 01 8B D8 4C A0 00      -- hostTime microseconds (example)
```

Total: 8 + 1 + 8 + 8 = **25 bytes**.

### B.3 Link v1 PONG (first round)

The responder appends the received PING payload verbatim; if the PING
had a `_pgt` entry, it is echoed too.

```
-- Protocol header
5F 6C 69 6E 6B 5F 76 01

-- Message header
02                           -- messageType = PONG

-- sess entry
73 65 73 73 00 00 00 08      -- key + size
<8 bytes SessionId>

-- __gt entry
5F 5F 67 74 00 00 00 08      -- key + size
<8 bytes ghostTime>

-- echoed __ht entry
5F 5F 68 74 00 00 00 08      -- key + size
<8 bytes echoed hostTime>
```

Total: 8 + 1 + (8+8) + (8+8) + (8+8) = **57 bytes**.

### B.4 Link-Audio v1 AudioBuffer (single chunk, stereo 48 kHz, 64 frames)

```
-- Protocol header
63 68 6E 6E 6C 73 76 01

-- Message header
06                           -- messageType = kAudioBuffer
00                           -- ttl = 0
00 00                        -- groupId
<8 bytes sender NodeId>

-- AudioBuffer (raw, NOT TLV-wrapped)
<8 bytes channelId>
<8 bytes sessionId>
00 00 00 01                  -- chunkCount = 1

  -- Chunk[0]
  00 00 00 00 00 00 00 2A    -- count = 42
  00 40                      -- numFrames = 64
  00 00 00 00 00 00 00 00    -- beginBeats (micro-beats)
  00 00 00 00 00 07 A1 20    -- tempo micros/beat (500_000 = 120 BPM)

01                           -- codec = kPCM_i16
00 00 BB 80                  -- sampleRate = 48000
02                           -- numChannels = 2
01 00                        -- numBytes = 64 * 2ch * 2B = 256
<256 bytes of big-endian i16 PCM samples>
```

Total: 8 + 12 + 16 + 4 + 26 + (1 + 4 + 1 + 2 + 256) = **330 bytes**.

---

## Appendix C — Mapping to reference source

The structural claims in this document can be cross-checked against
the following files in the reference [Ableton/link](https://github.com/Ableton/link)
source tree (paths relative to that repository's root).

| Section | Files |
|---------|-------|
| §3 primitives | `include/ableton/discovery/NetworkByteStreamSerializable.hpp` |
| §3 TLV | `include/ableton/discovery/Payload.hpp` |
| §4 transport | `include/ableton/discovery/IpInterface.hpp`, `UnicastIpInterface.hpp`, `platforms/darwin/Darwin.hpp`, `platforms/linux/Linux.hpp` |
| §5 Discovery v1 framing | `include/ableton/discovery/v1/Messages.hpp`, `UdpMessenger.hpp`, `PeerGateway.hpp` |
| §5.2 payload types | `include/ableton/link/Timeline.hpp`, `Tempo.hpp`, `Beats.hpp`, `SessionId.hpp`, `StartStopState.hpp`, `PeerState.hpp`, `NodeState.hpp`, `EndpointV4.hpp`, `EndpointV6.hpp` |
| §6 session | `include/ableton/link/Sessions.hpp`, `SessionController.hpp`, `Peers.hpp` |
| §7 measurement | `include/ableton/link/Measurement.hpp`, `MeasurementService.hpp`, `PingResponder.hpp`, `Median.hpp`, `link/v1/Messages.hpp`, `PayloadEntries.hpp` |
| §9 Link-Audio | `include/ableton/link_audio/**`, especially `v1/Messages.hpp`, `UdpMessenger.hpp`, `PeerAnnouncement.hpp`, `ChannelAnnouncements.hpp`, `ChannelRequests.hpp`, `ChannelId.hpp`, `AudioBuffer.hpp`, `PCMCodec.hpp`, `Encoder.hpp`, `Resizer.hpp`, `NetworkMetrics.hpp` |

When in doubt, the reference source is authoritative over this
document. Bug reports against the spec welcome.
