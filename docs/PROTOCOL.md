# DME binary protocol v1

The Linux gateway uses a length-prefixed, little-endian TCP protocol. Frames may be
fragmented or coalesced by TCP; receivers must buffer until the declared length is
available and may then continue decoding the remaining bytes. Signed prices use
64-bit two's-complement representation. No C++ structure is copied directly onto
the wire.

## Common header

Every frame begins with this 24-byte header:

| Offset | Size | Field | Value |
|---:|---:|---|---|
| 0 | 4 | magic | bytes `44 4d 45 31` (`DME1`) |
| 4 | 1 | version | `1` |
| 5 | 1 | message type | see below |
| 6 | 2 | frame length | total bytes, including header |
| 8 | 4 | session ID | nonzero |
| 12 | 4 | reserved | zero on encode; ignored in v1 decode |
| 16 | 8 | client sequence | nonzero |

Message types are `1` new order, `2` cancel, `3` replace, and `128` execution
event. The first request binds a TCP connection to its session. A second live
connection cannot claim that session. Client sequence starts at 1 and advances by
one for each syntactically valid request, including a request rejected by risk.

## Requests

### New order (52 bytes)

| Offset | Size | Field |
|---:|---:|---|
| 24 | 1 | side: `0` buy, `1` sell |
| 25 | 1 | order type: `0` limit, `1` market, `2` IOC, `3` FOK |
| 26 | 2 | reserved |
| 28 | 8 | order ID, nonzero |
| 36 | 8 | price |
| 44 | 8 | quantity |

### Cancel (32 bytes)

| Offset | Size | Field |
|---:|---:|---|
| 24 | 8 | order ID, nonzero |

### Replace (48 bytes)

| Offset | Size | Field |
|---:|---:|---|
| 24 | 8 | order ID, nonzero |
| 32 | 8 | new price |
| 40 | 8 | new quantity |

Engine sequence numbers are deliberately absent from requests. The single-writer
gateway runner assigns them in accepted queue order.

## Execution event response (68 bytes)

One request may produce multiple event frames. Every event repeats the originating
session ID and client sequence for correlation. The engine sequence is zero for a
rejection produced by the gateway before the request reaches the matching core.

| Offset | Size | Field |
|---:|---:|---|
| 24 | 1 | event type |
| 25 | 1 | aggressor side |
| 26 | 1 | rejection reason |
| 27 | 1 | reserved |
| 28 | 8 | order ID |
| 36 | 8 | contra order ID |
| 44 | 8 | execution/order price |
| 52 | 8 | quantity |
| 60 | 8 | engine sequence |

Event types are `0` accepted, `1` rejected, `2` trade, `3` cancelled, `4`
replaced, and `5` rested. Rejection reasons are `0` none, `1` invalid quantity,
`2` invalid price, `3` duplicate order ID, `4` unknown order ID, `5` book
capacity, `6` would not fill, `7` engine sequence gap, `8` invalid session
sequence, `9` risk limit, and `10` gateway backpressure.

## Validation and connection behavior

Malformed magic, version, type, length, enum values, or zero identity fields close
the connection. Session-sequence, risk, and input-queue backpressure failures return
a rejected event when transmit capacity remains. Slow readers are disconnected if
their bounded 256 KiB transmit buffer cannot accept another event. The reference
limits are quantity 1,000,000, price 1,000,000,000, and notional
100,000,000,000 integer units.

The protocol provides framing and validation, not encryption or identity. Deploy it
only behind appropriate TLS, authentication, authorization, and traffic controls.
