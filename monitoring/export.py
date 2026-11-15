"""Read replay JSON lines; export OTel metrics off the trading process.

Default: console. --otlp uses standard OTEL_EXPORTER_OTLP_* environment variables.
Snapshots are cumulative, so losing one never loses subsequent accounting totals.
"""

import argparse
import json
import sys
import threading

from opentelemetry.metrics import Observation
from opentelemetry.sdk.metrics import MeterProvider
from opentelemetry.sdk.metrics.export import ConsoleMetricExporter, PeriodicExportingMetricReader
from opentelemetry.sdk.resources import Resource


def run(stream, exporter):
    latest = {}
    lock = threading.Lock()
    reader = PeriodicExportingMetricReader(exporter, export_interval_millis=1000)
    provider = MeterProvider(
        resource=Resource.create({"service.name": "ull-replay", "trading.mode": "simulation"}),
        metric_readers=[reader],
    )
    meter = provider.get_meter("ull.monitoring")

    def callback(key):
        def observe(_options):
            with lock:
                value = latest.get(key)
            return [] if value is None else [Observation(value)]
        return observe

    for key in ("books", "committed_pairs", "rejects", "telemetry_dropped"):
        meter.create_observable_counter("ull." + key, callbacks=[callback(key)])
    for key in ("position", "cash", "pnl", "open_buy", "open_sell", "killed", "feed_ready"):
        unit = "{tick_lot}" if key in ("cash", "pnl") else "1"
        meter.create_observable_gauge("ull." + key, callbacks=[callback(key)], unit=unit)
    # Replay clock is NOT wall time. Export its value explicitly, never use it
    # as an OTel Unix timestamp or manufacture a wire-latency span from it.
    meter.create_observable_gauge("ull.replay_time", callbacks=[callback("sample_ns")], unit="ns")
    latency = meter.create_histogram(
        "ull.cpu_book_to_reservation", unit="ns",
        description="Sampled CPU interval, includes clock overhead; NOT wire-to-wire",
        explicit_bucket_boundaries_advisory=(25, 50, 100, 250, 500, 1000, 5000, 10000, 100000),
    )
    try:
        for line in stream:
            snapshot = json.loads(line)
            with lock:
                latest = snapshot
            if snapshot.get("cpu_latency_ns", -1) >= 0:
                latency.record(snapshot["cpu_latency_ns"])
        provider.force_flush()
    finally:
        provider.shutdown()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--otlp", action="store_true", help="export OTLP/HTTP protobuf instead of console")
    args = parser.parse_args()
    if args.otlp:
        from opentelemetry.exporter.otlp.proto.http.metric_exporter import OTLPMetricExporter
        selected = OTLPMetricExporter(timeout=2)
    else:
        selected = ConsoleMetricExporter()
    run(sys.stdin, selected)
