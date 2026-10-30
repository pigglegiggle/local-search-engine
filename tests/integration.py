#!/usr/bin/env python3
"""Black-box persistence, incremental rebuild, filesystem, and failure tests."""
import concurrent.futures
import fcntl
import json
import pathlib
import random
import subprocess
import sys
import tempfile
import zlib

binary = pathlib.Path(sys.argv[1]).resolve()
checks = 0


def check(condition, message):
    global checks
    checks += 1
    if not condition:
        raise AssertionError(message)


with tempfile.TemporaryDirectory(prefix="lse-tests-") as temporary:
    base = pathlib.Path(temporary)
    root = base / "corpus"
    root.mkdir()
    index = base / "index.lsi"

    def run(*args, ok=True, target=index):
        result = subprocess.run([str(binary), *map(str, args), "--index", str(target)],
                                capture_output=True, text=True, timeout=30)
        check((result.returncode == 0) == ok, f"{args}: {result.stdout} {result.stderr}")
        return result

    def search(query, **kwargs):
        return json.loads(run("search", query, "--json", **kwargs).stdout)

    (root / "a.md").write_text("Memory allocation cache cache\nไทย สวัสดี", encoding="utf-8")
    (root / "b.cpp").write_text("allocation memory scheduler", encoding="utf-8")
    (root / "empty.txt").write_text("")
    (root / 'quote"name.md').write_text("quote filename")
    (root / "ภาษาไทย.md").write_text("ไทย สวัสดี")
    (root / "binary.txt").write_bytes(b"memory\x00binary")
    (root / "invalid.txt").write_bytes(b"\xff\xfe")
    (root / "ignored.pdf").write_text("memory")
    (root / "build").mkdir()
    (root / "build" / "hidden.md").write_text("memory")
    outside = base / "outside.md"
    outside.write_text("secret external data")
    (root / "symlink.md").symlink_to(outside)
    result = run("index", root, "--threads", 3)
    check("skipped 2" in result.stdout, "binary and invalid UTF-8 reported")
    check(search("memory")["total"] == 2, "exact retrieval persisted")
    check(search('"memory allocation"')["total"] == 1, "phrase persisted")
    check(search("secret")["total"] == 0, "symlinks not followed")
    check(search("ไทย")["total"] == 2, "Unicode persisted")
    check(search("quote")["hits"][0]["path"] == 'quote"name.md', "JSON escaping")
    run("verify")
    stats = json.loads(run("stats", "--json").stdout)
    check(stats["documents"] == 5, "document inclusion")
    original = index.read_bytes()
    result = run("index", root, "--threads", 1)
    check("unchanged 5" in result.stdout, "reuse unchanged postings")
    check(index.read_bytes() == original, "deterministic index across thread counts")

    (root / "a.md").write_text("cache changed")
    (root / "b.cpp").unlink()
    (root / "new.md").write_text("brand new scheduler")
    result = run("index", root)
    check("added 1" in result.stdout.lower() and "changed 1" in result.stdout and "removed 1" in result.stdout,
          "incremental add/change/remove report")
    check(search("memory")["total"] == 0, "stale postings removed")
    check(search("scheduler")["hits"][0]["path"] == "new.md", "new postings searchable")
    run("verify")
    rebuilt = base / "fresh.lsi"
    run("index", root, target=rebuilt)
    check(index.read_bytes() == rebuilt.read_bytes(), "incremental and full rebuild byte equivalence")
    saved = index.read_bytes()

    with open(str(index) + ".lock", "r+") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        run("index", root, ok=False)
        check(index.read_bytes() == saved, "competing writer cannot alter index")
        check(search("scheduler")["total"] == 1, "readers work under writer lock")

    run("index", base / "missing", ok=False)
    check(index.read_bytes() == saved, "failed scan preserves previous index")
    for args in [("search", "memory OR"), ("search", "*"), ("search", "x", "-k", "-1"),
                 ("index", root, "--threads", "0"), ("search", "x", "--wat"), ("search", "x", "--index")]:
        # Missing-value parsing needs a direct call, since run() appends --index.
        if args[-1] == "--index":
            r = subprocess.run([str(binary), *args], capture_output=True, timeout=10)
            check(r.returncode != 0, "missing option value rejected")
        else:
            run(*args, ok=False)

    corrupt = base / "corrupt.lsi"
    for cut in (0, 1, 8, 19, 20, len(saved) // 2, len(saved) - 1):
        corrupt.write_bytes(saved[:cut])
        run("stats", target=corrupt, ok=False)
    for offset in (0, 8, 16, 20, len(saved) - 1):
        mutated = bytearray(saved)
        mutated[offset] ^= 0x7f
        corrupt.write_bytes(mutated)
        run("verify", target=corrupt, ok=False)
    # Correct CRC with malformed varints exercises the parser beyond checksum rejection.
    for payload in (b"\xff" * 12, b"\x01/\xff" * 5, b"\x01/\x00\x00extra"):
        corrupt.write_bytes(b"LSEIDX01" + len(payload).to_bytes(8, "little") +
                            zlib.crc32(payload).to_bytes(4, "little") + payload)
        run("stats", target=corrupt, ok=False)

    # Mutate checksum-valid payloads to exercise bounded structural decoding.
    rng = random.Random(20261030)
    for _ in range(40):
        payload = bytearray(saved[20:])
        payload[rng.randrange(len(payload))] ^= 1 << rng.randrange(8)
        corrupt.write_bytes(b"LSEIDX01" + len(payload).to_bytes(8, "little") +
                            zlib.crc32(payload).to_bytes(4, "little") + payload)
        result = subprocess.run([str(binary), "verify", "--index", str(corrupt)],
                                capture_output=True, text=True, timeout=30)
        check(result.returncode in (0, 1), "checksum-valid malformed index does not crash")

    corrupt.write_bytes(b"damaged snapshot")
    run("index", root, target=corrupt, ok=False)
    check(corrupt.read_bytes() == b"damaged snapshot", "corrupt snapshot not silently overwritten")
    run("index", root, "--rebuild", target=corrupt)
    check(corrupt.read_bytes() == saved, "explicit full rebuild recovers corrupted snapshot")
    run("verify", target=corrupt)

    # Concurrent readers see complete snapshots during atomic replacement.
    def reader(_):
        for _ in range(4):
            result = subprocess.run([str(binary), "search", "scheduler", "--json", "--index", str(index)],
                                    capture_output=True, text=True, timeout=30)
            if result.returncode or json.loads(result.stdout)["total"] != 1:
                return False
        return True

    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as executor:
        readers = [executor.submit(reader, i) for i in range(3)]
        for _ in range(3):
            run("index", root)
        check(all(f.result() for f in readers), "atomic replacement supports concurrent readers")

    empty = base / "empty"
    empty.mkdir()
    run("index", empty)
    check(search("NOT missing")["total"] == 0, "empty corpus and changed root")
    run("verify")

    # A custom index with a supported text extension must not index itself.
    custom = root / "snapshot.md"
    run("index", root, target=custom)
    first_custom = custom.read_bytes()
    run("index", root, target=custom)
    check(custom.read_bytes() == first_custom, "custom index file excluded from source corpus")

    (root / "large.txt").write_text("x" * (1024 * 1024 + 1))
    oversized = run("index", root, "--max-file-mb", 1)
    check("exceeds file size limit" in oversized.stderr, "oversized files skipped with warning")

print(f"{checks} integration checks passed.")
