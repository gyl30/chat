#!/usr/bin/env python3
"""Small evidence collector; callers schedule samples and own the test actions."""
import csv
import hashlib
import io
import json
import os
from pathlib import Path
import re
import subprocess
import time
from datetime import datetime, timezone


def utc_now():
    return datetime.now(timezone.utc).isoformat()


def process_sample(pid):
    """Read one Linux process without inspecting its command line or environment."""
    try:
        root = Path("/proc") / str(int(pid))
        stat = (root / "stat").read_text()
        fields = stat[stat.rfind(")") + 2:].split()
        status = dict(line.split(":", 1) for line in
                      (root / "status").read_text().splitlines() if ":" in line)
        kib = lambda key: int(status.get(key, "0 kB").split()[0])
        return {"pid": int(pid), "alive": fields[0] not in {"Z", "X", "x"}, "state": fields[0],
                "executable": os.readlink(root / "exe"),
                "start_ticks": int(fields[19]),
                "cpu_seconds": (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK"),
                "rss_kib": kib("VmRSS"), "peak_rss_kib": kib("VmHWM"),
                "threads": int(status.get("Threads", "0")),
                "fds": len(list((root / "fd").iterdir()))}
    except (OSError, ValueError, IndexError) as error:
        return {"pid": int(pid), "alive": False, "error": type(error).__name__}


def read_only_sql(sql, timeout=15):
    """Allow one plain SELECT with a small safe-function set; no WITH or scripts."""
    statement = sql.strip().removesuffix(";").strip()
    if not re.match(r"(?is)^SELECT\b", statement):
        raise ValueError("Only one plain SELECT is supported")
    if ";" in statement or "--" in statement or "/*" in statement or '"' in statement or "$" in statement:
        raise ValueError("SQL scripts, comments and quoted identifiers are not supported")
    if re.search(r"(?i)\b(INSERT|UPDATE|DELETE|MERGE|DROP|ALTER|CREATE|COPY|CALL|DO|SET|RESET|INTO|FOR|LOCK|GRANT|REVOKE|TRUNCATE|VACUUM|ANALYZE)\b", statement):
        raise ValueError("SQL mutation or locking is not supported")
    functions = {"count", "sum", "min", "max", "avg", "coalesce", "nullif",
                 "length", "octet_length", "md5", "encode", "decode", "lower", "upper",
                 "current_database", "current_schema", "current_setting",
                 "json_agg", "json_build_object", "array_agg", "bool_and", "bool_or",
                 "abs", "round", "extract", "in", "exists", "any", "all", "filter", "over",
                 "select", "from", "where", "and", "or", "not", "having"}
    # Ignore ordinary string literals when identifying function names.
    without_strings = re.sub(r"'(?:''|[^'])*'", "''", statement)
    for name in re.findall(r"([a-zA-Z_][a-zA-Z_0-9.]*)\s*\(", without_strings):
        if name.lower() not in functions:
            raise ValueError("SQL function is outside the evidence-query allowlist")
    environment = os.environ.copy()
    environment["PGOPTIONS"] = environment.get("PGOPTIONS", "") + " -c default_transaction_read_only=on"
    completed = subprocess.run(
        ["psql", "-X", "-q", "--csv", "--tuples-only", "-v", "ON_ERROR_STOP=1"],
        input="BEGIN READ ONLY;\n" + statement + ";\nROLLBACK;\n",
        text=True, capture_output=True, timeout=timeout, env=environment)
    if completed.returncode:
        # libpq errors can contain connection strings; never relay them to evidence.
        raise RuntimeError("Read-only evidence query failed (psql exit %d)" % completed.returncode)
    return list(csv.reader(io.StringIO(completed.stdout)))


class Observer:
    def __init__(self, output_dir, processes=None):
        self.output_dir = Path(output_dir)
        self.output_dir.mkdir(parents=True, exist_ok=True)
        self.processes = dict(processes or {})
        self.started = time.monotonic()
        self.previous = {}

    def _write(self, filename, value):
        with (self.output_dir / filename).open("a", encoding="utf-8") as stream:
            stream.write(json.dumps(value, ensure_ascii=False) + "\n")
        return value

    def set_process(self, label, pid):
        self.processes[label] = int(pid)
        self.previous.pop(label, None)

    def remove_process(self, label):
        self.processes.pop(label, None)
        self.previous.pop(label, None)

    def sample(self, checkpoint=None):
        now = time.monotonic()
        values = {}
        for label, pid in self.processes.items():
            value = process_sample(pid)
            previous = self.previous.get(label)
            value["cpu_percent_interval"] = None
            value["pid_reused"] = False
            if value["alive"]:
                if previous:
                    timestamp, old = previous
                    same_process = old["pid"] == pid and old["start_ticks"] == value["start_ticks"]
                    value["pid_reused"] = not same_process
                    if same_process and now > timestamp:
                        value["cpu_percent_interval"] = round(
                            max(0, value["cpu_seconds"] - old["cpu_seconds"]) / (now - timestamp) * 100, 3)
                self.previous[label] = (now, value.copy())
            values[label] = value
        return self._write("resources.jsonl", {
            "utc": utc_now(), "elapsed_s": round(now - self.started, 3),
            "checkpoint": checkpoint, "processes": values})

    def record(self, case_id, expected, observed, passed, refs=None):
        if not isinstance(passed, bool):
            raise TypeError("passed must be an explicit bool, never an inferred status")
        return self._write("cases.jsonl", {
            "utc": utc_now(), "case_id": case_id, "expected": expected,
            "observed": observed, "status": "PASS" if passed else "FAIL",
            "refs": list(refs or [])})

    def capture_reference(self, case_id, path, description=""):
        artifact = Path(path)
        digest = hashlib.sha256()
        size = 0
        with artifact.open("rb") as stream:
            while block := stream.read(65536):
                digest.update(block)
                size += len(block)
        return self._write("captures.jsonl", {
            "utc": utc_now(), "case_id": case_id, "path": str(artifact.resolve()),
            "description": description, "bytes": size, "sha256": digest.hexdigest()})
