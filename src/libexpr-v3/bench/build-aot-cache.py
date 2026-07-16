#!/usr/bin/env python3
"""
build-aot-cache.py — R8a Phase 1 Day 4-6 deliverable.

Consumes a NIX_V3_AOT_BUILD_MODE manifest (one line per disk_cache
insert) + the v3 SQLite cache DB and produces a single mmap'd
flat file ready for distribution.

File format v1:

    Magic (8 bytes):     "V3AOTC01"
    Format version (4):  u32 = 1
    Entry count (4):     u32 N
    Entries (56 * N):    per-entry header (sorted-by-key)
        key (32):        SHA-256 cache key
        table_id (4):    1=CompilationUnits, 2=EvalResults
        _pad (4):        zero
        blob_offset (8): byte offset from start of file
        blob_length (8): byte length of the blob
    Blob data:           concatenated blobs starting at first
                         blob_offset

The entries are sorted by `key` so readers can do binary search.
Multiple identical (key, table_id) pairs in the manifest are
deduplicated — only the first occurrence is kept.

The whole file is content-addressed by SHA-256.  Print the digest
on success so distribution scripts can use it for substitution
keys / signatures.

Usage:
    build-aot-cache.py <manifest> [<sqlite-db>] -o <output>
    build-aot-cache.py --verify <output>   # round-trip sanity check

If <sqlite-db> is omitted, defaults to:
    $XDG_CACHE_HOME/nix/v3-bytecode-v3.sqlite
    (or ~/.cache/nix/v3-bytecode-v3.sqlite)

Companion docs:
    lode/AOT_DISTRIBUTION_2026-05-26.md (the Phase 1 plan)
    lode/AOT_PHASE1_DAY1-3_2026-05-26.md (the Day 1-3 manifest)
    lode/EVAL_CACHE_ARCHITECTURE_2026-05-23.md §4.3 + §7

Retirement criterion:
    AOT Phase 1 Day 13-15 measurement must show ≥30% warm-eval
    improvement on haskell-nix-example.  Below 15% triggers REVERT
    WITH DATA per AOT_DISTRIBUTION §7.2 — delete this script + the
    NIX_V3_AOT_BUILD_MODE flag + the disk_cache.cc recorder.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group.  SPDX-License-Identifier: Apache-2.0
"""

import argparse
import hashlib
import os
import sqlite3
import struct
import sys
from pathlib import Path


MAGIC = b"V3AOTC01"
FORMAT_VERSION = 1
TABLE_IDS = {"CompilationUnits": 1, "EvalResults": 2}
TABLE_NAMES = {v: k for k, v in TABLE_IDS.items()}
ENTRY_SIZE = 32 + 4 + 4 + 8 + 8  # 56 bytes
HEADER_SIZE = 8 + 4 + 4          # magic + format_version + entry_count


def default_db_path() -> Path:
    xdg = os.environ.get("XDG_CACHE_HOME")
    if xdg:
        base = Path(xdg)
    else:
        base = Path.home() / ".cache"
    return base / "nix" / "v3-bytecode-v3.sqlite"


def parse_manifest(path: Path):
    """Yield (table_id, key_bytes_32) tuples from the manifest, dedup'd."""
    seen = set()
    with open(path, "r") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) < 4:
                print(f"warning: skipping malformed manifest line: {line!r}",
                      file=sys.stderr)
                continue
            _ts, table, key_hex, _size = parts[:4]
            if table not in TABLE_IDS:
                print(f"warning: unknown table {table!r}, skipping",
                      file=sys.stderr)
                continue
            if len(key_hex) != 64:
                print(f"warning: malformed key hex {key_hex!r}", file=sys.stderr)
                continue
            try:
                key = bytes.fromhex(key_hex)
            except ValueError:
                print(f"warning: non-hex key {key_hex!r}", file=sys.stderr)
                continue
            table_id = TABLE_IDS[table]
            sig = (table_id, key)
            if sig in seen:
                continue
            seen.add(sig)
            yield sig


def fetch_blobs(db_path: Path, entries):
    """Return list of (table_id, key, blob_bytes), skipping rows that
    aren't present in the DB."""
    conn = sqlite3.connect(str(db_path))
    try:
        out = []
        cur = conn.cursor()
        for table_id, key in entries:
            table = TABLE_NAMES[table_id]
            row = cur.execute(
                f"SELECT blob FROM {table} WHERE key = ?",
                (key,)
            ).fetchone()
            if row is None:
                print(f"warning: {table} key {key.hex()} not in DB; skipping",
                      file=sys.stderr)
                continue
            out.append((table_id, key, row[0]))
        return out
    finally:
        conn.close()


def write_aot_cache(rows, output: Path):
    """Write the sorted-by-key flat file.  Returns (entry_count, total_bytes,
    sha256_hex)."""
    # Sort by key so the reader can binary-search.
    rows.sort(key=lambda r: r[1])

    n = len(rows)
    # Layout: header (16 B) + entries (56 * N) + blobs.
    # WS5-D2a: each blob is placed at an 8-byte-aligned file offset so the
    # deserializeCUBorrowed path can `reinterpret_cast` the CU's POD block
    # (int64/double constants need 8-byte alignment) directly out of the
    # page-aligned mmap.  Alignment padding is dead space BETWEEN blobs; each
    # entry's blob_length stays the exact serialized length.  HEADER_SIZE (16)
    # and ENTRY_SIZE (56) are both multiples of 8, so the first blob is
    # already aligned; only inter-blob gaps need padding.
    ALIGN = 8

    def align_up(x):
        return (x + ALIGN - 1) & ~(ALIGN - 1)

    blob_offset_start = HEADER_SIZE + n * ENTRY_SIZE
    offset_cursor = blob_offset_start

    with open(output, "wb") as f:
        # Magic + format version + entry count.
        f.write(MAGIC)
        f.write(struct.pack("<II", FORMAT_VERSION, n))
        # Pre-compute 8-aligned offsets so the entry table is written in order.
        offsets = []
        for table_id, key, blob in rows:
            offset_cursor = align_up(offset_cursor)
            offsets.append(offset_cursor)
            offset_cursor += len(blob)
        # Entry table.
        for (table_id, key, blob), off in zip(rows, offsets):
            f.write(key)
            f.write(struct.pack("<II", table_id, 0))
            f.write(struct.pack("<QQ", off, len(blob)))
        # Blob data, each padded up to its 8-aligned offset.
        cur = blob_offset_start
        for (table_id, key, blob), off in zip(rows, offsets):
            if off > cur:
                f.write(b"\x00" * (off - cur))
                cur = off
            f.write(blob)
            cur += len(blob)

    # Content-address: SHA-256 of the whole file.
    h = hashlib.sha256()
    with open(output, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return n, offset_cursor, h.hexdigest()


def verify(output: Path):
    """Open the flat file and walk the entry table + blob region.
    Returns 0 on success, non-zero on format violation."""
    with open(output, "rb") as f:
        magic = f.read(8)
        if magic != MAGIC:
            print(f"verify FAIL: bad magic {magic!r}, expected {MAGIC!r}",
                  file=sys.stderr)
            return 1
        fmt_ver, n = struct.unpack("<II", f.read(8))
        if fmt_ver != FORMAT_VERSION:
            print(f"verify FAIL: format version {fmt_ver}, "
                  f"expected {FORMAT_VERSION}", file=sys.stderr)
            return 1
        prev_key = b""
        last_offset = HEADER_SIZE + n * ENTRY_SIZE
        for i in range(n):
            key = f.read(32)
            table_id, _pad = struct.unpack("<II", f.read(8))
            offset, length = struct.unpack("<QQ", f.read(16))
            if table_id not in TABLE_NAMES:
                print(f"verify FAIL: entry {i} unknown table_id {table_id}",
                      file=sys.stderr)
                return 1
            if key <= prev_key and i > 0:
                print(f"verify FAIL: entry {i} key not strictly ascending",
                      file=sys.stderr)
                return 1
            prev_key = key
            # WS5-D2a: blobs are 8-aligned, so an entry's offset is the
            # previous blob's end rounded UP to 8 (there may be up to 7 pad
            # bytes of dead space in between).  It must also be 8-aligned.
            expected = (last_offset + 7) & ~7
            if offset != expected:
                print(f"verify FAIL: entry {i} offset {offset} != expected "
                      f"(8-aligned) {expected}", file=sys.stderr)
                return 1
            if offset % 8 != 0:
                print(f"verify FAIL: entry {i} offset {offset} not 8-aligned",
                      file=sys.stderr)
                return 1
            last_offset = offset + length
        # File should end at the last blob's end (no trailing pad after the
        # final blob).
        file_size = output.stat().st_size
        if file_size != last_offset:
            print(f"verify FAIL: trailing junk ({file_size - last_offset} B) "
                  f"after last blob", file=sys.stderr)
            return 1
        # SHA-256.
        f.seek(0)
        h = hashlib.sha256()
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
        print(f"verify OK: {n} entries, {file_size} bytes, sha256={h.hexdigest()}")
        return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--verify", metavar="OUTPUT",
                    help="verify the produced flat file structure")
    ap.add_argument("manifest", nargs="?",
                    help="NIX_V3_AOT_BUILD_MODE manifest file")
    ap.add_argument("sqlite_db", nargs="?",
                    help="optional path to v3-bytecode-v3.sqlite "
                         "(defaults to XDG cache location)")
    ap.add_argument("-o", "--output", metavar="PATH",
                    help="output flat-file path")
    args = ap.parse_args()

    if args.verify:
        sys.exit(verify(Path(args.verify)))

    if not args.manifest or not args.output:
        ap.print_usage(sys.stderr)
        print("error: <manifest> and -o <output> are required when not "
              "verifying", file=sys.stderr)
        sys.exit(2)

    manifest = Path(args.manifest)
    output = Path(args.output)
    db_path = Path(args.sqlite_db) if args.sqlite_db else default_db_path()

    if not manifest.exists():
        print(f"error: manifest {manifest} not found", file=sys.stderr)
        sys.exit(2)
    if not db_path.exists():
        print(f"error: SQLite DB {db_path} not found", file=sys.stderr)
        sys.exit(2)

    print(f"reading manifest {manifest}", file=sys.stderr)
    entries = list(parse_manifest(manifest))
    print(f"  {len(entries)} unique (table, key) pairs", file=sys.stderr)

    print(f"fetching blobs from {db_path}", file=sys.stderr)
    rows = fetch_blobs(db_path, entries)
    if not rows:
        print("error: no blobs available — DB out of sync with manifest?",
              file=sys.stderr)
        sys.exit(1)
    print(f"  {len(rows)} blobs retrieved", file=sys.stderr)

    print(f"writing {output}", file=sys.stderr)
    n, total, digest = write_aot_cache(rows, output)
    print(f"AOT cache built: entries={n} size={total} sha256={digest}")


if __name__ == "__main__":
    main()
