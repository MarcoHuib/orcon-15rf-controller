"""Bounded RX/WiFi capture and conservative verdicts. Entry point: rxmon.py soak.

Only GET /api/state and serial reads touch the board. No firmware operations.
All hardware data, launchd definitions and reports belong in a private directory
outside the Git tree and outside temporary locations.
"""
import argparse
import base64
import collections
import datetime
import hashlib
import io
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
import urllib.parse

import rxmon

MIB = 1024 ** 2
GIB = 1024 ** 3
MIN_START_FREE = 10 * GIB            # never start a capture with less free disk than this
FW = "d5c39e1b4d1f0ed37defe69b51e3562c36058208"
HERE = Path(__file__).resolve().parent
REPO = HERE.parent
TOOLS = ("rxmon.py", "rxmon_soak.py")
# macOS empties these on restart or by age: evidence must never live there.
TEMP_ROOTS = ("/tmp", "/private/tmp", "/var/tmp", "/private/var/tmp",
              "/var/folders", "/private/var/folders")
PASS_LABEL = "PASS (passieve RX/WiFi-scope)"
NOT_TESTED = {
    "remote_presses_95pct_and_remote_confirmed": "geen geregistreerde afstandsbedieningsdrukken in deze passieve meting",
    "http_403_and_rejected_tx_commands": "geen commandoverzoeken tijdens deze meting",
    "mqtt_and_production_topics": "MQTT staat uit; geen broker of topics",
    "deliberate_network_disturbances": "geen bewuste broker-/AP-/DNS-/trage-clientverstoringen",
    "physical_oled_wake": "geen OLED-bediening tijdens deze meting",
    "rf_emission_without_sdr": "geen SDR/spectrumanalyzer; TX-afwezigheid alleen via tellers en logs",
}
ZERO = ("tx_n", "rf_uart_overflows", "rf_queue_drops", "rf_buffer_resets", "rf_lost")
REQUIRED = ZERO + ("fw", "uptime_ms", "rx_only", "mqtt_on", "mqtt_connected",
    "fan_id", "rf_state", "rf_opmode", "rf_stack_free", "heap_min", "rf_frames",
    "rf_err", "rf_recoveries", "rf_uart_pend_max", "oled_available", "oled_mode",
    "oled_on", "rx_n", "mode", "mode_age", "fan_age", "remote_confirmed")
BAD = re.compile(r"watchdog|Guru Meditation|panic|Backtrace:|state JSON .*skipped|"
                 r"emulator starting|^ets |^rst:0x|\[tx(?:\s|\])", re.I)


def stamp():
    return datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="milliseconds")


def utc(wall):
    return datetime.datetime.fromtimestamp(wall, datetime.timezone.utc).isoformat(timespec="seconds")


def read_lines(path):
    with path.open(encoding="utf-8", errors="replace") as source:
        yield from source


def save(path, value):
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(value, indent=2) + "\n")
    tmp.replace(path)


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(MIB), b""):
            digest.update(chunk)
    return digest.hexdigest()


def tool_hashes():
    return {name: sha256(HERE / name) for name in TOOLS}


def tool_info():
    def git(*args):
        return subprocess.run(["git", "-C", str(REPO), *args], capture_output=True,
                              text=True, check=True).stdout.strip()
    return {"git_head": git("rev-parse", "HEAD"),
            "dirty": bool(git("status", "--porcelain", "--", *("tools/" + n for n in TOOLS))),
            "sha256": tool_hashes(), "python": sys.version.split()[0]}


class CappedText(io.TextIOBase):
    """Collector stderr: keep the first `cap` characters, drop the rest."""
    def __init__(self, target, cap):
        self.target, self.left = target, cap

    def write(self, text):
        if self.left > 0:
            self.target.write(text[:self.left])
            self.target.flush()
            self.left -= len(text[:self.left])
        return len(text)


def request_state(url, timeout=3):
    """Use the system curl: works under macOS launchd and bounds total request time.

    Python urllib from this GUI LaunchAgent returns EHOSTUNREACH locally, while
    the system curl succeeds. No redirects/proxies, retries or board writes.
    """
    start = time.monotonic()
    rec = {"utc": stamp(), "wall": time.time(), "status": None}
    try:
        result = subprocess.run(["/usr/bin/curl", "--noproxy", "*", "--silent",
            "--show-error", "--proto", "=http", "--connect-timeout", str(timeout),
            "--max-time", str(timeout), "--max-filesize", "65536",
            "--header", "Cache-Control: no-store", "--write-out", "\n%{http_code}",
            url], capture_output=True, timeout=timeout+2)
        body, _, status = result.stdout.rpartition(b"\n")
        rec.update(status=int(status) if status.isdigit() else None,
                   body_b64=base64.b64encode(body).decode(), curl_exit=result.returncode)
        if result.returncode:
            raise RuntimeError("curl failed")
        if rec["status"] != 200 or len(body) > 65536:
            raise ValueError("unexpected status or response size")
        state = json.loads(body)
        if not isinstance(state, dict):
            raise ValueError("state is not an object")
        rec["state"] = state
    except Exception as exc:
        rec["error"] = type(exc).__name__
        reason = getattr(exc, "reason", exc)
        rec["error_reason_type"] = type(reason).__name__
        rec["error_errno"] = getattr(reason, "errno", None)
    rec["latency_ms"] = round((time.monotonic() - start) * 1000, 3)
    return rec


def violations(s, cfg, base=None):
    """Critical state rules. `base` is the first valid state of this capture."""
    missing = set(REQUIRED) - s.keys()
    if missing:
        return ["missing state fields: " + ", ".join(sorted(missing))]
    out = []
    for k in ZERO:
        if type(s[k]) is not int or s[k] != 0:
            out.append(k + " nonzero")
    for k, expected in (("fw", cfg["firmware"]), ("rx_only", True),
                        ("mqtt_on", False), ("mqtt_connected", False),
                        ("fan_id", cfg["fan_id"]), ("rf_state", "receiving"),
                        ("oled_available", True), ("oled_mode", "auto"), ("oled_on", False)):
        if s[k] != expected:
            out.append(k + " unexpected")
    numeric = ("rf_opmode", "rf_recoveries", "rf_stack_free", "rf_uart_pend_max",
               "heap_min", "uptime_ms", "rf_frames", "rx_n", "rf_err")
    if any(type(s[k]) is not int or s[k] < 0 for k in numeric):
        return out + ["invalid numeric telemetry"]
    if s["rf_opmode"] & 7 != 5:
        out.append("radio left RX")
    # Every recovery during the capture is FAIL, decided by the counter (not by sampling luck).
    if base is not None and s["rf_recoveries"] != base.get("rf_recoveries"):
        out.append("radio recovery during capture")
    if s["rf_stack_free"] <= 1024:
        out.append("RX stack <=1024 B")
    if s["rf_uart_pend_max"] >= 4096:
        out.append("UART pending >=4096 B")
    return out


class Budget:
    """Never rotate away evidence; stop before the cap or reserve is exhausted."""
    def __init__(self, root, cap, reserve):
        self.root, self.cap, self.reserve = root, cap, reserve
        self.used = 0
        self.lock = threading.Lock()

    def write(self, name, data):
        if isinstance(data, str):
            data = data.encode()
        with self.lock:
            if self.used + len(data) > self.cap:
                raise RuntimeError("log size cap reached")
            if shutil.disk_usage(self.root).free - len(data) < self.reserve:
                raise RuntimeError("disk reserve reached")
            with (self.root / name).open("ab") as f:
                f.write(data)
            self.used += len(data)

    def record(self, name, obj):
        self.write(name, json.dumps(obj, separators=(",", ":")) + "\n")


def finish(root, meta):
    """Write the end of run.json, then the report; a broken report still yields FAIL."""
    save(root / "run.json", meta)
    try:
        report(root)
    except Exception as exc:
        # Raw evidence is never overwritten; regenerate the report later.
        save(root / "result.json", {"verdict": "FAIL", "report_error": type(exc).__name__, "run": meta})
        (root / "report.md").write_text("# RX/WiFi duurtest: FAIL\n\nRapportagefout; bewaar de ruwe data en genereer het rapport opnieuw.\n")
    (root / "FINISHED").write_text(stamp() + "\n")


def run(root):
    os.umask(0o077)
    cfg = json.loads((root / "config.json").read_text())
    # Atomic claim prevents accidentally starting a directory twice.
    with (root / "STARTED").open("x") as f:
        f.write(stamp())
    budget = Budget(root, cfg["cap_mib"] * MIB, (cfg["reserve_mib"] + 16) * MIB)
    stop = threading.Event()
    errors = []
    start = time.monotonic()
    meta = {"start_utc": stamp(), "start_wall": time.time(), "pid": os.getpid(),
            "requested_seconds": cfg["seconds"], "reason": "running",
            "tool_verified": tool_hashes() == cfg.get("tool", {}).get("sha256")}
    save(root / "run.json", meta)

    def fail(exc):
        errors.append(str(exc))
        stop.set()

    def interrupted(signum, frame):
        fail("signal " + str(signum))

    for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(sig, interrupted)

    def serial_worker():
        try:
            with rxmon.open_serial(cfg["port"]) as serial_port:
                buf = b""
                last_stat = time.monotonic()
                previous_t = rec0 = None
                while not stop.is_set():
                    chunk = serial_port.read(4096)
                    if chunk:
                        budget.write("serial.raw", chunk)
                        buf += chunk
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        line = line.decode("utf-8", "replace").rstrip("\r")
                        budget.write("serial.log", stamp() + " " + line + "\n")
                        if BAD.search(line):
                            raise RuntimeError("serial reset/watchdog/TX/error marker")
                        if line.startswith("[rxstat] "):
                            st = rxmon.parse_stat(line)
                            if not st:
                                raise RuntimeError("damaged rxstat")
                            t = int(st["t"])
                            if previous_t is not None and t < previous_t:
                                raise RuntimeError("serial uptime decreased")
                            previous_t = t
                            last_stat = time.monotonic()
                            if any(int(st.get(k, -1)) != 0 for k in ("ovf", "drop", "resets", "lost")):
                                raise RuntimeError("serial loss counter nonzero")
                            if int(st["op"], 16) & 7 != 5 or int(st.get("rxst", -1)) != 0:
                                raise RuntimeError("serial radio left RX")
                            rec = int(st.get("rec", -1))
                            rec0 = rec if rec0 is None else rec0
                            if rec < 0 or rec != rec0:
                                raise RuntimeError("serial radio recovery during capture")
                    if len(buf) > 16384:
                        raise RuntimeError("serial line exceeds 16 KiB")
                    if time.monotonic() - last_stat > 25:
                        raise RuntimeError("no rxstat for 25 seconds")
        except Exception as exc:
            fail(exc)

    def http_worker(base):
        previous = base
        failures, outage_since = 0, None
        try:
            while not stop.is_set():
                tick = time.monotonic()
                rec = request_state(cfg["url"])
                budget.record("state.jsonl", rec)
                if "error" in rec:
                    # HTTP loss only affects the HTTP verdict; serial capture never waits or stops.
                    failures += 1
                    outage_since = outage_since or rec["utc"]
                else:
                    failures, outage_since = 0, None
                    s = rec["state"]
                    problems = violations(s, cfg, base)
                    if problems:
                        raise RuntimeError("; ".join(problems))
                    if s["uptime_ms"] < previous["uptime_ms"]:
                        raise RuntimeError("HTTP uptime decreased")
                    previous = s
                save(root / "status.json", {"utc": stamp(), "pid": os.getpid(),
                    "elapsed_seconds": round(time.monotonic() - start, 1),
                    "bytes_logged": budget.used, "last_http_status": rec["status"],
                    "last_http_error": rec.get("error"), "latency_ms": rec["latency_ms"],
                    "http_consecutive_failures": failures, "http_outage_since": outage_since})
                stop.wait(max(0, cfg["interval"] - (time.monotonic() - tick)))
        except Exception as exc:
            fail(exc)

    workers = []
    try:
        if not meta["tool_verified"]:
            raise RuntimeError("tooling differs from prepare")
        need = max((cfg["cap_mib"] + cfg["reserve_mib"] + 16) * MIB, MIN_START_FREE)
        if shutil.disk_usage(root).free < need:
            raise RuntimeError("insufficient free disk at start")
        if (root / "STOP").exists():
            raise RuntimeError("stop requested before start")
        # A preflight GET also prevents opening serial on a wrong/unsafe target.
        rec = request_state(cfg["url"])
        budget.record("state.jsonl", rec)
        if "error" in rec or violations(rec.get("state", {}), cfg):
            raise RuntimeError("preflight state failed")
        workers = [threading.Thread(target=serial_worker),
                   threading.Thread(target=http_worker, args=(rec["state"],))]
        for worker in workers:
            worker.start()
        while not stop.wait(0.2):
            if (root / "STOP").exists():
                fail("operator stop")
            elif time.monotonic() - start >= cfg["seconds"]:
                stop.set()
    except Exception as exc:
        fail(exc)
    finally:
        stop.set()
        for worker in workers:
            worker.join(timeout=10)
        meta.update(end_utc=stamp(), end_wall=time.time(),
                    elapsed_seconds=time.monotonic() - start,
                    reason="; ".join(errors) if errors else "completed", bytes_logged=budget.used)
        finish(root, meta)


def grade(ok, missing=False):
    return "FAIL" if not ok else "INCOMPLETE" if missing else "PASS"


def combine(values):
    values = list(values)
    return "FAIL" if "FAIL" in values else "INCOMPLETE" if "INCOMPLETE" in values or not values else "PASS"


def outage_windows(records, end_wall):
    """Consecutive failed GETs -> one window from the first failure to the next good answer."""
    windows, begin = [], None
    for r in records:
        good = "state" in r and "error" not in r
        if not good and begin is None:
            begin = r["wall"]
        elif good and begin is not None:
            windows.append({"start_utc": utc(begin), "end_utc": utc(r["wall"]),
                            "duration_s": round(r["wall"] - begin, 3), "open_at_end": False})
            begin = None
    if begin is not None:
        windows.append({"start_utc": utc(begin), "end_utc": utc(end_wall) if end_wall else None,
                        "duration_s": round(end_wall - begin, 3) if end_wall else None,
                        "open_at_end": True})
    return windows


def median(values):
    values = sorted(values)
    return values[len(values)//2] if values else None


def report(root):
    """Fail closed: missing data/manual evidence can never produce PASS."""
    cfg = json.loads((root / "config.json").read_text())
    meta = json.loads((root / "run.json").read_text())
    end_wall = meta.get("end_wall")
    tool = cfg.get("tool", {})
    run_c, rf, http, review_c, details = {}, {}, {}, {}, {}

    run_c["completed_24h"] = grade(meta.get("reason") == "completed" and
        cfg["seconds"] >= 86400 and (meta.get("elapsed_seconds") or 0) >= 86400)
    run_c["tool_provenance"] = grade(tool.get("dirty") is False and meta.get("tool_verified") is True)
    serial_path = root / "serial.log"
    boots = rxmon.from_file(serial_path) if serial_path.exists() else []
    stats = [s for b in boots for s in b["stats"]]
    frames = [r for b in boots for r in b["rx"]]
    serial_times = [int(s["t"]) for s in stats]
    run_c["serial_coverage"] = grade(len(serial_times) >= 2 and
        serial_times[-1] - serial_times[0] >= (cfg["seconds"] - 25) * 1000 and
        all(0 <= b - a <= 25000 for a, b in zip(serial_times, serial_times[1:])))
    run_c["serial_data_integrity"] = grade(bool(stats) and not any(b["bad"] for b in boots) and
        all(len(b["stats"]) >= 2 and "MISMATCH" not in rxmon.check_counters(b) for b in boots))
    details["counter_checks"] = [rxmon.check_counters(b) for b in boots]

    serial_bad = any(BAD.search(line.partition(" ")[2]) for line in read_lines(serial_path)) if boots else True
    rf["no_restart_watchdog_tx_markers"] = grade(len(boots) == 1 and not serial_bad)
    rf["serial_rx_and_no_loss"] = grade(bool(stats) and all(
        all(int(s.get(k, -1)) == 0 for k in ("ovf", "drop", "resets", "lost", "rxst")) and
        int(s["op"], 16) & 7 == 5 for s in stats))
    details["serial_stats"] = {k: {"first": stats[0].get(k), "last": stats[-1].get(k),
        "min": min(int(s[k]) for s in stats if k in s),
        "max": max(int(s[k]) for s in stats if k in s)}
        for k in ("uart", "frames", "ovf", "drop", "resets", "err", "rec", "lost",
                  "stack", "heap", "minheap", "pendmax", "qmax", "taskus", "loopmax")
        if stats and any(k in s for s in stats)}

    records, damaged = [], 0
    if (root / "state.jsonl").exists():
        for line in read_lines(root / "state.jsonl"):
            try:
                records.append(json.loads(line))
            except ValueError:
                damaged += 1
    good = [r for r in records if "state" in r and "error" not in r]
    states = [r["state"] for r in good if not (set(REQUIRED) - r["state"].keys())]
    # Counters (tx_n, losses, recoveries) in a sample near the end also cover HTTP gaps before it.
    near_end = bool(good) and end_wall is not None and good[-1]["wall"] >= end_wall - 60
    rf["state_invariants"] = grade(len(states) == len(good) and
        all(not violations(s, cfg, states[0]) for s in states), missing=len(states) < 2 or not near_end)
    rf["uptime_continuous"] = grade(all(b["uptime_ms"] >= a["uptime_ms"] for a, b in zip(states, states[1:])),
                                    missing=len(states) < 2)
    recs = {s.get("rec") for s in stats}
    rf["radio_no_recovery"] = grade(bool(stats) and None not in recs and len(recs) == 1 and
                                    len({s["rf_recoveries"] for s in states}) <= 1)

    heap = [(int(s["t"]), int(s["heap"])) for s in stats if "heap" in s]
    minheap = [int(s["minheap"]) for s in stats if "minheap" in s]
    if heap and minheap:
        first = median(h for t, h in heap if t <= heap[0][0] + 3600000)
        last = median(h for t, h in heap if t >= heap[-1][0] - 3600000)
        drop = minheap[0] - minheap[-1]
        rf["memory"] = grade(drop <= 10240 and first - last <= 10240 and
                             all(int(s.get("stack", 0)) > 1024 for s in stats))
        details["memory"] = {"minheap_drop_B": drop, "first_hour_heap_median_B": first,
            "last_hour_heap_median_B": last,
            "http_heap_min_first_last_B": [states[0]["heap_min"], states[-1]["heap_min"]] if states else None}
    else:
        rf["memory"] = "FAIL"

    own, neighbor = 0, 0
    for _, _, _, frame in frames:
        fields = frame.split()
        if fields[0] in ("I", "RP") and fields[5] in ("31D9", "31DA"):
            if fields[2] == cfg["fan_id"]:
                own += 1
            else:
                neighbor += 1
    details["rx"] = {"frames": len(frames), "selected_status": own, "neighbor_status": neighbor,
                     "commands": dict(collections.Counter(r[3].split()[5] for r in frames))}
    rf["rx_status_observed"] = grade(own > 0 and len(stats) >= 2 and
                                     int(stats[-1]["frames"]) > int(stats[0]["frames"]))

    windows = outage_windows(records, end_wall)
    http["http_available"] = grade(len(records) >= 2 and len(good) == len(records) and damaged == 0)
    gaps = [b["wall"] - a["wall"] for a, b in zip(good, good[1:])]
    coverage = good[-1]["wall"] - good[0]["wall"] if len(good) >= 2 else 0
    http["http_coverage"] = grade(coverage >= cfg["seconds"] - 2 * cfg["interval"] and
        bool(gaps) and max(gaps) <= 2 * cfg["interval"] + 3 and min(gaps) >= 0)
    latencies = sorted(r["latency_ms"] for r in good)
    details["http"] = {"requests": len(records), "failures": len(records) - len(good),
        "damaged_records": damaged, "coverage_seconds": coverage,
        "p95_ms": latencies[int((len(latencies)-1)*.95)] if latencies else None,
        "max_ms": max(latencies) if latencies else None,
        "outage_count": len(windows),
        "longest_outage_s": max((w["duration_s"] or 0 for w in windows), default=0),
        "outage_windows": windows[:1000]}   # complete list is derivable from state.jsonl

    # This signoff belongs ONLY in the private capture directory, after reviewing
    # the raw timeline and physical OLED observations. Never infer it from no traffic.
    review_path = root / "review.json"
    if review_path.exists() and review_path.stat().st_size > MIB:
        raise ValueError("review exceeds 1 MiB")
    review = json.loads(review_path.read_text()) if review_path.exists() else {}
    for name, observed in (("neighbor_isolation", neighbor > 0), ("physical_oled", True)):
        entry = review.get(name, {})
        review_c[name] = ("PASS" if observed and entry.get("passed") is True and entry.get("evidence")
                          else "FAIL" if entry.get("passed") is False else "INCOMPLETE")
    details["review"] = review
    details["radio_recoveries"] = states[-1].get("rf_recoveries") if states else None
    details["radio_errors"] = states[-1].get("rf_err") if states else None
    details["wifi_serial_events"] = []
    if serial_path.exists():
        for line in read_lines(serial_path):
            if re.search(r"wifi|\[net\]", line, re.I) and len(details["wifi_serial_events"]) < 100:
                # Keep report size bounded; complete messages remain in serial.log.
                details["wifi_serial_events"].append(line.rstrip()[:512])
    err = root / "collector.err"
    if err.exists():
        details["collector_stderr"] = {"bytes": err.stat().st_size,
                                       "head": err.read_bytes()[:2048].decode("utf-8", "replace")}
    hashes = {name: sha256(root / name) for name in
              ("serial.raw", "serial.log", "state.jsonl", "events.jsonl", "review.json", "collector.err")
              if (root / name).exists()}

    rf_verdict = combine([*run_c.values(), *rf.values(), review_c["neighbor_isolation"]])
    http_verdict = combine([*run_c.values(), *http.values()])
    overall = combine([*run_c.values(), *rf.values(), *http.values(), *review_c.values()])
    checks = {"run": run_c, "rf": rf, "http": http, "review": review_c,
              "not_tested": dict.fromkeys(NOT_TESTED, "NOT_TESTED")}
    result = {"verdict": PASS_LABEL if overall == "PASS" else overall,
              "rf_verdict": rf_verdict, "http_verdict": http_verdict, "generated_utc": stamp(),
              "tool": {**tool, "verified_at_start": meta.get("tool_verified")}, "run": meta,
              "checks": checks, "not_tested_reasons": NOT_TESTED, "details": details, "sha256": hashes}
    save(root / "result.json", result)

    lines = ["# RX/WiFi duurtest: " + result["verdict"], "",
             f"- RF-firmware (seriële RX, radio, geheugen, buren): **{rf_verdict}**",
             f"- WiFi/HTTP-beschikbaarheid: **{http_verdict}**",
             "- Stopreden: " + str(meta.get("reason", "unknown")),
             f"- Tooling: git `{tool.get('git_head')}`, gewijzigd={tool.get('dirty')}, "
             f"hashes gecontroleerd bij start={meta.get('tool_verified')}", "",
             "| Groep | Criterium | Resultaat |", "|---|---|---|"]
    for group, values in checks.items():
        lines += [f"| {group} | {k} | {v} |" for k, v in values.items()]
    lines += ["", "NOT_TESTED telt nooit als geslaagd; deze criteria vallen buiten deze passieve meting:"]
    lines += [f"- `{k}`: {v}" for k, v in NOT_TESTED.items()]
    lines += ["", f"## HTTP-uitval: {len(windows)} venster(s), langste {details['http']['longest_outage_s']} s", ""]
    lines += [f"- {w['start_utc']} → {w['end_utc'] or 'open'} ({w['duration_s']} s)"
              + (" — nog open bij einde" if w["open_at_end"] else "") for w in windows[:50]]
    lines += ["", "Seriële logging loopt onafhankelijk van HTTP door. WiFi-bereikbaarheid wordt",
              "afgeleid uit HTTP en seriële netwerkmeldingen; de firmware exposeert geen",
              "afzonderlijke WiFi-linkstatus/RSSI. OLED-telemetrie bewijst geen fysiek",
              "schermgedrag. Zonder buurverkeer is isolatie niet bewezen (INCOMPLETE).",
              "", "Details, tellerbereiken, HTTP-latenties en hashes: `result.json`.",
              "\n## Meetwaarden\n", "```json", json.dumps(details, indent=2), "```"]
    (root / "report.md").write_text("\n".join(lines) + "\n")
    return result


def collector_alive(root):
    out = subprocess.run(["ps", "-axo", "command="], capture_output=True, text=True, check=True).stdout
    return any("soak run" in line and line.rstrip().endswith(str(root)) for line in out.splitlines())


def prepare(root, args):
    os.umask(0o077)
    parsed = urllib.parse.urlsplit(args.url)
    if parsed.scheme != "http" or parsed.path != "/api/state" or parsed.username or parsed.query or parsed.fragment:
        raise ValueError("use an http://host/api/state URL without credentials/query")
    if args.seconds < 30 or args.seconds > 86400:
        raise ValueError("duration must be 30..86400 seconds")
    if not 1 <= args.cap_mib <= 512 or args.reserve_mib < 1024:
        raise ValueError("cap must be 1..512 MiB; reserve must be >=1024 MiB")
    real = root.resolve()
    if any(str(real) == t or str(real).startswith(t + "/") for t in TEMP_ROOTS):
        raise ValueError("use a persistent private directory such as ~/OrconSoak/<run-id>, not a temp location")
    if real == REPO or REPO in real.parents:
        raise ValueError("the run directory must be outside the Git tree")
    existing = next(p for p in (real, *real.parents) if p.exists())
    if shutil.disk_usage(existing).free < max((args.cap_mib + args.reserve_mib + 16) * MIB, MIN_START_FREE):
        raise ValueError("insufficient free disk (need >=10 GiB and cap + reserve + report allowance)")
    tool = tool_info()
    root.mkdir(parents=True, exist_ok=False, mode=0o700)
    cfg = {"url": args.url, "port": args.port, "fan_id": args.fan_id, "firmware": FW,
           "seconds": args.seconds, "interval": 5, "cap_mib": args.cap_mib,
           "reserve_mib": args.reserve_mib, "prepared_utc": stamp(), "tool": tool,
           "label": "local.orcon.rxmon." + str(time.time_ns())}
    save(root / "config.json", cfg)
    (root / "collector.err").touch()          # created 0600 before launchd opens it
    args_run = [sys.executable, "-B", str(HERE / "rxmon.py"), "soak", "run", str(root)]
    plist = {"Label": cfg["label"], "ProgramArguments": ["/usr/bin/caffeinate", "-ims"] + args_run,
             "RunAtLoad": True, "KeepAlive": False, "WorkingDirectory": str(root),
             "StandardOutPath": "/dev/null", "StandardErrorPath": str(root / "collector.err"),
             "Umask": 0o077, "ProcessType": "Standard"}
    with (root / "agent.plist").open("wb") as f:
        plistlib.dump(plist, f)
    print("PREPARED (not started):", root, "git", tool["git_head"], "dirty" if tool["dirty"] else "clean")


def main(argv):
    os.umask(0o077)
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest="action", required=True)
    prep = sub.add_parser("prepare")
    prep.add_argument("directory", type=Path)
    prep.add_argument("--url", required=True)
    prep.add_argument("--fan-id", required=True)
    prep.add_argument("--port", default="/dev/cu.usbserial-0001")
    prep.add_argument("--seconds", type=int, default=86400)
    prep.add_argument("--cap-mib", type=int, default=512)
    prep.add_argument("--reserve-mib", type=int, default=1024)
    for name in ("start", "run", "status", "stop", "report", "unload", "finalize"):
        cmd = sub.add_parser(name)
        cmd.add_argument("directory", type=Path)
        if name == "start":
            cmd.add_argument("--approved", action="store_true", help="explicit user permission to start")
    note = sub.add_parser("note")
    note.add_argument("directory", type=Path)
    note.add_argument("text")
    args = p.parse_args(argv)
    root = args.directory.expanduser().absolute()
    if args.action == "prepare":
        prepare(root, args)
    elif args.action == "start":
        if not args.approved:
            p.error("start requires --approved AFTER explicit user approval")
        if (root / "STARTED").exists() or (root / "STOP").exists():
            p.error("directory already started/stopped; prepare a fresh directory")
        tool = json.loads((root / "config.json").read_text()).get("tool", {})
        if tool.get("dirty") is not False or tool.get("sha256") != tool_hashes():
            p.error("tooling must be committed and unchanged since prepare; prepare a fresh directory")
        if shutil.disk_usage(root).free < MIN_START_FREE:
            p.error("less than 10 GiB free: NO-GO")
        auto = subprocess.run(["/usr/bin/defaults", "read", "/Library/Preferences/com.apple.SoftwareUpdate",
                               "AutomaticallyInstallMacOSUpdates"], capture_output=True, text=True).stdout.strip()
        if auto == "1":
            p.error("macOS installs updates automatically and may restart during the test: "
                    "turn off 'Install macOS updates' first")
        subprocess.run(["/bin/launchctl", "bootstrap", f"gui/{os.getuid()}", str(root / "agent.plist")], check=True)
        print("Submitted to launchd; check status.json and launchctl print.")
    elif args.action == "run":
        sys.stderr = CappedText(sys.stderr, 65536)
        run(root)
    elif args.action == "status":
        for name in ("status.json", "run.json", "result.json"):
            if (root / name).exists():
                value = json.loads((root / name).read_text())
                if name == "result.json":
                    value = {k: value.get(k) for k in ("verdict", "rf_verdict", "http_verdict", "checks")}
                print(name, json.dumps(value))
        if not (root / "STARTED").exists():
            print("PREPARED, not started")
    elif args.action == "stop":
        (root / "STOP").touch()
        print("Stop requested; wait up to 30 s for run.json end_utc and report.md.")
    elif args.action == "note":
        path = root / "events.jsonl"
        data = json.dumps({"utc": stamp(), "wall": time.time(), "note": args.text}) + "\n"
        if len(data.encode()) > 2048 or (path.stat().st_size if path.exists() else 0) > MIB:
            p.error("observation too large (2 KiB/note, 1 MiB/file)")
        with os.fdopen(os.open(path, os.O_WRONLY | os.O_APPEND | os.O_CREAT, 0o600), "a") as f:
            f.write(data)
    elif args.action == "finalize":
        if not (root / "STARTED").exists():
            p.error("this directory was never started")
        meta = json.loads((root / "run.json").read_text()) if (root / "run.json").exists() else {}
        if "end_utc" in meta:
            p.error("already finished; use soak report")
        if collector_alive(root):
            p.error("collector is still running; use soak stop")
        meta.update(end_utc=stamp(), end_wall=time.time(), finalized_utc=stamp(),
                    reason="collector died or was killed (finalized afterwards)")
        finish(root, meta)
        print(json.loads((root / "result.json").read_text())["verdict"])
    elif args.action == "unload":
        cfg = json.loads((root / "config.json").read_text())
        meta = json.loads((root / "run.json").read_text())
        if "end_utc" not in meta or not (root / "FINISHED").exists():
            p.error("request stop (or finalize a dead collector) and wait for the report before unloading")
        subprocess.run(["/bin/launchctl", "bootout", f"gui/{os.getuid()}/{cfg['label']}"], check=True)
    else:
        meta = json.loads((root / "run.json").read_text())
        if "end_utc" not in meta:
            p.error("capture still running, or the collector died: use soak finalize")
        print(report(root)["verdict"])
