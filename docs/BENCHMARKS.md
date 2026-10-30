# Reproducible measurements

See the [measured performance report](PERFORMANCE_REPORT.md) for synthetic scaling, CPython/Lib, a 16 MiB file, process peak memory, raw observations and reproduction commands. The real-data harness, `scripts/benchmark_real.py`, uses the existing native Python binding and macOS `/usr/bin/time -l`.

Run from the repository root:

```sh
make benchmark
```

For another size:

```sh
make build/benchmark_runner
python3 scripts/benchmark.py --documents 10000 --repetitions 20 --output build/benchmark-10000.json
```

## Corpus

Python's seeded RNG (`20261030`) generates 180 tokens per document from 1,000 synthetic identifiers, plus controlled common phrases. Terms appear at different frequencies. The harness runs eight two-term AND queries and four ordered phrases, including rare and absent terms. These are synthetic source-like text files, not a representative sample of Thai, PDFs, natural-language relevance, or a production workload.

## Baseline and correctness

The reference baseline scans resident, pre-tokenized documents, counts the two query terms, checks adjacency where required, computes BM25 from precomputed document frequencies, and sorts matches. It can immediately return empty results for a globally absent term. Neither baseline tokenization nor engine index loading is included in query timing.

For every workload, before timing, compare total matching documents, top-10 document IDs in order, and BM25 scores within `1e-9`. A mismatch aborts the benchmark. This verifies retrieval/ranking agreement, not whether human users consider BM25 results relevant.

The engine additionally parses queries, builds term explanations, and extracts snippets for its top 10 results. The baseline does not create snippets or explanations. Therefore this is a conservative comparison of the application retrieval path against a straightforward token-scan ranker, not a claim that every component is identical. It does not compare against ripgrep, Spotlight, SQLite FTS, Lucene, or Elasticsearch.

## Timing

Each workload has a correctness/warm-up run, followed by repeated measurements. Execution order alternates. The output reports pooled p50/p95 across all query samples, not an average of per-query ratios. Resident-memory measurements use a C++ steady clock. Both methods consume results so work is not optimized away.

Also recorded separately:

- Entire first CLI indexing process, including saving.
- Entire unchanged reindex process, including old-index loading and snapshot rewrite.
- Index bytes and source bytes (the index includes full document content).
- Structural load validation and BK-tree preparation time.
- Five fresh search-process wall times, including startup, index load, query execution, and JSON output. The OS filesystem cache is **not** flushed, so these are not cold-storage measurements.

Results depend on hardware, compiler, system load, and corpus. Do not put an unexplained multiplier in a resume. State the exact corpus size, baseline, metric, and environment, and retain the JSON report.
