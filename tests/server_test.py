#!/usr/bin/env python3
"""Real HTTP/1.1 tests against the built server; no handler mocks."""
import base64
import concurrent.futures
import http.client
import json
import os
from pathlib import Path
import selectors
import signal
import socket
import subprocess
import sys
import tempfile
import time
import unittest

EXECUTABLE = str(Path(sys.argv.pop(1)).resolve())
try:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
except PermissionError:
    print("SKIP: environment prohibits localhost TCP bind (Operation not permitted)", file=sys.stderr)
    sys.exit(77)


def b64(value):
    return base64.b64encode(value).decode("ascii")


class ServerTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="aegis-http-")
        self.process = None
        self.config = Path(self.directory.name) / "server.json"
        self.config.write_text(json.dumps({"snapshot_ttl_seconds": 300}))
        self.start()

    def start(self):
        self.process = subprocess.Popen(
            [EXECUTABLE, "--config", str(self.config), "--port", "0", "--db",
             str(Path(self.directory.name) / "db"), "--memtable-bytes", "4096",
             "--max-request-bytes", "1048576", "--timeout-ms", "1000"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        with selectors.DefaultSelector() as selector:
            selector.register(self.process.stdout, selectors.EVENT_READ)
            if not selector.select(15):
                self.fail("server did not become ready")
        line = self.process.stdout.readline().strip()
        if not line.startswith("AEGISDB_READY "):
            self.fail(f"server startup failed: {line} {self.process.stderr.read()}")
        self.port = int(line.split()[1])

    def stop(self, crash=False):
        if self.process and self.process.poll() is None:
            self.process.send_signal(signal.SIGKILL if crash else signal.SIGTERM)
            self.process.wait(timeout=20)
            if not crash:
                self.assertEqual(self.process.returncode, 0, self.process.stderr.read())
        if self.process:
            self.process.stdout.close()
            self.process.stderr.close()

    def tearDown(self):
        self.stop()
        self.directory.cleanup()

    def request(self, path, body=None, method="POST", raw=None):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=15)
        payload = raw if raw is not None else json.dumps(body or {})
        conn.request(method, path, payload, {"Content-Type": "application/json"})
        result = conn.getresponse()
        code, data = result.status, json.loads(result.read())
        conn.close()
        return code, data

    def put(self, key, value, sync=True):
        code, data = self.request("/v1/kv/put", {"key_base64": b64(key), "value_base64": b64(value), "sync": sync})
        self.assertEqual(code, 200, data)

    def get(self, key, snapshot=None):
        body = {"key_base64": b64(key)}
        if snapshot:
            body["snapshot_id"] = snapshot
        code, data = self.request("/v1/kv/get", body)
        self.assertEqual(code, 200, data)
        return base64.b64decode(data["value_base64"]) if data["found"] else None

    def maintain(self, name):
        code, operation = self.request("/v1/admin/" + name)
        self.assertEqual(code, 202, operation)
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            code, result = self.request("/v1/admin/operations/" + operation["operation_id"], method="GET")
            self.assertEqual(code, 200, result)
            if result["state"] in ("done", "failed"):
                self.assertEqual(result["state"], "done", result)
                return
            time.sleep(0.02)
        self.fail("maintenance did not finish")

    def test_binary_api_snapshots_pagination_and_restart(self):
        self.assertEqual(self.request("/v1/health", method="GET")[0], 200)
        self.put(b"\0\xff", b"\xff\0hello")
        self.put(b"a", b"A")
        code, snap = self.request("/v1/snapshots")
        self.assertEqual(code, 201)
        self.put(b"a", b"new")
        self.assertEqual(self.get(b"a", snap["snapshot_id"]), b"A")
        batch = {"operations": [{"type": "put", "key_base64": b64(b"b"), "value_base64": b64(b"B")},
                                 {"type": "delete", "key_base64": b64(b"a")},
                                 {"type": "put", "key_base64": b64(b"c"), "value_base64": b64(b"C")}]}
        self.assertEqual(self.request("/v1/kv/batch", batch)[0], 200)
        self.assertIsNone(self.get(b"a"))
        self.maintain("flush")
        self.maintain("compact")
        self.assertEqual(self.get(b"a", snap["snapshot_id"]), b"A")
        code, page = self.request("/v1/kv/scan", {"limit": 2})
        self.assertEqual(code, 200)
        self.assertEqual([base64.b64decode(e["key_base64"]) for e in page["entries"]], [b"\0\xff", b"b"])
        code, page2 = self.request("/v1/kv/scan", {"limit": 2, "after_key_base64": page["next_after_key_base64"]})
        self.assertEqual(code, 200)
        self.assertEqual(base64.b64decode(page2["entries"][0]["key_base64"]), b"c")
        self.assertEqual(self.request("/v1/snapshots/" + snap["snapshot_id"], method="DELETE")[0], 200)
        self.put(b"durable", b"before-kill")
        self.stop(crash=True)
        self.start()
        self.assertEqual(self.get(b"durable"), b"before-kill")
        self.assertEqual(self.get(b"\0\xff"), b"\xff\0hello")
        self.assertIsNone(self.get(b"a"))
        code, stats = self.request("/v1/stats", method="GET")
        self.assertEqual(code, 200)
        self.assertGreater(stats["last_sequence"], 0)

    def test_concurrent_clients_and_shutdown_under_load(self):
        def client(index):
            for n in range(15):
                key = f"client:{index}:{n}".encode()
                self.put(key, key, sync=False)
                self.assertEqual(self.get(key), key)
        with concurrent.futures.ThreadPoolExecutor(max_workers=12) as pool:
            list(pool.map(client, range(12)))
        self.maintain("flush")
        self.stop()
        self.start()
        for index in range(12):
            self.assertEqual(self.get(f"client:{index}:14".encode()), f"client:{index}:14".encode())
        def pending(index):
            try:
                self.request("/v1/kv/get", {"key_base64": b64(b"absent")})
            except (ConnectionError, OSError, http.client.HTTPException):
                pass
        with concurrent.futures.ThreadPoolExecutor(max_workers=12) as pool:
            tasks = [pool.submit(pending, n) for n in range(100)]
            self.stop()
            for task in tasks:
                task.result()

    def test_invalid_large_oversized_timeout_and_snapshot_expiry(self):
        for body in ({}, {"key_base64": "AB==", "value_base64": ""}, {"key_base64": "", "value_base64": "", "sync": "yes"}):
            self.assertEqual(self.request("/v1/kv/put", body)[0], 400)
        self.assertEqual(self.request("/v1/kv/put", raw="{")[0], 400)
        self.assertEqual(self.request("/v1/kv/scan", {"limit": 0})[0], 400)
        self.assertEqual(self.request("/v1/kv/batch", {"operations": [{"type": "put", "key_base64": b64(b"not-committed"), "value_base64": ""}, {"type": "bad", "key_base64": ""}]})[0], 400)
        self.assertIsNone(self.get(b"not-committed"))
        value = bytes(range(256)) * 2048
        self.put(b"large", value)
        self.assertEqual(self.get(b"large"), value)
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        conn.putrequest("POST", "/v1/kv/put")
        conn.putheader("Content-Type", "application/json")
        conn.putheader("Content-Length", "2097152")
        conn.endheaders()
        self.assertEqual(conn.getresponse().status, 413)
        conn.close()
        sock = socket.create_connection(("127.0.0.1", self.port), timeout=5)
        sock.sendall(b"POST /v1/kv/put HTTP/1.1\r\nHost: localhost\r\nContent-Length: 100\r\n\r\n")
        self.assertEqual(sock.recv(1), b"")
        sock.close()
        self.stop()
        self.config.write_text(json.dumps({"snapshot_ttl_seconds": 1}))
        self.start()
        code, snap = self.request("/v1/snapshots")
        self.assertEqual(code, 201)
        time.sleep(1.1)
        self.assertEqual(self.request("/v1/kv/get", {"key_base64": b64(b"large"), "snapshot_id": snap["snapshot_id"]})[0], 404)


if __name__ == "__main__":
    unittest.main()
