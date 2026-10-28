# PagerDB Benchmarks

Single-machine numbers for tracking relative progress, not audited
comparisons. Reproduce everything below with one command:

```bash
tools/run_bench.sh              # builds the 'bench' preset, runs both binaries,
                                # saves raw output under docs/bench_results/
```

## Methodology

- **Harness:** Google Benchmark (`bench/db_bench.cc`), pinned v1.9.1. The
  SQLite baseline (`bench/sqlite_bench.cc`) runs byte-identical workloads
  through `bench/bench_util.h` — same keys, values, seed, and op mix.
- **Machine:** Apple M2 (8 cores, 8 GB), macOS 15, Apple Clang 21, `-O3
  -DNDEBUG`, no sanitizers. (The `bench` preset is Release.)
- **Data set:** 100,000 keys · 16-byte zero-padded decimal keys (lexicographic
  order == numeric order) · 100-byte deterministic values · fixed seed 42
  (`KVSTORE_BENCH_SEED` overrides).
- **Percentiles** (`p50`/`p99`) are hand-collected: every op inside the timed
  region is wrapped in a `steady_clock` pair and the samples are reduced after
  the run. The ~40 ns clock overhead is byte-identical in both binaries.
- **Reproducibility:** each run writes `environment.txt` (CPU, compiler, seed,
  git rev) next to the raw `*.txt`/`*.json` under
  `docs/bench_results/<date>-<label>/`. Numbers here are the `phase6-final`
  run; `--benchmark_repetitions=3`, aggregates reported as means.
- **Caveat:** unless a row says `sync`, `sync_on_write=false` — throughput
  measures the engine, not the disk's fsync latency. Read benchmarks run at
  two buffer-pool sizes: **64 frames** (256 KB, constant eviction, cold) and
  **8192 frames** (32 MB, whole tree resident, warm).

## PagerDB vs SQLite

SQLite in WAL mode, `WITHOUT ROWID` (clustered B-tree keyed on the blob key,
the same shape as our tree), 32 MB page cache, prepared statements reused,
single connection/autocommit. Throughput in ops/sec (higher is better);
warm = 8192-frame pool.

| Workload | PagerDB | SQLite | Ratio |
|----------|--------:|-------:|------:|
| Sequential load (100k puts) | **303.5k/s** | 58.6k/s | 5.2× |
| Random load (100k puts) | **121.9k/s** | 44.4k/s | 2.7× |
| Sequential load, `sync_on_write` | 32.1k/s | 13.5k/s | 2.4× |
| Point read, uniform, warm | **854.0k/s** | 172.6k/s | 4.9× |
| Point read, Zipfian, warm | **880.8k/s** | 186.6k/s | 4.7× |
| Point read, uniform, cold (64 frames) | 243.8k/s | 172.6k/s* | 1.4× |
| 95% read / 5% write, warm | **542.9k/s** | 173.1k/s | 3.1× |
| Scan-heavy (100-entry ranges) | **20.6M entries/s** | 5.6M/s | 3.7× |
| Delete churn (delete+reinsert) | **160.5k/s** | 56.2k/s | 2.9× |

\* SQLite has no comparable "small cache" knob wired in the harness; its 32 MB
cache holds the whole set, so its read number is the warm one in both rows.

Latency (µs per op, warm where applicable):

| Workload | PagerDB p50 / p99 | SQLite p50 / p99 |
|----------|------------------:|-----------------:|
| Sequential load | 1.0 / 22.8 | 14.2 / 52.5 |
| Random load | 2.6 / 25.0 | 14.8 / 56.9 |
| Load, `sync_on_write` | 39.3 / 323 | 83.0 / 154 |
| Point read, uniform, warm | 1.0 / 2.9 | 5.6 / 7.9 |
| 95/5 mixed, warm | 1.1 / 4.2 | 5.1 / 13.8 |
| Delete churn | 1.3 / 5.1 | 15.4 / 23.3 |

PagerDB comes out ahead across the board, which is the expected shape: it is a
narrow embedded index doing less per operation. **What SQLite does that
PagerDB does not** — and what the gap partly pays for — a SQL parser/planner
(even for prepared statements), full ACID transactions with rollback, secondary
indexes, types and collations, mature concurrency, and decades of portability
and edge-case hardening. The honest read is "a focused B+ tree engine beats a
general-purpose database on the general-purpose database's simplest workload,"
not "PagerDB is faster than SQLite."

Durability caveat on the `sync` rows: `PosixFile::Sync` is `fsync`, which on
macOS flushes to the drive's write cache but **not** through it —
`F_FULLFSYNC` would, at a large slowdown, and is left as a documented Option
(see `DESIGN_DECISIONS.md`). So 32k synced-puts/s is fsync-to-cache, not
power-loss-safe, on this platform.

## Phase 6 optimization progression (PagerDB, warm)

Profile first, one change at a time, re-measured after each (raw runs saved
as `docs/bench_results/2026-07-06-{phase6-baseline,crc32-hw,inplace-insert,phase6-final}`).
The profiler (`macOS sample` on a RelWithDebInfo random-load driver) drove the
*order* — it disagreed with the plan's guess about the hotspot, which is the
whole point of profiling first.

| Workload (ops/sec) | Baseline | +HW CRC-32 | +in-place insert | +WAL append fix (final) | Total |
|--------------------|---------:|-----------:|-----------------:|------------------------:|------:|
| Sequential load | 69.1k | 105.5k | 145.9k | **303.5k** | 4.4× |
| Random load | 25.1k | 74.2k | 84.9k | **121.9k** | 4.9× |
| Point read, uniform, warm | 330.3k | 828.1k | 835.9k | **854.0k** | 2.6× |
| Point read, uniform, cold | 30.9k | 243.4k | 236.6k | **243.8k** | 7.9× |
| 95/5 mixed, warm | 194.4k | 487.2k | 419.8k | **542.9k** | 2.8× |
| Delete churn | 29.2k | 118.0k | 124.9k | **160.5k** | 5.5× |
| Scan-heavy (M entries/s) | 14.9M | 20.3M | 19.1M | **20.6M** | 1.4× |

Run-to-run variation is ~2–4% (coefficient of variation over 3 reps), so the
per-column wobble on the read rows is noise; the load and churn columns move
well outside it.

### 1. CRC-32: byte-at-a-time table → ARMv8 hardware / slicing-by-8

The initial profile put **~55% of random-load wall time** inside the old
table CRC — every WAL page image, every page-write stamp, and every read
verify pays a full 4 KB CRC pass. Replaced with ARMv8 `__crc32b/__crc32d`
instructions (which implement exactly the reflected 0xEDB88320 polynomial, so
stored checksums are unchanged) and a portable slicing-by-8 fallback for
non-ARM builds. Cross-checked bit-identical against the old implementation
over 2000 random buffers plus the standard `CRC32("123456789")==0xCBF43926`
vector. Biggest single win; cold reads 7.9×, random load 3×.

### 2. `NodeView::InsertAt`: rebuild-the-page → in-place slot shift (defect #10)

The old insert copied every record on the page into a `vector<string>` and
rebuilt the page from scratch. `Page::InsertRecordAt` instead shifts the slot
directory by one entry and drops the record at the free-space boundary — O(slots)
memmove of 8-byte slots instead of O(page) record copies plus allocations.
Modest on its own (leaves hold ~30 records, so the rebuild was cheap in
absolute terms — the profile had it at ~5%, *not* the hotspot the plan
guessed), but it removes per-insert heap traffic and is the correct
foundation. One contract change: inserts no longer implicitly compact dead
bytes, so `EnsureFit`/`Compact` is now the sole owner of reclamation
(`node_test` updated to match).

### 3. WAL `AppendTo`: drop the per-record `reserve()` (latent quadratic bug)

Profiling with a raised `max_wal_bytes` (64 MB) to expose the checkpoint-light
path surfaced a real bug: `LogRecord::AppendTo` called
`out.reserve(size + delta)` on every append. Reserving an *exact* capacity
each time defeats `std::string`'s geometric growth, so the WAL buffer
reallocated and copied its entire accumulated contents on nearly every record
— O(n²) in records between checkpoints. At 64 MB it turned a 2 s load into
minutes of `memmove`. Removing the `reserve` (plain `append` grows
geometrically) fixed it: the same 64 MB run dropped to **0.95 s, now faster
than the 4 MB default** because it checkpoints far less often. This is why
the "final" column jumps most on the write-heavy rows.

### Deferred, with rationale

- **memcmp key compare** (planned): already a no-op — libc++'s
  `string_view::compare` is memcmp-based, and the profile confirms
  `_platform_memcmp` at <1%. Dropped.
- **Group commit:** needs the Phase 5 threading/transaction machinery to share
  an fsync window across writers; there is only one writer today.
- **Separator/prefix truncation, `BulkLoad`:** fanout and tree height are not
  the bottleneck — checkpoint I/O is (full-page images make random writes
  checkpoint-bound: ~60% of random-load time is checkpoint fsync/pwrite after
  the CRC fix). Optimizing height would not move the measured numbers.

## Historical: repeat-read microbench (Checkpoint C3, 2026-07-05)

Kept for continuity. 200,000 uniformly random Gets over 10,000 keys
(120-byte values), varying only pool size, before the Phase 6 CRC work:

| Pool frames | Throughput | Pool hits | Misses | Evictions |
|------------:|-----------:|----------:|-------:|----------:|
| 4 | 28,436 gets/sec | 464,436 | 374,478 | 375,197 |
| 1024 | 1,471,599 gets/sec | 838,914 | 0 | 0 |

Cached reads were ~52× the disk-per-descent path even then; the Phase 6 CRC
change widened the warm/cold gap further by making the cold (CRC-verifying)
path cheaper too.
