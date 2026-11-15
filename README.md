# ull — low-latency market-maker research baseline

**Replay-only, not production trading software.** The goal is sub-microsecond
market-data-to-order latency on appropriate colocated hardware, measured at the
wire. This repository does not yet implement an exchange connection or establish
that latency. No binary here can send an exchange order.

## First deployment target: Nasdaq, with venue-local execution

The selected first venue is **Nasdaq US equities in Carteret, NJ**, using
**TotalView-ITCH 5.0 / MoldUDP64** for direct market data and **OUCH 5.0 /
SoupBinTCP** for native order entry. Begin with a small licensed replay universe
of liquid Nasdaq-listed stocks; do not assume a consolidated US book or globally
unique symbols. Nasdaq stock-locate IDs are assigned per day, not permanent IDs.

The FPGA reference platform is **AMD Alveo X3522PV** (the programmable variant,
not the X3522 NIC), with a 10 GbE initial lab path, Vivado RTL and a Linux/C++20
control host. Board supply, production lifecycle, MAC/PCS and TCP/session IP,
tool licensing, optics and Nasdaq handoff compatibility must be confirmed before
purchase. This is a design selection, not a procurement or access commitment.

Target **p99.9 < 1,000 ns from trigger-frame RX end-of-frame to first order-frame
TX start-of-frame** at calibrated wire reference points, under a declared traffic
envelope. Also report RX start-of-frame to TX start/end-of-frame, packet lengths,
clock uncertainty and rejects/drops. The latter first-bit interval includes input
serialization and is not promised below 1,000 ns for all packet sizes. Exchange
matching/acknowledgement time is outside this local target. No target is measured
or achieved by the current prototype.

Global expansion means **independent venue-local execution**, not a WAN-dependent
order path. Each site owns a disjoint allocated exposure budget, locally enforces
it and stops new exposure if its authorization expires. Budget transfer must fence
the former owner and reconcile resting/uncertain exposure before reuse; a central
service being unreachable never grants extra capacity. Global reporting combines
timestamped native-currency P&L and FX marks asynchronously. Protocol-specific
decoders/encoders stay under venue namespaces; do not force every exchange through
a universal hot-path object model. These distributed controls are planned, not
implemented in the single-instrument demo.

Sources: [Nasdaq ITCH specification](https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHSpecification.pdf),
[OUCH 5.0 specification](https://www.nasdaqtrader.com/content/technicalsupport/specifications/TradingProducts/OUCH5.0.pdf),
[MoldUDP64 specification](https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/moldudp64.pdf),
[AMD X3522PV product page](https://www.amd.com/en/products/accelerators/alveo/x3/a-x3522pv-p08g-pq-g.html).

## Run locally

Requires Linux, GCC with C++20, Make, Python 3.10+, and Icarus Verilog for RTL tests.
The C++ binaries have no third-party runtime dependencies. The compiler barriers
in the measurement harness use GCC/Clang syntax.

```sh
make -j2 all test rtl-test sanitize
./build/benchmark
./build/replay

uv venv .venv
uv pip install --python .venv/bin/python -r monitoring/requirements.txt
make telemetry-test
./build/replay | .venv/bin/python monitoring/export.py
```

The replay generates 100,000 normalized top-of-book updates. Every third update
gets synthetic executions on both sides; other orders receive immediate synthetic
terminal acknowledgements. This deliberately tests accounting, **not** queue
position, adverse selection, fill probability, exchange throughput or profits.
The final result is flat inventory and 433,329 tick-lots of synthetic P&L after
fees. A tick-lot is one price tick times one quantity lot; apply instrument tick
value and lot size before interpreting it as money.

### Nasdaq transport replay

`build/nasdaq_ingress SESSION10 NEXT_SEQUENCE udp-payload.bin [...]` reads one
complete MoldUDP64 UDP payload per file, using an explicitly provided trusted
session/checkpoint. It validates the entire datagram before exposing any messages,
suppresses duplicate/overlapping message prefixes, recognizes heartbeats/end of
session, and latches off on a gap, malformed packet or session mismatch. The
library does not allocate, copy payloads or use unaligned integer loads. Packet
views borrow their immutable input buffer; consumers must finish before reuse.

This layer does **not** decode ITCH, verify Ethernet/IP/UDP integrity, open a socket,
request retransmissions, recover a book, send orders or connect to the toy engine.
Mold sequence numbers count all messages in a channel, including other instruments
and administrative events; do not map them directly to `Engine::on_book`'s
single-instrument sequence. The next layer needs a separate per-book generation,
day/session-qualified instrument mapping and trading-state gates. Tests use
hand-written synthetic wire fixtures, not licensed exchange recordings.

The conservative receiver stops on the first forward gap rather than waiting on
a redundant channel. It does not automatically resume when late messages arrive.
An eventual recovery coordinator must rebuild/reconcile state before explicit
rearming. `transport_ready` is not permission to trade; downstream decode/book
failure must also disarm the path. Parse validation is not application rollback.

## What is implemented

- A single-owner C++20 engine with fixed storage, integer arithmetic, no hot-path
  heap allocation, I/O, mutexes or OpenTelemetry SDK calls. It joins the best bid
  and ask with a fixed quantity. It is a correctness baseline, not a calibrated
  market-making strategy.
- Speculative quote **preparation** with engine identity, book generation and risk
  epoch checks at commit. Any book, fill, terminal acknowledgement, commit or kill
  that makes a candidate obsolete prevents its reuse. Preparation does not yet
  encode or prebuild venue packets.
- Freshness and sequence gates, explicit snapshot recovery, independent worst-case
  buy/sell reservations, order notional and position limits, and a latching
  mark-to-market loss kill. Both sides must pass or the entire pair is rejected.
- Sixty-four fixed order slots, partial/cumulative fills, price improvement,
  duplicate-fill suppression, fees, cash, position and total marked P&L. Slots are
  selected by monotonic order ID modulo capacity; a busy selected slot rejects a
  pair even if another slot is free. No unbounded slot search occurs.
- A bounded SPSC telemetry queue and separate consumer thread. Full queues drop
  snapshots rather than blocking the engine. JSON formatting and OTel export
  occur off the engine thread. A separate Python process uses the official SDK.
- A one-register-stage Verilog candidate gate with widened arithmetic, version
  tags, and packet/sequence/freshness/kill inputs. It has a simulation testbench.
  It is **not** the complete C++ engine in hardware: no reservations, accounting,
  sequence recovery, packet parser, MAC, session state, encoder or transmitter.

### Safety contracts

The C++ owner must serially process feed, executions and controls in a coherent
order. Input `Book` values must already come from validated packets with a local
monotonic receive timestamp. Incrementals must be contiguous; recovery requires a
validated, reconciled snapshot. The engine cannot validate Ethernet CRCs, exchange
sessions or packets it never sees.

A committed pair is an in-memory reservation and simulated handoff, not exchange
acceptance. A cancel request, disconnect, ambiguous send failure or timeout must
**not** release a reservation. `on_terminal` is only for definitive terminal
acknowledgements, after processing their cumulative fills. Execution adapters must
deduplicate and sequence reports, reconcile unknown IDs, handle rejects and trade
corrections, and detect missing reports. The demo assumes those adapter contracts;
it does not implement them.

Kill blocks new orders but does not cancel resting ones or reject valid fills.
There is no automatic kill reset. Live operation needs a cancellation path,
exchange cancel-on-disconnect, reconciliation and a supervised recovery procedure.
Do not reconstruct a fresh engine against an account that still has exposure.

P&L is cash plus position times the last valid midpoint, rounded down to a whole
tick, with fees already deducted. It is total marked P&L, not separate realized
and unrealized ledgers, executable liquidation value or a portfolio valuation.
Invalid feeds leave the last mark intact and set `feed_ready` false. That flag
means sequence/snapshot readiness, not a continuously ticking freshness watchdog;
age is checked at each commit. Monitoring snapshots are not a durable execution
journal and must not be used to reconstruct authoritative risk.

## Observability stays off the trading path

The replay samples one CPU interval every 256 updates, from entry of normalized
book processing through reservation. Two `steady_clock` calls instrument only
sampled updates. The sample excludes execution simulation and queue insertion.
The benchmark measures every update with telemetry disabled. Neither measures
market-data-wire to order-wire latency.

The exporter publishes cumulative book/pair/reject/drop counters, position, cash,
P&L, resting quantities, kill/feed status, replay time, and the
`ull.cpu_book_to_reservation` nanosecond histogram. No per-order labels or per-tick
spans are created. SDK clocks timestamp export observations; replay time is a
separate metric, not a fabricated wall-clock timestamp.

Accounting snapshots carry cumulative totals, so the next delivered snapshot
restores current totals after drops. Lost latency samples cannot be recovered;
sampled tails can be biased, particularly when the queue is full. Alert on
telemetry drops and missing data rather than treating a silent exporter as healthy.
The demo emits exact final totals after draining the consumer; shutdown can wait
on a blocked stdout, although the trading loop never waits for the exporter.

Console export is the default. To use a local OpenTelemetry Collector:

```sh
# Requires an installed otelcol binary; this repository does not deploy a backend.
otelcol --config monitoring/collector.yaml
# In another shell, on the same host:
./build/replay | .venv/bin/python monitoring/export.py --otlp
```

The SDK defaults to the local OTLP/HTTP endpoint. Configure
`OTEL_EXPORTER_OTLP_METRICS_ENDPOINT` and standard OTel TLS/auth variables for your
own collector. Keep credentials out of source control. The supplied collector
configuration listens on loopback and prints metrics; it is not a deployed
dashboard, alerting system or durable store. `make telemetry-test` uses a local
HTTP receiver and decodes actual OTLP protobuf to verify P&L and histogram values.

## A CPU nanosecond result is not a wire-to-wire result

`build/benchmark` reports 200,000 cache-warm intervals after 10,000 warmup updates,
with p50, p99, p99.9, max, a checksum and an empty clock-pair median. It includes
book validation, quote preparation, risk checks and reservation of two orders;
it excludes input decoding, output encoding, hardware, transport, telemetry and
cancel acknowledgements. Clock overhead is reported but not subtracted. A shared
VM's scheduler, CPU frequency and instrumentation affect these numbers. No timing
threshold is used as a correctness test.

For a meaningful sub-microsecond target, choose and report:

1. Venue, feed/order protocols, instruments, maximum packet sizes, line rate,
   NIC/FPGA board and whether the strategy needs the whole packet.
2. RX start-of-frame to TX start-of-frame, and RX end-of-frame to TX
   start-of-frame as separate metrics. Correlate the actual triggering feed
   message and outgoing order, not just adjacent packets. Include external tap
   or calibrated MAC/PHY measurements; a transmit API return is not wire egress.
3. Clock source/domain, timestamp resolution, calibration and uncertainty. Use a
   common clock where possible; subtracting unsynchronized RX/TX clocks is invalid.
4. Hardware p50/p99/p99.9/max under representative bursts, packet sizes, symbol
   mixes, fills, cancels and backpressure. Retain rejected and dropped-event counts.
   Repeat with telemetry enabled/disabled to measure its cost.

Even serialization imposes limits: 1,500 bytes take 1,200 ns at 10 Gb/s before
additional framing overhead. Waiting for full-frame integrity cannot meet a
sub-microsecond first-bit-to-first-bit budget for that frame size. Cut-through
parsing can prepare candidates earlier, but this baseline never authorizes an
irrevocable order before packet integrity and final risk validation. Hardware
abort semantics require a venue/transport-specific design and fault testing.

## Path to an actual FPGA market maker

For consistently sub-microsecond wire latency, aim to keep decoding, incremental
book updates, simple pricing, risk reservations and order encoding on the FPGA.
Use C++ for the reference model, configuration, replay, reconciliation and slower
strategy/control functions. A CPU/NIC kernel-bypass path is a separate engineering
option whose end-to-end latency must be measured, not inferred from this kernel.

The next milestone is **ITCH decoding and order-book reconstruction** against
licensed fixtures, including directory/session mapping, halts, executions,
cancels/replaces and explicit recovery. Follow with OUCH encoding and a session/
order state machine, reliable execution processing, cancel/rate/self-trade controls
and persistent reconciliation. Port validated behavior into a complete FPGA
pipeline with coherent risk ownership and a final transmit gate, then establish
hardware timing. Do not let CPU and FPGA independently spend the same risk budget.

The RTL candidate output has no ready/valid backpressure handshake: each valid
cycle is one advisory candidate that may be discarded. It must never connect
directly to a transmitter. A consumer must recheck current versions and risk,
reserve atomically, and handle downstream readiness. One registered stage says
nothing about achievable frequency: synthesis, place-and-route, timing closure,
CDC/reset analysis and hardware-in-loop fault tests remain undone. In particular,
variable notional multipliers may require pipelining on the chosen device.

References: [AMD low-latency CPU/NIC measurements](https://www.amd.com/content/dam/amd/en/documents/epyc-technical-docs/white-papers/amd-low-latency-solutions-amd-epyc-9005-amd-alveo-x3.pdf),
[OpenTelemetry Python exporters](https://opentelemetry.io/docs/languages/python/exporters/),
and [OpenTelemetry batch processor semantics](https://opentelemetry.io/docs/specs/otel/trace/sdk/).
Batch export alone does not remove SDK work from span creation, which is why the
trading engine never calls the SDK at all.
