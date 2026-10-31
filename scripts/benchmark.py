#!/usr/bin/env python3
"""Reproducible synthetic benchmark; results are measurements, not hiring claims."""

import argparse
import json
import pathlib
import platform
import random
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument("--binary", type=pathlib.Path, default=pathlib.Path("build/lse"))
parser.add_argument("--documents", type=int, default=3000)
parser.add_argument("--repetitions", type=int, default=15)
parser.add_argument("--output", type=pathlib.Path, default=pathlib.Path("build/benchmark.json"))
args = parser.parse_args()
if not 1 <= args.documents <= 100000:
    parser.error("documents must be in [1, 100000]")
binary = args.binary.resolve()
runner = binary.parent / "benchmark_runner"
if not runner.exists():
    raise SystemExit("Build the harness first: make build/benchmark_runner")


def run(command):
    return subprocess.run(
        list(map(str, command)), check=True, text=True, capture_output=True, timeout=300
    )


with tempfile.TemporaryDirectory(prefix="lse-benchmark-") as temp:
    directory = pathlib.Path(temp)
    corpus = directory / "corpus"
    corpus.mkdir()
    rng = random.Random(20261030)
    vocabulary = [f"term_{i:04d}" for i in range(1000)]
    for i in range(args.documents):
        words = rng.choices(vocabulary, k=180)
        if i % 2 == 0:
            words[10:12] = ["memory", "allocation"]
        if i % 7 == 0:
            words[30:32] = ["cache", "locality"]
        if i % 17 == 0:
            words[50:52] = ["scheduler", "latency"]
        if i % 43 == 0:
            words[70:72] = ["kernel", "thread"]
        (corpus / f"doc_{i:06d}.txt").write_text(" ".join(words) + "\n")
    index = directory / "corpus.lsi"
    begin = time.perf_counter()
    run([binary, "index", corpus, "--index", index])
    build_ms = (time.perf_counter() - begin) * 1000
    begin = time.perf_counter()
    unchanged = run([binary, "index", corpus, "--index", index])
    unchanged_ms = (time.perf_counter() - begin) * 1000
    stats = json.loads(run([binary, "stats", "--json", "--index", index]).stdout)
    measurements = json.loads(run([runner, index, args.repetitions]).stdout)
    end_to_end = []
    for _ in range(5):
        begin = time.perf_counter()
        run([binary, "search", "memory allocation", "--json", "--index", index])
        end_to_end.append((time.perf_counter() - begin) * 1000)
    # Temporary paths differ each run and are irrelevant to reproducibility.
    stats.pop("root")
    result = {
        "seed": 20261030,
        "platform": platform.platform(),
        "machine": platform.machine(),
        "compiler": run(["clang++", "--version"]).stdout.splitlines()[0],
        "corpus": {
            "documents": args.documents,
            "words_per_document": 180,
            "random_vocabulary": 1000,
        },
        "build_end_to_end_ms": build_ms,
        "unchanged_reindex_end_to_end_ms": unchanged_ms,
        "unchanged_report": unchanged.stdout.splitlines()[1],
        "stats": stats,
        "search": measurements,
        "fresh_process_cli_ms": end_to_end,
        "methodology": [
            "Synthetic UTF-8 corpus; not representative of all real document distributions.",
            "Both methods operate on resident memory; tokenization and index loading excluded from query timings.",
            "Baseline scans pre-tokenized documents, computes BM25 with precomputed document frequencies, and sorts matches.",
            "Engine also parses the query, generates explanations internally, and extracts 10 snippets; baseline omits snippets.",
            "12 workloads: eight two-term AND queries and four phrases, including rare and missing terms; top-10 IDs, scores, and totals verified.",
            "Execution order alternates; one correctness/warm-up run precedes timing for each workload.",
            "Fresh-process CLI numbers include process startup, index load/validation, BK-tree construction, search, and JSON output; filesystem cache is not flushed.",
            "Unchanged reindex still reads source bytes, remaps postings, rebuilds the dictionary/BK-tree, and saves a new snapshot.",
        ],
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
