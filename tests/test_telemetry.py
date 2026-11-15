import http.server
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import unittest

from opentelemetry.proto.collector.metrics.v1.metrics_service_pb2 import ExportMetricsServiceRequest

ROOT = Path(__file__).resolve().parents[1]


class TelemetryTest(unittest.TestCase):
    def test_replay_to_otlp_protobuf(self):
        # Local protocol receiver, not a deployed collector/backend.
        received = []

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                request = ExportMetricsServiceRequest()
                request.ParseFromString(self.rfile.read(int(self.headers["Content-Length"])))
                received.append((self.path, request))
                self.send_response(200)
                self.send_header("Content-Type", "application/x-protobuf")
                self.send_header("Content-Length", "0")
                self.end_headers()

            def log_message(self, *_args):
                pass

        with http.server.HTTPServer(("127.0.0.1", 0), Handler) as server:
            thread = threading.Thread(target=server.serve_forever)
            thread.start()
            try:
                replay = subprocess.run([str(ROOT / "build/replay")], check=True, capture_output=True, text=True)
                final = json.loads(replay.stdout.splitlines()[-1])
                self.assertEqual(final["books"], 100000)
                self.assertEqual(final["committed_pairs"], 100000)
                self.assertEqual(final["position"], 0)
                self.assertEqual(final["open_buy"], 0)
                self.assertEqual(final["open_sell"], 0)
                self.assertEqual(final["rejects"], 0)
                # 33,333 round trips * (3 ticks * 5 lots - 2 fee tick-lots).
                self.assertEqual(final["pnl"], 433329)
                self.assertEqual(final["cash"], 433329)
                env = {k: v for k, v in os.environ.items() if not k.startswith("OTEL_")}
                env["OTEL_EXPORTER_OTLP_METRICS_ENDPOINT"] = f"http://127.0.0.1:{server.server_port}/v1/metrics"
                # Lose intermediate accounting snapshots but retain two distinct
                # latency samples: distinguish sum/count from last-value gauges.
                first = dict(final, cpu_latency_ns=37)
                second = dict(final, cpu_latency_ns=211)
                subprocess.run([sys.executable, str(ROOT / "monitoring/export.py"), "--otlp"],
                               input="\n".join(map(json.dumps, (first, second, final))) + "\n",
                               text=True, env=env, check=True,
                               capture_output=True, timeout=15)
            finally:
                server.shutdown()
                thread.join()
        self.assertTrue(received)
        values = {}
        for path, request in received:
            self.assertEqual(path, "/v1/metrics")
            for resource in request.resource_metrics:
                attrs = {a.key: a.value.string_value for a in resource.resource.attributes}
                self.assertEqual(attrs["trading.mode"], "simulation")
                for scope in resource.scope_metrics:
                    for metric in scope.metrics:
                        data = getattr(metric, metric.WhichOneof("data"))
                        point = data.data_points[-1]
                        if metric.name == "ull.cpu_book_to_reservation":
                            values[metric.name] = (point.count, point.sum)
                        else:
                            values[metric.name] = point.as_int
        self.assertEqual(values["ull.pnl"], 433329)
        self.assertEqual(values["ull.position"], 0)
        self.assertEqual(values["ull.committed_pairs"], 100000)
        self.assertEqual(values["ull.killed"], 0)
        self.assertEqual(values["ull.cpu_book_to_reservation"], (2, 248))


if __name__ == "__main__":
    unittest.main()
