# PagerDB

[![CI](https://github.com/Anashwara16/kvstore_engine/actions/workflows/ci.yml/badge.svg)](https://github.com/Anashwara16/kvstore_engine/actions/workflows/ci.yml)

A file-backed, crash-safe key-value storage engine in modern C++ (C++23),
built from the disk up: a slotted-page store, an on-disk B+ tree with latch
crabbing, a thread-safe buffer pool with LRU eviction, a write-ahead log
with redo+undo recovery, and transactions with strict two-phase locking
(wait-die deadlock avoidance). Single process, multi-threaded.

This is a learning/portfolio engine — the goal is a correct, legible
implementation of the ideas behind SQLite/LMDB/InnoDB, not a production
database. It is thoroughly tested (17 test binaries, seeded reference-model
property tests, tsan-clean concurrency stress suites, and a fork-based
crash harness with concurrent writers) and benchmarked against SQLite.

```cpp
#include "kvstore/db.h"

kvstore::Options options;
options.create_if_missing = true;
auto db = std::move(kvstore::DB::Open("/tmp/mydb", options).value());

db->Put("name", "Alice");
auto v = db->Get("name");            // v.value() == "Alice"
db->Scan("a", "z", [](auto k, auto val) { /* ... */ return true; });
db->Delete("name");
```

## What's inside

| Layer | Header | What it does |
|-------|--------|--------------|
| Public API | `kvstore/db.h` | Get / Put / Delete / Contains / Scan / Sync / Checkpoint |
| B+ tree | `kvstore/btree/btree.h` | Ordered index, leaf/internal splits, sibling-linked leaves, cursor scans |
| Buffer pool | `kvstore/storage/buffer_pool.h` | Frame cache, RAII page guards, LRU eviction, write-ahead rule |
| WAL | `kvstore/wal/` | Redo log, physiological records, idempotent replay, checkpoints |
| Pager | `kvstore/storage/pager.h` | 4 KB pages, dual meta, free list, CRC-32 checksums |
| File | `kvstore/storage/file.h` | RAII `pread`/`pwrite`/`fsync` wrapper |

Design docs: **[ARCHITECTURE.md](docs/ARCHITECTURE.md)** (layers + Get/Put/
Recovery lifecycles) · **[FORMAT.md](docs/FORMAT.md)** (byte-level on-disk
layout, v3) · **[DESIGN_DECISIONS.md](docs/DESIGN_DECISIONS.md)** (every choice
and how it held up) · **[BENCHMARKS.md](docs/BENCHMARKS.md)** (numbers +
methodology).

## Build & test

Requires CMake ≥ 3.21 and a C++23 compiler (Apple/LLVM Clang with libc++, or
GCC 14+). GoogleTest and Google Benchmark are fetched automatically.

```bash
cmake --preset asan          # or: debug, release, bench
cmake --build --preset asan -j
ctest --preset asan          # 14 test binaries, green under ASan+UBSan
```

## Durability demo: load → kill -9 → recover → verify

The engine never returns a corrupt tree after a hard kill. Recovery replays
the WAL and leaves exactly the durable prefix of the write stream:

```bash
tools/crash_demo.sh 500000
```

```
== Loading 500000 keys into /tmp/.../crashdemo (will be killed mid-flight) ==
== kill -9 sent to loader ==
== Reopening (runs recovery) and verifying ==
verify OK: 17078 of 500000 puts survived as a clean prefix (num_keys agrees)
== Structural integrity check via kvstore_inspect ==
kvstore_inspect: tree + leaf chain + free list consistent (exit 0)
```

The loader runs with `sync_on_write=false`, so this demonstrates crash
*consistency* (no corruption, clean prefix, agreeing key count) rather than
per-write durability. Turn `sync_on_write` on and every acknowledged write is
recoverable — at the cost of an fsync per Put (see the benchmark table).

## Benchmarks

One command builds the `bench` preset, runs PagerDB and the SQLite baseline
on identical workloads (100k keys, 16-byte keys, 100-byte values, seed 42),
and saves raw output under `docs/bench_results/`:

```bash
tools/run_bench.sh
```

Throughput on an Apple M2 (8 GB, Apple Clang 21, `-O3`), warm buffer pool,
`sync_on_write=false` unless noted. Full methodology, latency percentiles,
and fairness notes in [BENCHMARKS.md](docs/BENCHMARKS.md).

| Workload | PagerDB | SQLite (WAL) | Ratio |
|----------|--------:|-------------:|------:|
| Sequential load (100k puts) | **303.5k/s** | 58.6k/s | 5.2× |
| Random load (100k puts) | **121.9k/s** | 44.4k/s | 2.7× |
| Sequential load, `sync_on_write` | 32.1k/s | 13.5k/s | 2.4× |
| Point read, uniform, warm | **854.0k/s** | 172.6k/s | 4.9× |
| Point read, Zipfian, warm | **880.8k/s** | 186.6k/s | 4.7× |
| 95% read / 5% write, warm | **542.9k/s** | 173.1k/s | 3.1× |
| Scan-heavy (100-entry ranges) | **20.6M entries/s** | 5.6M/s | 3.7× |
| Delete churn (delete+reinsert) | **160.5k/s** | 56.2k/s | 2.9× |

Phase 6 profiling-driven optimizations (hardware CRC-32, in-place slot-shift
inserts, and fixing a quadratic `reserve()` in the WAL append path) improved
PagerDB's own throughput 2.6–4.9× over the start of the phase — see the
[progression table](docs/BENCHMARKS.md#phase-6-optimization-progression-pagerdb-warm).

Numbers are single-machine data points for tracking relative progress, not
audited comparisons. SQLite carries a SQL layer and full ACID guarantees
PagerDB does not yet have; PagerDB is a narrower, lower-level engine.

## Project status

Phases 0–6 of a 6-phase plan (`plan.md`, `phases.md`) are complete through
checkpoints C0–C6. What works today: ordered Get/Put/Delete, range scans,
on-demand compaction and page reuse, a thread-safe buffer pool, CRC-checked
pages, dual meta pages, WAL-based crash recovery (redo + undo of in-flight
transactions) verified by a fork harness with single and concurrent writers,
concurrent readers/writers via latch crabbing, and explicit transactions
(`DB::Begin`) with strict 2PL between transactions. Stated precisely, the
isolation level is: explicit transactions are serializable against each other
for point operations only (per-key S/X locks, no phantom protection); scans
are read-uncommitted; autocommit ops take no key locks and ignore transaction
boundaries. MVCC (5.6) remains a documented stretch goal.

## Known limitations & Phase 7 candidates

- **No overflow pages** — key+value must fit in one 4 KB page; anything
  larger is rejected up front (`InvalidArgument`). Keep records under ~1 KB
  for reliability: bigger ones usually work, but the count-based leaf split
  gives no byte guarantee, so an unlucky page packing can fail a Put
  mid-split (handle poisoned until reopen). First thing a real user hits;
  the fix is overflow pages plus a byte-aware split point.
- **WAL truncation waits for open transactions** — checkpoints flush pages
  regardless, but the log is only truncated when no written-to transaction
  is in flight, so log growth is bounded by transaction lifetime. Refinement:
  truncate up to the oldest active transaction's begin-LSN.
- **`root_latch` is a process-global reader-writer lock** touched by every
  operation; even shared acquisition bounces its cache line across cores. At
  high thread counts this becomes the scalability ceiling before the lock
  manager does. Direction: version-validated optimistic root reads (OLC).
- **No group commit** — under `sync_on_write`, N concurrent committers pay N
  serialized fsyncs. Unblocked since Phase 5's threading landed; the obvious
  next performance item (batch commit records into one fsync).
- **Isolation boundaries** — see Project status above; MVCC is the stretch
  answer for scans and phantoms.
