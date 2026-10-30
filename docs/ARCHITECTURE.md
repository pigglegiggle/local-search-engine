# Architecture and correctness

## Data flow

```text
Files → bounded parallel readers → UTF-8 tokens with ordinal positions
                                    ↓
                      per-document term → positions map
                                    ↓
                    sorted positional inverted index
                       ↙                         ↘
           delta/varint snapshot              BK-tree vocabulary
                                                  ↓
Query → lexer → precedence parser → sorted Boolean matches + BM25
                                                  ↓
                                         bounded top-k heap
                                                  ↓
                                        contextual text snippets
```

## Fundamental invariants

1. Documents are sorted by relative path; their array offset is the document ID.
2. Every dictionary term is exactly one normalized analyzer token.
3. Each term's document IDs are strictly increasing.
4. Token positions within a posting are strictly increasing and less than document length.
5. The sum of posting frequencies for a document equals its analyzed length.
6. `verify` checks the stronger invariant that every `(term, document, position)` actually matches stored source content.

Term positions are ordinal token positions, not byte offsets. The analyzer additionally returns byte offsets for UTF-8-safe snippet boundaries. No stopwords are removed, so an exact token phrase does not accidentally skip common words.

## Retrieval

Each leaf produces a vector sorted by document ID, containing scores and term explanations. AND intersects vectors using two pointers, OR unions them, and NOT creates the complement over all indexed documents. Equal IDs combine positive scores. This intentionally separates eligibility (Boolean logic) from relevance (BM25).

Phrase retrieval first intersects term results. For each surviving document, it tries each occurrence of the first term, then binary-searches for `start + offset` in the other terms' position lists. It handles repeated words and never matches across document boundaries. The algorithm is straightforward to verify; it does not yet use rarest-position anchoring or skip blocks.

With `N` documents, document length `dl`, mean length `avgdl`, frequency `tf`, and matching-document count `df`:

```text
idf   = ln(1 + (N - df + 0.5) / (df + 0.5))
score = idf × tf × (k1 + 1) / (tf + k1 × (1 - b + b × dl / avgdl))
k1 = 1.2; b = 0.75
```

Empty documents count in `N` and `avgdl`. Only documents with term occurrences are scored, so a corpus of empty documents cannot divide by zero. No proximity or phrase bonus is applied. NOT and metadata filters score zero. A repeated query term contributes repeatedly; this is documented rather than silently deduplicated.

The final heap keeps at most `k` matches, with the worst selected result at its root. A higher score wins; ties favor the lower document ID (therefore lexicographically earlier relative path). Selection costs `O(M log k)` and final ordering `O(k log k)`. Intermediate retrieval still stores up to `M` matches and explanations.

## Fuzzy matching

The vocabulary BK-tree uses exact Levenshtein distance over decoded Unicode codepoints. A node at distance `d` from the query can only have qualifying children on edges in `[d-r, d+r]`. This pruning requires a true metric: a distance approximation truncated at `r` would be incorrect for navigating the tree.

Levenshtein uses two DP rows: `O(ab)` time and `O(b)` auxiliary space for word lengths `a,b`. Each visited node incurs a distance computation. Worst-case traversal is the full vocabulary. Only terms up to 128 codepoints enter the BK-tree; longer terms remain in the positional dictionary. Prefix lookup uses the ordered dictionary, independently of the BK-tree.

## Incremental rebuild

Every eligible file is read, so same-size edits and unchanged mtimes are detected. Equal path and equal content permit reuse of previous token length and postings. Changed files are reanalyzed. New compact document IDs are assigned in sorted path order, and reused postings are remapped and merged with changed/new postings. Deleted, invalid, and newly excluded documents disappear from the new snapshot.

This trades additional source I/O and full snapshot writes for simple consistency. It does not claim constant-time updates. The worker pool owns separate prepared-document slots; the final dictionary merge is single-threaded and deterministic. Failures propagate after all worker threads are joined.

## Binary format, version 1

```text
8 bytes   ASCII LSEIDX01
8 bytes   payload length, unsigned little-endian
4 bytes   IEEE CRC32 of payload, little-endian
payload:
  root string
  document count
  documents: relative-path string, full-content string, token count
  dictionary count
  terms: word string, posting count
    postings: document-ID delta, position count, position deltas
```

All payload integers are unsigned base-128 varints. Strings have a varint byte length followed by raw UTF-8 bytes. Dictionary and document ordering is deterministic. The first document ID or token position is a delta from zero; later deltas must describe strictly increasing values. Loader bounds precede reserve/allocation operations, then structural validation follows decoding.

CRC32 detects damaged bytes, not hostile tampering. The index is a local snapshot, not a trusted external message format. Semantic verification can detect inconsistent but checksum-valid postings. The parser rejects unknown versions; it does not guess or silently migrate an incompatible file.

## Storage commit

CLI writers acquire `flock(LOCK_EX | LOCK_NB)` on a separate lock file. They load the prior snapshot while holding that lock, build the next snapshot, write an owner-only temporary file, `fsync` it, rename it, then sync the parent directory. Readers need no lock because an open file descriptor refers to one complete snapshot. Library callers of `save_index` must hold `WriteLock` themselves when multiple writers are possible.

Power-loss durability depends on filesystem and hardware guarantees; the test suite checks concurrent-process snapshot visibility, not physical power cuts. A directory-sync failure after rename is reported as such because replacement has already occurred.
