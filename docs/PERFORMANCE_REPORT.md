# Local Search Engine — Performance Report

**Measured on 31 October 2026 · Apple M2 · 16 GiB RAM**

This report records local measurements, including a resource-limited run. It separates resident search latency from full CLI execution and does not claim production scalability or human relevance quality.

## Results at a glance

- Synthetic corpus: **20,000 documents / 3.6 million tokens**; resident search p50 **0.388 ms**, p95 **2.158 ms**.
- Real corpus: **2,291 indexed CPython files**, 43.04 MiB source; resident C++ search p50 **1.937 ms**, p95 **2.943 ms**.
- Synthetic reference comparisons verified total matches, top-10 IDs/order and BM25 scores for all 12 workloads at each completed size.
- CPython semantic index verification passed; a one-file modification was indexed, found, restored and confirmed absent.
- A 50,000-file run could not finish creating its corpus because the disk filled; the engine was not reached. No result is claimed for that size.

## 1. Environment and artifacts

| Property | Recorded value |
|---|---|
| CPU | Apple M2; 8 logical CPUs |
| RAM | 16 GiB |
| OS | macOS-26.5.2-arm64-arm-64bit-Mach-O |
| Compiler | Apple clang version 21.0.0 (clang-2100.1.1.101) |
| CLI / C++ runner flags | `-std=c++17 -O2 -Wall -Wextra -Wpedantic -pthread` |
| Native bridge | Same C++ engine, Python 3.13 ABI; `-O2` |
| Concurrency | Sequential requests; no concurrent-user/load test |
| Cache / system | No filesystem cache flush; regular desktop background processes remained active |

Binary and source SHA-256 hashes are recorded in [environment.json](performance/2026-10-31/environment.json). At measurement time the project directory had no Git repository; hashes identify the measured local sources instead of a project commit.

The native bridge timer measures C++ retrieval; Python wall timing additionally includes calling the binding, conversion to Python objects and returning results. Neither includes HTTP or Streamlit rendering.

## 2. Synthetic scaling and scan baseline

Seed `20261030`; 180 tokens per document, 1,000 random identifiers plus 8 controlled terms. Eight two-term AND workloads and four exact phrases cover common, rare and missing terms. Each workload has one warm-up/correctness check and 30 timed repetitions per method: **360 samples per method per size**. Query execution order alternates between the engine and baseline. Percentiles below pool the 12 workloads and use nearest rank.

| Documents | Source MiB | Index MiB | First index wall ms | Unchanged wall ms | Index p50 / p95 ms | Scan p50 / p95 ms | p50 ratio |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 3,000 | 5.14 | 6.80 | 285.5 | 332.3 | 0.149 / 0.396 | 0.658 / 3.329 | 4.42× |
| 10,000 | 17.15 | 22.64 | 1102.9 | 1224.8 | 0.230 / 1.101 | 2.203 / 11.008 | 9.56× |
| 20,000 | 34.29 | 45.27 | 3020.3 | 2073.6 | 0.388 / 2.158 | 4.435 / 22.125 | 11.42× |

The ratio is **pooled scan p50 divided by pooled engine p50**, not an average per-query speedup. It applies only to this baseline, workload and machine. The baseline scans resident pre-tokenized documents, uses precomputed document frequencies, calculates BM25 and sorts matching results. It omits snippets and explanations; the engine parses the query and produces snippets/explanations. Index loading and baseline tokenization are excluded from resident timings. Missing terms can return immediately in both methods.

Initial indexing and unchanged reindexing each have **one observation per size**. They include process startup and saving; these are not statistically stable indexing medians. Synthetic process peak memory was not collected.

| Documents | Fresh CLI median ms (5 runs) | Stats-process index load ms (1 run) |
|---:|---:|---:|
| 3,000 | 75.62 | 67.76 |
| 10,000 | 224.23 | 204.07 |
| 20,000 | 437.00 | 400.96 |

Fresh CLI runs use `memory allocation`, include startup, load/validation, BK-tree preparation, retrieval and JSON output, and do not flush the OS cache. They are **fresh processes, not cold-storage tests**.

## 3. Real corpus: CPython/Lib

Source: [python/cpython](https://github.com/python/cpython/tree/4a9b98115fec4d93c32c554aa541d6c3fc4c3b27/Lib), pinned commit `4a9b98115fec4d93c32c554aa541d6c3fc4c3b27`. Only `Lib/` was indexed, using supported extensions and the normal UTF-8/binary filters. A sparse checkout retains this folder and top-level repository files. The downloaded source remains in `build/performance-report/cpython`.

Fifteen candidate files were skipped because they contain non-UTF-8 encoding fixtures, including CJK encoding samples and encoded Python modules. Their exact paths and warnings are retained in the raw observations. Other unsupported extensions are excluded from candidate selection.

| Property | Measured value |
|---|---:|
| documents | 2,291 |
| terms | 211,820 |
| tokens | 4,932,286 |
| postings | 800,290 |
| Indexed source | 43.04 MiB |
| Snapshot (includes document text) | 53.80 MiB |
| Snapshot / source | 1.25× |

### Indexing, memory and reload

Rebuild and unchanged runs use `--threads 4`; three sequential observations each. Peak RSS comes from macOS `/usr/bin/time -l`, whose RSS field is bytes. It is the **whole CLI process peak**, not index-only memory.

| Operation | Runs | Wall samples ms | Median wall ms | Maximum process peak RSS MiB |
|---|---:|---|---:|---:|
| Full rebuild | 3 | 14663.6, 14649.2, 14672.6 | 14663.6 | 660.1 |
| Unchanged reindex | 3 | 30581.9, 29368.4, 29158.0 | 29368.4 | 505.5 |
| One file changed | 1 | 30655.2 | 30655.2 | 475.6 |

First rebuild output:

```text
Indexed 2291 documents · 211820 terms · 4932286 tokens
Added 2291 · changed 0 · unchanged 0 · removed 0 · skipped 15
Build 14146.8942 ms (excludes old-index load and save)
Index: "/Users/omsinpt/local_search_engine/build/performance-report/cpython.lsi"
```

Native index load: median **13679.74 ms** across five loads (prior native objects released between samples). CLI fresh-search median: **13529.10 ms**, maximum process peak RSS **278.9 MiB** across five runs of `memory allocation`.

An unchanged reindex still loads/validates the old snapshot, reads source bytes, remaps/reuses postings, prepares the dictionary/BK-tree and writes a full new snapshot. It is not a constant-time no-op. The changed-file sample appends a unique comment to `Lib/abc.py`; a `finally` block restores the source and reindexes it. Assertions verify the marker appears once after the change and disappears after restoration.

### Resident queries

Twelve manually selected workloads; one warm-up and 30 measured repetitions each, top-10 results. These queries test supported operations on real source, not a representative traffic trace. Repeated result consistency was checked, but no independent relevance/ranking oracle was used for this corpus.

| Query | Total matches | C++ p50 ms | C++ p95 ms | Binding wall p50 ms |
|---|---:|---:|---:|---:|
| `memory` | 179 | 0.7554 | 0.8452 | 0.7620 |
| `thread` | 305 | 1.9583 | 2.0698 | 1.9668 |
| `memory allocation` | 23 | 2.2392 | 2.5724 | 2.2471 |
| `asyncio OR threading` | 296 | 2.9356 | 3.1300 | 2.9450 |
| `"event loop"` | 45 | 2.4562 | 2.5663 | 2.4620 |
| `"garbage collection"` | 26 | 1.3043 | 1.3522 | 1.3098 |
| `sched*` | 66 | 2.5368 | 2.7344 | 2.5435 |
| `memry~1` | 179 | 2.4970 | 2.6718 | 2.5041 |
| `(cache OR memory) NOT ext:txt` | 297 | 1.4460 | 1.7452 | 1.4541 |
| `path:asyncio thread` | 25 | 1.7298 | 1.7822 | 1.7353 |
| `def AND return` | 1,468 | 1.7965 | 3.3763 | 1.8214 |
| `lsebenchmarkabsenttoken` | 0 | 0.0009 | 0.0010 | 0.0015 |

Pooled C++ p50/p95: **1.9371 / 2.9430 ms**. Pooled binding wall p50/p95: **1.9525 / 2.9529 ms**. Percentiles represent this fixed mixture, including an absent-term query; consult per-query results before generalizing.

Semantic verification output:

```text
Verified checksum, structure, and every token position in 2291 documents.
```

## 4. One large text file

A deterministic ASCII fixture repeats `memory allocation cache locality scheduler latency kernel thread` until reaching at least 16 MiB. It stresses tokenization, positions and file handling; its tiny vocabulary and repeated content make it unsuitable for ranking-quality or general corpus claims.

| Property / operation | Result |
|---|---:|
| File size | 16,777,280 bytes |
| Default 8 MiB file limit | Skipped as expected |
| With `--max-file-mb 32` | 1 document indexed |
| Raised-limit indexing wall | 678.5 ms |
| Raised-limit peak process RSS | 1282.6 MiB |
| Snapshot size | 17.97 MiB |
| Semantic verification | Passed |

Each large-file indexing condition was measured once. A single file is one searchable document even if it contains millions of tokens.

## 5. Interpretation and limits

- The clearest next optimization target is index startup: native loading took a median 13.68 seconds while pooled resident search took 1.94 ms. This measurement combines deserialization, validation and BK-tree preparation; no profiler was run to attribute the time to an individual component.
- On CPython, unchanged reindexing took a median 29.37 seconds versus 14.66 seconds for a full rebuild, approximately 2.00× as long. Reusing postings currently does not offset the extra old-snapshot load and other work for this corpus.
- The single 16 MiB document reached 1,282.6 MiB process peak RSS, approximately 80× its source size. This is an observed peak of this indexing run, not a general memory formula. Large-document tokenization/index construction deserves memory profiling before increasing file-size limits.
- The completed synthetic tests demonstrate agreement with a simple BM25 token-scan reference and lower pooled search latency for that workload. They do not establish a universal speedup.
- Real CPython indexing exercises a much larger dictionary than the synthetic corpus. Synthetic document-count scaling alone cannot predict real-corpus latency or memory.
- Resident search and per-command CLI latency answer different questions. Reusing a loaded index avoids paying full load/validation for every query; the native bridge used by the API already supports this.
- Incremental reuse should not be described as always faster than rebuilding. Compare the measured wall samples above; unchanged operation retains full read/load/save work.
- No claim is made for 50,000 documents: fixture creation failed with `OSError: [Errno 28] No space left on device` before engine execution. Temporary synthetic files were cleaned automatically; a 20,000-file run completed instead. Only the newly downloaded CPython checkout was reduced to save space.
- Current CLI limits remain 100,000 candidate files, 256 MiB aggregate source and 8 MiB per file by default (configurable up to 64 MiB). These tests do not establish behavior at those maxima.
- No BEIR relevance evaluation, nDCG/Recall, PDF/Word extraction, cold-disk test, HTTP/UI latency, concurrent-client throughput, sustained-load, battery/thermal study or comparison with Spotlight/Lucene/SQLite FTS was performed.
- No production engine behavior was changed for this report. The new real-data harness contains assertions for incremental updates, repeatability and large-file limits; both real snapshots also passed semantic verification.

## 6. Reproduce

Run from the project root. Allow sufficient free disk space for corpus files, existing snapshots and temporary snapshot rewrites. This run encountered low disk space; free additional space before repeating larger tests.

```bash
cd local-search-engine
make build/lse build/benchmark_runner
mkdir -p build/performance-report

# If not already present:
git clone --depth 1 --sparse https://github.com/python/cpython.git build/performance-report/cpython
git -C build/performance-report/cpython fetch --depth 1 origin 4a9b98115fec4d93c32c554aa541d6c3fc4c3b27
git -C build/performance-report/cpython checkout --detach 4a9b98115fec4d93c32c554aa541d6c3fc4c3b27
git -C build/performance-report/cpython sparse-checkout set Lib

for n in 3000 10000 20000; do
  python3 scripts/benchmark.py --documents "$n" --repetitions 30 \
    --output "build/performance-report/synthetic-$n.json"
done

# Uses the existing project virtual environment / pybind11 installation:
make native
.venv/bin/python scripts/benchmark_real.py
```

Download and compilation are outside the timed regions. The real harness reuses its output paths, forces rebuilds for its initial-index measurements, temporarily edits only the downloaded `Lib/abc.py`, restores it, and leaves its datasets/snapshots under `build/performance-report/`. Synthetic corpus directories are temporary and removed automatically.

## 7. Raw observations

- [Environment, hashes and failed-run note](performance/2026-10-31/environment.json)
- [3,000 synthetic documents](performance/2026-10-31/synthetic-3000.json)
- [10,000 synthetic documents](performance/2026-10-31/synthetic-10000.json)
- [20,000 synthetic documents](performance/2026-10-31/synthetic-20000.json)
- [CPython and large-file observations](performance/2026-10-31/real.json)

The report folder retains raw JSON separately from ignored build artifacts. Source harnesses: `scripts/benchmark.py`, `tests/benchmark.cpp`, `scripts/benchmark_real.py`.

## Reading the percentiles

p50 is the median: at least half the samples complete within this time. At p95, at least 95% complete within the reported time. Resident search excludes startup and index loading, so it is not total web response latency. The reference is the project's token-scan ranker, not Google or Elasticsearch; human relevance quality was not measured.


## Follow-up: streaming UTF-8 traversal

On 1 November 2026, three alternating fresh-process indexing runs compared the previous implementation at `c171d07` with the streaming decoder on this same Mac. The fixture contained 1,150,000 bytes and 200,000 tokens, with one worker. Median process peak RSS fell from **93.73 MiB to 26.19 MiB**. Each saved snapshot passed semantic verification.

This removes the full decoded-rune intermediate from analysis and stored-content validation; it does not bound total indexing memory or eliminate token/posting allocations. These small-fixture observations do not replace the earlier corpus measurements, which remain historical. See [raw samples and fixture recipe](performance/utf8-streaming-memory.json) for flags, baseline revision and measurement boundaries.
