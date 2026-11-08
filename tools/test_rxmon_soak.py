"""Synthetic soak tests: serial controls, bounded capture, verdicts and lifecycle."""
import argparse
import base64
import http.server
import io
import json
import os
from pathlib import Path
import plistlib
import signal
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from contextlib import redirect_stderr, redirect_stdout
from unittest.mock import patch

sys.dont_write_bytecode = True
import rxmon
import rxmon_soak as soak

HERE = Path(__file__).resolve().parent


def state(t=100000, frames=1, rec=0):
    s = dict.fromkeys(soak.REQUIRED, 0)
    s.update(fw=soak.FW, uptime_ms=t, rx_only=True, mqtt_on=False,
        mqtt_connected=False, fan_id="29:000202", rf_state="receiving", rf_opmode=13,
        rf_stack_free=2300, heap_min=190000, rf_frames=frames, rf_recoveries=rec,
        rf_uart_pend_max=100, oled_available=True, oled_mode="auto", oled_on=False,
        rx_n=frames, mode="low", mode_age=100, fan_age=100, remote_confirmed=False)
    return s


def stat(t, frames, rec=0):
    return (f"[rxstat] t={t} uart=100 ovf=0 sync5=0 sync4={frames} frames={frames} resets=0 pend=0 "
            f"pendmax=100 q=0 qmax=1 drop=0 taskus=900 stack=2300 rxst=0 err=0 rec={rec} lost=0 "
            "op=0D lna=20 rxcfg=08 rxbw=01 rssi=-90 heap=200000 minheap=190000 loopmax=1000")


def good(t=100000):
    return {"utc": soak.stamp(), "wall": time.time(), "status": 200, "latency_ms": 1, "state": state(t)}


def bad():
    return {"utc": soak.stamp(), "wall": time.time(), "status": None, "latency_ms": 1, "error": "RuntimeError"}


class Pty:
    """Synthetic serial line: writes an [rxstat] every `period` s with rising t."""
    def __init__(self, period=0.05, extra=None):
        self.master, self.slave = os.openpty()
        self.name = os.ttyname(self.slave)
        self.done = threading.Event()
        self.extra = extra or {}
        self.thread = threading.Thread(target=self.feed, args=(period,))
        self.thread.start()

    def feed(self, period):
        n = 0
        while not self.done.wait(period):
            n += 1
            line = self.extra.get(n, stat(100000 + n * 100, 0))
            try:
                os.write(self.master, (line + "\r\n").encode())
            except OSError:
                return

    def close(self):
        self.done.set()
        self.thread.join()
        os.close(self.master)
        os.close(self.slave)


class Tests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name) / "capture"
        # Test dirs live in the OS temp dir; the real temp/disk guards are tested separately.
        for name, value in (("TEMP_ROOTS", ()), ("MIN_START_FREE", 0)):
            p = patch.object(soak, name, value)
            p.start()
            self.addCleanup(p.stop)
        self.args = argparse.Namespace(url="http://127.0.0.1/api/state", fan_id="29:000202",
            port="synthetic", seconds=86400, cap_mib=1, reserve_mib=1024)
        with redirect_stdout(io.StringIO()):
            soak.prepare(self.root, self.args)
        self.cfg = json.loads((self.root / "config.json").read_text())

    def tearDown(self):
        self.tmp.cleanup()

    def configure(self, **kw):
        self.cfg.update(kw)
        soak.save(self.root / "config.json", self.cfg)

    def result(self):
        return json.loads((self.root / "result.json").read_text())

    def reason(self):
        return json.loads((self.root / "run.json").read_text())["reason"]

    def test_prepare_does_not_start_and_records_provenance(self):
        self.assertFalse((self.root / "STARTED").exists())
        p = plistlib.loads((self.root / "agent.plist").read_bytes())
        self.assertEqual(p["ProgramArguments"][:2], ["/usr/bin/caffeinate", "-ims"])
        self.assertFalse(p["KeepAlive"])
        self.assertEqual(p["ProcessType"], "Standard")
        self.assertEqual(p["StandardErrorPath"], str(self.root / "collector.err"))
        self.assertEqual(self.root.stat().st_mode & 0o777, 0o700)
        tool = self.cfg["tool"]
        self.assertEqual(tool["sha256"], soak.tool_hashes())
        self.assertEqual(len(tool["git_head"]), 40)
        self.assertIn(tool["dirty"], (True, False))
        with self.assertRaises(SystemExit), patch.object(subprocess, "run") as submit, redirect_stderr(io.StringIO()):
            soak.main(["start", str(self.root)])
        submit.assert_not_called()
        for dirty, hashes in ((True, tool["sha256"]), (False, {"rxmon.py": "0"})):
            self.configure(tool={**tool, "dirty": dirty, "sha256": hashes})
            with self.assertRaises(SystemExit), patch.object(subprocess, "run") as submit, \
                 redirect_stderr(io.StringIO()):
                soak.main(["start", str(self.root), "--approved"])
            submit.assert_not_called()

    def test_prepare_refuses_temp_git_tree_and_low_disk(self):
        with patch.object(soak, "TEMP_ROOTS", ("/tmp", "/private/tmp")):
            with self.assertRaisesRegex(ValueError, "temp"):
                soak.prepare(Path("/private/tmp/soak-test-never-created"), self.args)
        self.assertFalse(Path("/private/tmp/soak-test-never-created").exists())
        with self.assertRaisesRegex(ValueError, "Git tree"):
            soak.prepare(soak.REPO / "logs" / "never-created", self.args)
        with patch.object(soak, "MIN_START_FREE", 10**20), self.assertRaisesRegex(ValueError, "10 GiB"):
            soak.prepare(Path(self.tmp.name) / "low", self.args)
        self.assertFalse((Path(self.tmp.name) / "low").exists())

    def test_serial_controls_set_before_open_without_reset_or_write(self):
        seen = []
        class Serial:
            def __setattr__(self, k, v):
                seen.append((k, v))
                object.__setattr__(self, k, v)
            def open(self):
                seen.append(("open", None))
        with patch("serial.Serial", Serial):
            rxmon.open_serial("synthetic")
        self.assertLess(seen.index(("dtr", True)), seen.index(("open", None)))
        self.assertLess(seen.index(("rts", True)), seen.index(("open", None)))
        self.assertNotIn(("dtr", False), seen)
        self.assertIn(("exclusive", True), seen)

    def test_budget_preserves_evidence_and_reserve(self):
        b = soak.Budget(self.root, 5, 0)
        b.write("serial.raw", b"12345")
        with self.assertRaisesRegex(RuntimeError, "cap"):
            b.write("serial.raw", b"6")
        self.assertEqual((self.root / "serial.raw").read_bytes(), b"12345")
        with self.assertRaisesRegex(RuntimeError, "reserve"):
            soak.Budget(self.root, 100, 10**20).write("other", b"x")

    def test_capped_stderr(self):
        out = io.StringIO()
        capped = soak.CappedText(out, 5)
        capped.write("abc")
        capped.write("defgh")
        self.assertEqual(out.getvalue(), "abcde")

    def test_invariants_cover_loss_rx_mqtt_oled_recovery_and_schema(self):
        self.assertEqual(soak.violations(state(), self.cfg), [])
        for k, v in (("mqtt_on", True), ("oled_on", True), ("tx_n", 1),
                     ("rf_opmode", 12), ("rf_queue_drops", 1), ("rf_lost", 1),
                     ("rf_stack_free", 1024), ("fan_id", "29:000303"),
                     ("fw", "wrong"), ("rf_opmode", "bad"), ("rf_state", "recovering")):
            s = state(); s[k] = v
            self.assertTrue(soak.violations(s, self.cfg), k)
        self.assertTrue(soak.violations({}, self.cfg))
        # recoveries before the capture are history; any new one during it is FAIL
        self.assertEqual(soak.violations(state(rec=3), self.cfg, state(rec=3)), [])
        self.assertTrue(soak.violations(state(rec=4), self.cfg, state(rec=3)))

    def test_curl_preserves_bytes_and_bounds_request(self):
        body = b' {"uptime_ms": 123}\n'
        result = subprocess.CompletedProcess([], 0, body+b"\n200", b"")
        with patch.object(subprocess, "run", return_value=result) as call:
            rec = soak.request_state(self.args.url)
        self.assertEqual(base64.b64decode(rec["body_b64"]), body)
        self.assertEqual(rec["state"]["uptime_ms"], 123)
        self.assertEqual(call.call_args.kwargs["timeout"], 5)
        self.assertIn("--max-filesize", call.call_args.args[0])
        self.assertIn("--max-time", call.call_args.args[0])
        self.assertNotIn("--location", call.call_args.args[0])
        for code, payload in ((200, b"broken"), (403, b"{}"), (301, b"{}")):
            result = subprocess.CompletedProcess([], 0, payload+f"\n{code}".encode(), b"")
            with patch.object(subprocess, "run", return_value=result):
                rec = soak.request_state(self.args.url)
            self.assertIn("error", rec)
            self.assertEqual(base64.b64decode(rec["body_b64"]), payload)
        result = subprocess.CompletedProcess([], 28, b"\n000", b"timeout")
        with patch.object(subprocess, "run", return_value=result):
            self.assertEqual(soak.request_state(self.args.url)["curl_exit"], 28)

    def test_failed_preflight_low_disk_or_changed_tool_never_opens_serial(self):
        for reason, answer in (("preflight state failed", {"error": "offline"}),
                               ("insufficient free disk", good()), ("tooling differs", good())):
            self.tearDown(); self.setUp()
            if reason == "tooling differs":
                self.configure(tool={**self.cfg["tool"], "sha256": {"rxmon.py": "0"}})
            low = 10**20 if reason == "insufficient free disk" else 0
            with patch.object(soak, "MIN_START_FREE", low), \
                 patch.object(soak, "request_state", return_value=answer), \
                 patch.object(rxmon, "open_serial") as serial_open:
                soak.run(self.root)
            serial_open.assert_not_called()
            self.assertIn(reason, self.reason())
            self.assertEqual(self.result()["verdict"], "FAIL")
            self.assertTrue((self.root / "FINISHED").exists())

    def fixture(self, outages=(), rec_at=None):
        """24 h synthetic capture; `outages` = (first sample, count) of failed GETs."""
        soak.save(self.root / "run.json", {"reason": "completed", "elapsed_seconds": 86400,
                                           "tool_verified": True, "end_wall": 86401})
        self.configure(tool={**self.cfg["tool"], "dirty": False})
        failed = {i for first, n in outages for i in range(first, first + n)}
        lines, records = [], []
        for i in range(17281):
            t = 100000 + i*5000
            if i in failed:
                records.append(json.dumps({"wall": i*5, "latency_ms": 3000, "status": None, "error": "RuntimeError"}))
            else:
                records.append(json.dumps({"wall": i*5, "latency_ms": 20, "status": 200,
                                           "state": state(t, 0 if i == 0 else 2)}))
            if i % 2 == 0:
                lines.append("synthetic " + stat(t, 0 if i == 0 else 2, 1 if rec_at is not None and i >= rec_at else 0))
            if i == 1:
                lines += ["synthetic [sync 1 104990 -60dBm pre=FF]",
                    "synthetic [rx 1 105000 -60dBm] I  --- 29:000202 --:------ 29:000202 31D9 003 000002",
                    "synthetic [sync 2 105090 -80dBm pre=D5]",
                    "synthetic [rx 2 105100 -80dBm] I  --- 29:000303 --:------ 29:000303 31D9 003 000003"]
        (self.root / "state.jsonl").write_text("\n".join(records) + "\n")
        (self.root / "serial.log").write_text("\n".join(lines) + "\n")

    def review(self):
        soak.save(self.root / "review.json", {k: {"passed": True, "evidence": "synthetic review"}
                                             for k in ("neighbor_isolation", "physical_oled")})

    def test_report_requires_complete_data_and_manual_evidence(self):
        self.fixture()
        r = soak.report(self.root)
        self.assertEqual(r["verdict"], "INCOMPLETE")
        self.assertEqual(r["checks"]["not_tested"], dict.fromkeys(soak.NOT_TESTED, "NOT_TESTED"))
        self.review()
        r = soak.report(self.root)
        self.assertEqual((r["verdict"], r["rf_verdict"], r["http_verdict"]), (soak.PASS_LABEL, "PASS", "PASS"))
        self.assertEqual(r["tool"]["sha256"], self.cfg["tool"]["sha256"])
        report_md = (self.root / "report.md").read_text()
        self.assertIn(soak.PASS_LABEL, report_md)
        for name in soak.NOT_TESTED:
            self.assertIn(f"| not_tested | {name} | NOT_TESTED |", report_md)
        with (self.root / "serial.log").open("a") as f:
            f.write("synthetic Task watchdog triggered\n")
        self.assertEqual(soak.report(self.root)["verdict"], "FAIL")

    def test_dirty_tooling_or_missing_serial_can_never_pass(self):
        self.fixture(); self.review()
        self.configure(tool={**self.cfg["tool"], "dirty": True})
        self.assertEqual(soak.report(self.root)["checks"]["run"]["tool_provenance"], "FAIL")
        self.fixture(); self.review()
        (self.root / "serial.log").unlink()
        r = soak.report(self.root)
        self.assertEqual((r["verdict"], r["rf_verdict"]), ("FAIL", "FAIL"))

    def test_http_outages_15s_5min_6h_are_windows_and_rf_stays_separate(self):
        self.fixture(outages=((100, 3), (1000, 60), (5000, 4320)))
        self.review()
        r = soak.report(self.root)
        w = r["details"]["http"]["outage_windows"]
        self.assertEqual([x["duration_s"] for x in w], [15, 300, 21600])
        self.assertFalse(any(x["open_at_end"] for x in w))
        self.assertEqual((r["rf_verdict"], r["http_verdict"], r["verdict"]), ("PASS", "FAIL", "FAIL"))
        self.assertEqual(r["checks"]["http"], {"http_available": "FAIL", "http_coverage": "FAIL"})
        # an outage that is still open at the end leaves the API invariants unproven
        self.fixture(outages=((17000, 281),))
        self.review()
        r = soak.report(self.root)
        self.assertTrue(r["details"]["http"]["outage_windows"][0]["open_at_end"])
        self.assertEqual(r["checks"]["rf"]["state_invariants"], "INCOMPLETE")
        self.assertEqual(r["rf_verdict"], "INCOMPLETE")

    def test_any_radio_recovery_is_fail_regardless_of_sampling(self):
        self.fixture(rec_at=9000)
        self.review()
        r = soak.report(self.root)
        self.assertEqual(r["checks"]["rf"]["radio_no_recovery"], "FAIL")
        self.assertEqual(r["verdict"], "FAIL")

    def test_short_stopped_or_http_gap_can_never_pass(self):
        self.fixture(); self.review()
        for meta in ({"reason": "completed", "elapsed_seconds": 35},
                     {"reason": "operator stop", "elapsed_seconds": 86400}):
            soak.save(self.root / "run.json", {**meta, "tool_verified": True, "end_wall": 86401})
            self.assertEqual(soak.report(self.root)["verdict"], "FAIL")
        soak.save(self.root / "run.json", {"reason": "completed", "elapsed_seconds": 86400,
                                           "tool_verified": True, "end_wall": 86401})
        with (self.root / "state.jsonl").open("a") as f:
            f.write(json.dumps({"wall": 86405, "error": "TimeoutError", "status": None})+"\n")
        self.assertEqual(soak.report(self.root)["checks"]["http"]["http_available"], "FAIL")

    def test_long_http_outage_and_recovery_never_stop_serial_capture(self):
        line = Pty()
        sizes, calls = [], []
        def flaky(url):
            calls.append(1)
            n = len(calls)
            if 2 <= n <= 41:                  # 40 consecutive failures, then recovery
                time.sleep(0.02)
                sizes.append((self.root / "serial.log").stat().st_size if (self.root / "serial.log").exists() else 0)
                return bad()
            return good(100000 + n)
        self.configure(port=line.name, seconds=3, interval=0.01)
        try:
            with patch.object(soak, "request_state", side_effect=flaky):
                soak.run(self.root)
        finally:
            line.close()
        self.assertEqual(self.reason(), "completed")
        self.assertGreater(len(calls), 45)                  # polling continued after recovery
        self.assertGreater(sizes[-1], sizes[0])             # serial.log grew during the outage
        w = self.result()["details"]["http"]["outage_windows"]
        self.assertEqual(len(w), 1)
        self.assertFalse(w[0]["open_at_end"])
        self.assertEqual(self.result()["checks"]["http"]["http_available"], "FAIL")
        for p in self.root.iterdir():
            self.assertEqual(p.stat().st_mode & 0o077, 0, p.name)

    def test_reset_marker_or_recovery_on_serial_stops_with_fail(self):
        for extra, reason in (({5: "ets Jun  8 2016 00:22:57"}, "reset"),
                              ({5: stat(100500, 0, rec=1)}, "recovery")):
            self.tearDown(); self.setUp()
            line = Pty(extra=extra)
            self.configure(port=line.name, seconds=30, interval=0.05)
            try:
                with patch.object(soak, "request_state", side_effect=lambda url: good()):
                    soak.run(self.root)
            finally:
                line.close()
            self.assertIn(reason, self.reason())
            self.assertEqual(self.result()["verdict"], "FAIL")

    def test_capture_raw_bytes_operator_stop_and_note_permissions(self):
        line = Pty(period=0.3)
        self.configure(port=line.name, seconds=30)
        threading.Timer(0.8, (self.root / "STOP").touch).start()
        try:
            with patch.object(soak, "request_state", side_effect=lambda url: good()):
                soak.run(self.root)
            self.assertIn(b"\r\n", (self.root / "serial.raw").read_bytes())
            self.assertEqual(self.reason(), "operator stop")
            self.assertEqual(self.result()["verdict"], "FAIL")
            with self.assertRaises(FileExistsError):
                soak.run(self.root)
        finally:
            line.close()
        old = os.umask(0o022)
        try:
            soak.main(["note", str(self.root), "synthetic observation"])
        finally:
            os.umask(old)
        self.assertEqual((self.root / "events.jsonl").stat().st_mode & 0o777, 0o600)

    def test_finalize_dead_collector_never_passes(self):
        self.fixture(); self.review()
        (self.root / "STARTED").write_text("x")
        soak.save(self.root / "run.json", {"reason": "running", "pid": 0})
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            soak.main(["report", str(self.root)])                     # refuses: points to finalize
        with redirect_stdout(io.StringIO()):
            soak.main(["finalize", str(self.root)])
        self.assertIn("finalized", self.reason())
        self.assertEqual(self.result()["verdict"], "FAIL")
        self.assertTrue((self.root / "FINISHED").exists())
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            soak.main(["finalize", str(self.root)])                   # only once

    def spawn_collector(self):
        """Real collector process: curl to a local HTTP server, pty as serial port."""
        body = json.dumps(state()).encode()
        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                self.send_response(200); self.end_headers(); self.wfile.write(body)
            def log_message(self, *a):
                pass
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        line = Pty()
        self.addCleanup(line.close)
        self.configure(port=line.name, seconds=60, url=f"http://127.0.0.1:{server.server_port}/api/state")
        err = open(self.root / "collector.err", "ab")
        self.addCleanup(err.close)
        # Same entry as launchd (main(["run", dir])); only the 10 GiB start guard is lowered for CI disks.
        code = ("import sys; sys.path.insert(0, sys.argv[1]); import rxmon_soak as s; "
                "s.MIN_START_FREE = 0; s.main(['run', sys.argv[-1]])")
        proc = subprocess.Popen([sys.executable, "-B", "-c", code, str(HERE), "soak", "run", str(self.root)],
                                stderr=err)
        self.addCleanup(lambda: proc.poll() is None and proc.kill())
        deadline = time.time() + 20
        while not (self.root / "status.json").exists() and proc.poll() is None and time.time() < deadline:
            time.sleep(0.1)
        self.assertTrue((self.root / "status.json").exists(), (self.root / "collector.err").read_text())
        return proc

    def test_sigkill_crash_then_finalize(self):
        proc = self.spawn_collector()
        self.assertTrue(soak.collector_alive(self.root))
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            soak.main(["finalize", str(self.root)])                   # refuses while alive
        proc.send_signal(signal.SIGKILL)
        proc.wait()
        self.assertEqual(self.reason(), "running")                     # no clean end was written
        with redirect_stdout(io.StringIO()):
            soak.main(["finalize", str(self.root)])
        self.assertEqual(self.result()["verdict"], "FAIL")
        self.assertEqual(self.result()["checks"]["run"]["completed_24h"], "FAIL")

    def test_sigterm_writes_fail_report(self):
        proc = self.spawn_collector()
        proc.send_signal(signal.SIGTERM)
        self.assertEqual(proc.wait(timeout=30), 0)
        self.assertEqual(self.reason(), "signal 15")
        self.assertEqual(self.result()["verdict"], "FAIL")
        self.assertTrue((self.root / "FINISHED").exists())


if __name__ == "__main__":
    unittest.main()
