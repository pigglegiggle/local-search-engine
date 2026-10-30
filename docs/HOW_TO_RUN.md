# How to run Local Search Engine

The command-line engine indexes UTF-8 text/source files into a local snapshot. Search reads that snapshot, so it works without rereading the original files. The optional web interface uses the same C++ engine through a native Python binding.

## Requirements

- A C++17 compiler and `make`. The default compiler is `clang++`.
- POSIX filesystem APIs (`flock`, `fsync`, atomic rename). macOS is the measured/tested platform; Windows is not supported natively.
- Python 3 for CLI integration tests and synthetic benchmarks. The optional web stack and real-data benchmark were tested with Python **3.13**; use that version to reproduce the recorded setup.
- A browser for the optional Streamlit UI. No NVIDIA GPU, CUDA installation, external database or API key is required.

On macOS, install Apple's command-line tools if needed:

```sh
xcode-select --install
```

Clone and enter the repository:

```sh
git clone https://github.com/pigglegiggle/local-search-engine.git
cd local-search-engine
```

Run all following commands from that directory. Avoid indexing folders with private files if you plan to share the resulting snapshot: snapshots contain full document text.

## 1. Build and try the demo

```sh
make
make demo
```

`make` builds `build/lse`. A subsequent `make` may print `Nothing to be done`: that means the executable is already up to date. `make demo` indexes `examples/corpus` into `build/demo.lsi` and runs a sample query with score explanations.

Example demo output begins with:

```text
Indexed 5 documents · 120 terms · 161 tokens
```

The added/changed/unchanged counters depend on whether a previous demo snapshot exists. Timing depends on the machine. The demo queries only the included example files; it does not search the entire computer.

## 2. Index your own folder

```sh
./build/lse index ./examples/corpus --index build/my-notes.lsi --threads 4
./build/lse stats --index build/my-notes.lsi
./build/lse verify --index build/my-notes.lsi
```

Replace `./examples/corpus` with a folder you choose, such as `"$HOME/Documents/my-notes"`. Quote paths with spaces. Indexing recursively visits supported files in that folder. `--threads 4` selects four reader/tokenizer workers; the final index merge is deterministic.

- `--index` chooses the saved snapshot. Use the same path for indexing and searching.
- `stats` reports indexed documents, vocabulary, tokens, postings, source/index bytes and load time.
- `verify` validates the checksum and structure, then reconstructs postings from stored text and checks every token position.

Without `--index`, the CLI uses `.local-search/index.lsi` relative to your current working directory. Supported extensions and skip rules are listed in the [README](../README.md#scope-and-limits).

### Update after editing files

```sh
./build/lse index ./examples/corpus --index build/my-notes.lsi --threads 4
```

The same command detects additions, changed bytes and removals. Unchanged positional postings are reused, but the engine still reads source bytes, loads the old index and writes a complete replacement snapshot. An unchanged run can be slower than rebuilding on some corpora; see the [measurements](PERFORMANCE_REPORT.md).

To explicitly rebuild a damaged/incompatible snapshot from its source files:

```sh
./build/lse index ./examples/corpus --index build/my-notes.lsi --rebuild
```

## 3. Search from the terminal

```sh
# Exact analyzed-token phrase, with per-term BM25 contributions.
./build/lse search '"memory allocation"' --index build/my-notes.lsi --explain

# Boolean conditions and a file-extension exclusion.
./build/lse search '(cache OR memory) NOT ext:cpp' --index build/my-notes.lsi

# Prefix lookup or a one-edit typo, with machine-readable output.
./build/lse search 'sched* OR memry~1' --index build/my-notes.lsi --json

# Keep the index loaded while entering multiple queries.
./build/lse repl --index build/my-notes.lsi --explain
```

In the REPL, enter only the query, such as `memory OR cache`. Enter `:quit` to exit. Query operators must be uppercase, and spaces between terms imply AND. Shell quotes protect parentheses, wildcards and phrase quotes. See the [query language](../README.md#query-language) for the full table.

Results show the relative file path, BM25 score and contextual snippet. A larger score means a stronger match for that query under this ranking formula; scores are not probabilities and should not be compared across unrelated queries. `--explain` adds term frequency, document frequency and score contributions.

The printed in-memory search time excludes loading. Each separate CLI invocation loads the index again; use the REPL or API to amortize this cost.

## 4. Start the web UI and API

Install Python 3.13 first if necessary, then:

```sh
python3 --version
make web-setup
make web
```

`web-setup` creates `.venv`, installs the pinned `requirements-web.txt` dependencies and compiles `web/_native` for that Python interpreter. If your `python3` is another version, use these equivalent setup commands with your Python 3.13 executable instead:

```sh
# Alternative when Python 3.13 is installed as python3.13:
python3.13 -m venv .venv
.venv/bin/python -m pip install -r requirements-web.txt
make native
```

`make web` builds/updates the demo index, builds the native binding and launches both services:

- UI: <http://127.0.0.1:8501>
- API documentation: <http://127.0.0.1:8000/docs>
- API health: <http://127.0.0.1:8000/health>

Keep the terminal open. Press **Ctrl+C** there to stop both owned service processes. After installation, rerun `make web` to start again.

### Use a different default folder in the UI

Index it first, then pass the saved snapshot to the launcher:

```sh
./build/lse index ./examples/corpus --index build/my-notes.lsi --threads 4
LSE_INDEX="$PWD/build/my-notes.lsi" make web
```

The launcher still prepares the small demo index, but the API serves the snapshot selected by `LSE_INDEX`. This selects a saved index, not a folder to scan automatically. If the launcher is already running, stop it before starting this command.

After changing source files, run the CLI indexing command again and click **Reload index** in the UI sidebar. Reload loads a new saved snapshot; it does not scan the source folder. Searches already using an older snapshot can finish safely.

### Upload files

1. Open the UI and choose UTF-8 text/source files under **Upload files**.
2. Click **Index uploaded files**.
3. Enter a query and click the blue **Search** button.
4. Enable **Include BM25 explanations** to inspect score contributions.
5. Use **Search in** in the sidebar to switch between uploaded files and the default collection.

Upload limits: **50 files**, **8 MiB each**, **32 MiB aggregate**. Duplicate filenames, unsupported extensions, invalid UTF-8 and binary content are rejected. PDF, Word, images, ZIP archives and browser folder uploads are not implemented. Use CLI indexing for large existing folders.

Uploads create a separate temporary collection and do not overwrite the default snapshot. At most four collections are retained; least recently used collections can expire. They are removed when the API stops, and restarting requires reuploading them. Keep your original files separately.

### Run the services separately

Use two terminals from the repository root:

```sh
# Terminal 1: build the prerequisites, then run the API.
make web-demo native
make api

# Terminal 2: run the UI.
make ui
```

When using the separate targets, stop each terminal with Ctrl+C. Set `LSE_INDEX` on the API command to select a custom snapshot. The UI reads `LSE_API_URL`, defaulting to `http://127.0.0.1:8000`.

## 5. Call the API

With the API running:

```sh
curl http://127.0.0.1:8000/health
curl http://127.0.0.1:8000/stats

curl --get http://127.0.0.1:8000/search \
  --data-urlencode 'q="memory allocation"' \
  --data-urlencode 'k=10' \
  --data-urlencode 'explain=true'

# Load the default index after the CLI has replaced its snapshot.
curl -X POST http://127.0.0.1:8000/reload

# Create an isolated upload collection.
curl -X POST http://127.0.0.1:8000/collections \
  -F 'files=@examples/corpus/notes/memory.md' \
  -F 'files=@examples/corpus/notes/gpu.md'
```

The upload response contains `collection_id`. Add `--data-urlencode 'collection=YOUR_COLLECTION_ID'` to a search request, or call `/stats?collection=YOUR_COLLECTION_ID`, to use it. `/reload` applies to the default on-disk index.

The search response contains `query`, `total`, `search_ms`, `request_ms`, `snapshot_id`, `expanded_terms` and `hits`. A hit contains `path`, `score`, `snippet` and optional `explanation` entries.

- `search_ms`: C++ resident retrieval time.
- `request_ms`: API service search work, including native binding conversion; not complete network latency.
- UI **API round trip**: client-observed API request time; excludes full page rendering.

The services are intended for local use. Authentication, multi-user access controls and public hosting configuration are not implemented.

## 6. Tests and benchmarks

```sh
make test       # C++ checks and black-box CLI integration tests
make web-test   # Requires web-setup; HTTP/native parity and UI tests
make sanitize   # AddressSanitizer + UndefinedBehaviorSanitizer checks
make benchmark # Default 3,000-document synthetic benchmark
```

For another synthetic size:

```sh
make build/lse build/benchmark_runner
python3 scripts/benchmark.py --documents 10000 --repetitions 30 \
  --output build/benchmark-10000.json
```

Synthetic datasets are generated in a temporary folder and removed automatically; the JSON report remains. The harness verifies ranking/count/score agreement before timing. Keep enough free disk space for many small files and snapshots.

See [benchmark methodology](BENCHMARKS.md) and the [full performance report](PERFORMANCE_REPORT.md) for the pinned CPython dataset, 16 MiB fixture, real-data harness, timing boundaries, raw JSON and reproduction commands.

## 7. Troubleshooting

| Symptom | Action |
|---|---|
| `build/lse` does not exist | Run `make` from the repository root. |
| `make: Nothing to be done` | Build is already up to date; run the executable or `make demo`. |
| Missing native module or incompatible Python ABI | Run `make web-setup` / `make native` using the same `.venv`, then restart the API. Recreate `.venv` if its Python interpreter changed. |
| Ports 8000/8501 are already in use | Stop the previous launcher/services; do not start a second `make web` instance. |
| API cannot open its index | Run `make web-demo`, or check the absolute `LSE_INDEX` path. |
| Search misses a recently edited file | Run CLI indexing again, then reload the API snapshot; saved indexes are not live filesystem searches. |
| No results | Check the selected collection. Try one word, `OR`, a prefix or typo tolerance; implicit AND requires all terms. |
| File was skipped | Inspect CLI warnings; check supported extension, UTF-8 validity, binary content and the per-file size limit. |
| One large supported file exceeds 8 MiB | For CLI only, explicitly set `--max-file-mb 32` (allowed range 1–64). Raising the limit can require much more RAM than the source size. |
| Prefix/fuzzy query expands too broadly | Use a longer prefix or a smaller edit radius; expansion is capped at 128 terms. |
| Upload collection expired | Upload the files again; the default collection is independent. |
| `No space left on device` | Free disk space before retrying. Do not treat an incomplete run as a successful benchmark. |

`make clean` deletes the entire generated `build/` folder, including downloaded benchmark corpora, generated reports and any custom snapshots stored there. Published raw observations under `docs/performance/` are separate. `.venv` and original source folders are not removed by that target.
