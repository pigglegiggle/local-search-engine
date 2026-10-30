#!/usr/bin/env python3
"""Measure a downloaded CPython/Lib corpus on macOS; keep raw observations."""
import gc
import hashlib
import json
import math
from pathlib import Path
import platform
import re
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from web._native import NativeIndex

OUT = ROOT / "build/performance-report"
CORPUS = OUT / "cpython/Lib"
BINARY = ROOT / "build/lse"
INDEX = OUT / "cpython.lsi"
assert CORPUS.is_dir(), "Clone CPython into build/performance-report/cpython first"
OUT.mkdir(parents=True, exist_ok=True)


def run(arguments, measured=False):
    command = list(map(str, arguments))
    start = time.perf_counter_ns()
    result = subprocess.run(
        (["/usr/bin/time", "-l"] if measured else []) + command,
        text=True, capture_output=True, check=True, timeout=600,
    )
    elapsed = (time.perf_counter_ns() - start) / 1e6
    observation = {"command": command, "wall_ms": elapsed, "stdout": result.stdout,
                   "stderr": result.stderr}
    if measured:
        match = re.search(r"(\d+)\s+maximum resident set size", result.stderr)
        assert match, "macOS time -l RSS field missing"
        observation["peak_rss_bytes"] = int(match[1])
    return observation


def quantiles(values):
    ordered = sorted(values)
    return {"p50": ordered[math.ceil(len(ordered) * .50) - 1],
            "p95": ordered[math.ceil(len(ordered) * .95) - 1]}


result = {"platform": platform.platform(), "date": time.strftime("%Y-%m-%d %H:%M:%S %z"),
          "cpython_commit": run(["git", "-C", CORPUS.parent, "rev-parse", "HEAD"])["stdout"].strip(),
          "binary_sha256": hashlib.sha256(BINARY.read_bytes()).hexdigest()}
base = [BINARY, "index", CORPUS, "--index", INDEX, "--threads", "4"]
result["rebuild"] = [run(base + ["--rebuild"], True) for _ in range(3)]
result["unchanged"] = [run(base, True) for _ in range(3)]
assert all("· changed 0 ·" in r["stdout"] for r in result["unchanged"])
result["stats"] = json.loads(run([BINARY, "stats", "--index", INDEX, "--json"])["stdout"])
result["verification"] = run([BINARY, "verify", "--index", INDEX])

# A known one-file change; always restore the downloaded source, even on failure.
target = CORPUS / "abc.py"
original = target.read_bytes()
marker = "lsebenchmarkuniquemarker"
try:
    target.write_bytes(original + f"\n# {marker}\n".encode())
    result["one_file_changed"] = run(base, True)
    assert "· changed 1 ·" in result["one_file_changed"]["stdout"]
    hit = json.loads(run([BINARY, "search", marker, "--index", INDEX, "--json"])["stdout"])
    assert hit["total"] == 1
finally:
    target.write_bytes(original)
    run(base)
restored = json.loads(run([BINARY, "search", marker, "--index", INDEX, "--json"])["stdout"])
assert restored["total"] == 0
result["change_roundtrip_verified"] = True

loads = []
for _ in range(5):
    native = NativeIndex(str(INDEX))
    loads.append(native.stats()["load_ms"])
    del native
    gc.collect()
result["native_load_ms"] = loads
native = NativeIndex(str(INDEX))
queries = ["memory", "thread", "memory allocation", "asyncio OR threading",
           '"event loop"', '"garbage collection"', "sched*", "memry~1",
           "(cache OR memory) NOT ext:txt", "path:asyncio thread",
           "def AND return", "lsebenchmarkabsenttoken"]
query_results = []
for query in queries:
    expected = native.search(query)
    engine, bridge = [], []
    for _ in range(30):
        begin = time.perf_counter_ns()
        found = native.search(query)
        bridge.append((time.perf_counter_ns() - begin) / 1e6)
        engine.append(found["search_ms"])
        assert found == expected or (
            found["total"] == expected["total"] and found["hits"] == expected["hits"]
        )
    query_results.append({"query": query, "matches": expected["total"],
                          "engine_ms": engine, "bridge_wall_ms": bridge,
                          "engine_quantiles_ms": quantiles(engine),
                          "bridge_quantiles_ms": quantiles(bridge),
                          "top_paths": [h["path"] for h in expected["hits"][:3]]})
result["queries"] = query_results
result["engine_pooled_ms"] = quantiles([v for q in query_results for v in q["engine_ms"]])
result["bridge_pooled_ms"] = quantiles([v for q in query_results for v in q["bridge_wall_ms"]])
del native
gc.collect()
result["fresh_cli"] = [run([BINARY, "search", "memory allocation", "--index", INDEX, "--json"], True)
                       for _ in range(5)]

# Separate one-large-file throughput test, not a ranking-quality benchmark.
large = OUT / "large-file"
large.mkdir(exist_ok=True)
text = "memory allocation cache locality scheduler latency kernel thread\n".encode()
payload = text * ((16 * 1024 * 1024 + len(text) - 1) // len(text))
(large / "large.txt").write_bytes(payload)
large_index = OUT / "large-file.lsi"
large_args = [BINARY, "index", large, "--index", large_index, "--rebuild", "--threads", "4"]
result["large_file"] = {
    "source_bytes": len(payload),
    "default_limit": run(large_args, True),
    "raised_limit": run(large_args + ["--max-file-mb", "32"], True),
}
assert "skipped 1" in result["large_file"]["default_limit"]["stdout"]
large_stats = json.loads(run([BINARY, "stats", "--index", large_index, "--json"])["stdout"])
assert large_stats["documents"] == 1 and large_stats["source_bytes"] == len(payload)
result["large_file"]["stats"] = large_stats
result["large_file"]["verify"] = run([BINARY, "verify", "--index", large_index])
destination = OUT / "real.json"
destination.write_text(json.dumps(result, indent=2) + "\n")
print(destination)
