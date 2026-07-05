# PagerDB — Phased Task Breakdown

Companion to `plan.md`. Each phase lists small, ordered tasks (½–2 h each), the
tests that gate them, the invariants they must preserve, phase-specific risks, and
an exit checkpoint. Work test-first inside each phase where a defect is being
fixed: write the failing test, then the fix.

---

## Phase 0 — Make the repo buildable & testable (prerequisite, ~1 day) ✅ DONE (2026-07-04)

Tasks
- [x] 0.1 Create `tests/` with the three files CMakeLists already registers:
  `types_test.cc`, `file_test.cc`, `page_test.cc` (small real smoke tests, not
  empty stubs), so `-DKVSTORE_BUILD_TESTS=ON` configures and `ctest` runs.
- [x] 0.2 Add `CMakePresets.json`: `debug`, `release`, `asan`
  (`-fsanitize=address,undefined -fno-omit-frame-pointer`), `tsan` (unused until
  Phase 5). Dev presets default `KVSTORE_BUILD_TESTS=ON`; sanitizers wired via a
  `KVSTORE_SANITIZER` cache variable. Required bumping
  `cmake_minimum_required` 3.16 → 3.21 (presets schema v3).
- [x] 0.3 Rename the `kvstore_test` executable target (src/main.cc) to `kvstore_demo`
  to free the name for test binaries.
- [x] 0.4 Add `.clang-format` and `.clang-tidy` (bugprone-*, readability-*); no bulk
  reformat commit mixed with logic changes.
- [x] 0.5 (unplanned, required for C0) Fix two pre-existing compile errors on
  macOS/Apple clang 21: `src/storage/file.cc` used `::fdatasync` (absent on
  macOS; now `fsync` under `#if defined(__APPLE__)`, F_FULLFSYNC deferred to
  Phase 4) and `src/storage/page.cc` was missing `#include <array>`.

Tests: `ctest` runs and passes (trivially) under `debug` and `asan` presets.
→ Verified 2026-07-04: 3/3 tests pass under both `debug` and `asan` presets
  (16 assertions across types/status, PosixFile, and Page smoke tests).

Checkpoint C0 ✅: fresh clone → `cmake --preset asan && ctest --preset asan` green.

---

## Phase 1 — Foundation hardening (format v2, checksums, bug fixes, real tests)

### 1A Format v2 (one batched breaking change)
- 1.1 `page_format.h`: split `PageHeader.reserved` into `PageId rightmost_child;
  uint16_t dead_bytes; uint16_t reserved2;` keep `sizeof == 40` static_assert.
- 1.2 `node.h`: `InitInternal`/`GetRightmostChild`/`SetRightmostChild` use
  `rightmost_child`; internal nodes stop touching `next_page`. Audit
  `SplitInternal`, `InsertInternalAfterSplit`, `CreateNewRoot` for the change.
- 1.3 `pager.h`: `MetaPage` gains `PageId free_list_head;` and `uint32_t checksum;`
  from reserved padding; bump `kFormatVersion` to 2; keep `sizeof == kPageSize`
  assert. Open() on version≠2 → `Corruption`.
- 1.4 Write `docs/FORMAT.md` describing v2 byte-for-byte (header, slot, leaf and
  internal record encodings, meta page).

### 1B Checksums
- 1.5 `page.cc`: implement `uint32_t Page::ComputeChecksum() const` — CRC32 via the
  existing `kCrc32Table`, checksum field treated as zero.
- 1.6 `Pager::WritePage` stamps checksum; `Pager::ReadPage` verifies →
  `Status::Corruption("checksum mismatch page N")`. Same for meta in
  `WriteMetaPage`/`Open`.
- 1.7 `Options::verify_checksums = true` flag threaded from `DB::Open` to Pager.

### 1C Correctness fixes (test-first; these are plan.md defects #1, #2, #3, #6, #7)
- 1.8 Unify descent: make `FindLeafPath` use the exact `FindChild` comparison
  (`key < GetKey(i)` → child i, else next; equal goes right). Failing test first:
  put keys until a split, then Put a key equal to the separator, Get it back.
- 1.9 Fix `SplitLeaf` to preserve `prev_page` on the left page. Test: three-leaf
  chain, split middle, walk chain both directions.
- 1.10 Maintain `meta.num_keys`: increment on new-key Put (Pager::SetNumKeys or a
  batched meta update); test `ApproximateKeyCount()`.
- 1.11 Replace every `std::ignore = pager_.WritePage/ReadPage` in btree.cc with
  checked propagation (`Status`/early return).
- 1.12 Remove `pager_.Sync()` calls from `BPlusTree::Put`; `DbImpl` is the sole
  sync policy owner (respecting `sync_on_write`). Test: `sync_on_write=false`
  performs no fsync (count syncs via a test hook or PosixFile stat).

### 1D Validation & tooling
- 1.13 `Page::Validate()` per plan.md §6 page invariants; called after mutating
  NodeView ops under `#ifndef NDEBUG`.
- 1.14 `Page::Dump(std::ostream&)` — header, slots, decoded records by type.
- 1.15 `tools/kvstore_inspect.cc` (+ CMake target): open db read-only, print meta,
  recursive tree structure, leaf-chain walk asserting global key order.

### 1E Test suite (bulk of the phase; files under `tests/`)
- 1.16 `file_test.cc`: open create/no-create, NotFound, read/write round-trip,
  short read at EOF, Size, move ctor/assign leaves source closed, double move.
- 1.17 `page_test.cc`: Init, insert-until-`NoSpace`, byte-exact FreeSpace
  accounting, 0-byte value, max-size record, DeleteRecord tombstone, Validate.
- 1.18 `pager_test.cc`: create→valid meta; reopen preserves root/num_pages;
  corrupt magic / wrong version / truncated file / flipped byte (checksum) each
  yield `Corruption`; AllocatePage sequential and persistent.
- 1.19 `node_test.cc`: sorted insert order, FindKeyIndex on empty/first/last/
  missing/equal, leaf & internal record round-trips, UpdateLeaf same/smaller/
  larger value, rightmost_child handling.
- 1.20 `btree_test.cc`: single key; insert to first split; multi-level split;
  Get-after-split for every key; duplicate key updates; separator-equal Put/Get
  (regression for 1.8); leaf chain ordered both directions (regression for 1.9).
- 1.21 `btree_property_test.cc`: N=10k seeded random keys vs `std::map` — every
  Get matches, full leaf-chain scan equals sorted reference.

Invariants introduced: all of plan.md §6 "Page" and "Tree" invariants.

Risks: v2 change touches header semantics everywhere (mitigate: 1A lands as one
commit with tests); `Wconversion` noise when touching uint16 arithmetic.

Checkpoint C1: full suite green under `asan`; `kvstore_inspect` renders a 3-level
tree; `docs/FORMAT.md` committed; tag `v0.1-phase1`.

---

## Phase 2 — Delete, compaction, free list, iterators

### 2A Leaf delete
- 2.1 `NodeView::DeleteAt(uint16_t index)`: remove slot (shift slot array), leave
  record bytes dead, `dead_bytes += record length + slot size`.
- 2.2 `BPlusTree::Delete(key)`: FindLeafPath → FindKeyIndex → DeleteAt; `NotFound`
  if absent; decrement `num_keys`.
- 2.3 `DbImpl::Delete`: call through; honor `sync_on_write`.

### 2B In-page compaction
- 2.4 `Page::Compact()`: copy live records back-to-front into a scratch `Page`,
  rewrite slot offsets, reset `free_space_*`, zero `dead_bytes`.
- 2.5 Trigger in `InsertLeaf`/`InsertInternal`: if `!CanFit` but
  `FreeSpace() + dead_bytes` suffices → Compact, retry. (Note: today's
  rebuild-based InsertAt already compacts implicitly; keep semantics identical.)

### 2C Free list
- 2.6 `Pager::FreePage(PageId)`: page re-typed `kFree`, first 4 payload bytes =
  old `free_list_head`, meta head updated.
- 2.7 `Pager::AllocatePage()`: pop free list if nonempty, else extend (current
  path).
- 2.8 Decision (documented in plan.md): empty leaves are freed and unlinked from
  the sibling chain, but their separator stays in the parent pointing at… — pick
  the simpler documented option: **do not free empty leaves yet**; free list is
  exercised by page-churn tests and later by Phase 4/6 machinery. Underflow
  merge (2E) is the real consumer.

### 2D Cursor / scan
- 2.9 `include/kvstore/btree/cursor.h`: `Valid/Next/key/value`, holds PageId +
  slot index, follows `next_page` at node end, skips tombstoned slots.
- 2.10 `BPlusTree::Seek(key)` (lower bound) and `SeekToFirst()`.
- 2.11 `DB::Scan(start, end, callback)` on the public API; empty `end` = to end.

### 2E (Stretch) Underflow handling
- 2.12 Borrow from sibling; else merge + remove separator from parent, recurse;
  shrink root when a lone-child internal root remains. Only start if C2 gate is
  otherwise green.

Tests
- `node_test`: DeleteAt slot shifting, dead_bytes accounting, Compact round-trip.
- `btree_test`: delete existing/missing, delete-then-get, delete-then-reinsert,
  delete across split boundary.
- `pager_test`: Free→Allocate reuses the page; free-list survives reopen.
- `cursor_test` (new): full scan == sorted reference; bounded scan; scan across
  splits; scan after deletes skips tombstones.
- `btree_property_test` extended: random interleaved Put/Delete/Scan vs `std::map`
  (seeded, ≥50k ops).

Invariants added: `dead_bytes` ≤ payload area and consistent with tombstoned
slots; free-list pages typed `kFree`, acyclic, unreachable from root; scan output
strictly ascending, no tombstones.

Risks: Compact and rebuild-based InsertAt interacting (double bookkeeping of
dead_bytes — define exactly one owner of the counter); dangling separators if 2.8
decision drifts.

Checkpoint C2: property suite with deletes green under asan; file size stops
growing under a put/delete churn workload (free-list demonstrably reused);
tag `v0.2-phase2`.

---

## Phase 3 — Buffer pool

### Tasks
- 3.1 `include/kvstore/storage/buffer_pool.h` + `src/storage/buffer_pool.cc`:
  `Frame {Page, PageId, pin_count, dirty}`, page table
  (`unordered_map<PageId, FrameId>`), free-frame list;
  `FetchPage/NewPage → Result<PageGuard>`, `FlushPage`, `FlushAll`;
  pool size in `Options` (frames).
- 3.2 `include/kvstore/storage/page_guard.h`: move-only RAII guard; `page()`,
  `MarkDirty()`, `Drop()`; destructor unpins.
- 3.3 `include/kvstore/storage/lru_replacer.h`: map + intrusive list;
  `RecordAccess`, `Evict() → optional<FrameId>` (unpinned only). Dirty eviction
  writes back via Pager (checksum stamped there, as in Phase 1).
- 3.4 Refactor `BPlusTree` to take `BufferPool&`; every `Page page; ReadPage(...)`
  → `auto guard = pool_.FetchPage(id)`; every `WritePage` → `guard.MarkDirty()`.
  BufferPool exposes `pager()` for meta ops (root id, num_keys).
- 3.5 `DbImpl::Sync` → `pool.FlushAll(); pager.Sync()`; `sync_on_write` unchanged
  in meaning.
- 3.6 Audit guard lifetimes in split paths: leaf split holds leaf + new + old-next
  (3 guards) plus one parent per level. Document and assert minimum pool size
  (`tree depth + 3`); return `Busy` when all frames pinned.
- 3.7 Hit/miss/eviction counters; `DB::GetStats()` (small struct) for Phase 6.

Tests (`buffer_pool_test.cc` + regression)
- Fetch same page twice → one frame, pin_count 2; guard drop unpins.
- Fill pool, pinned page survives eviction pressure; all-pinned → `Busy`.
- Dirty eviction persists (verify via direct `Pager::ReadPage`).
- Guard move semantics; `Drop()` early release.
- **Regression gate:** entire Phase 1–2 suite runs with pool sizes {4, 16, 1024}
  (parameterized via Options/env).

Invariants added: a frame's contents may be replaced only when `pin_count == 0`;
dirty frame never discarded without write-back; page table ↔ frames bijective.

Risks: hidden aliasing (two guards to the same page mutating via NodeView is fine —
same frame — but stale local `Page` copies are now bugs); this refactor rewrites
most of btree.cc — do it as small commits per function with the suite green between.

Checkpoint C3: full suite green at pool size 4 under asan; repeat-read microbench
shows measurable speedup vs Phase 2 (record the number in `docs/BENCHMARKS.md` as
the first data point); tag `v0.3-phase3`.

---

## Phase 4 — WAL & crash recovery (format v3)

### Tasks
- 4.1 `include/kvstore/wal/log_record.h`:
  `[lsn:8][txid:8][type:1][key_len:2][val_len:2][key][value][crc:4]`;
  types `kPut, kDelete, kAllocPage, kFreePage, kRootChange, kCheckpoint`
  (txid written as 0 until Phase 5). Serialize/Deserialize + CRC; a torn/invalid
  tail record means clean end-of-log.
- 4.2 `wal/log_writer.{h,cc}`: append-only `wal.log` in the db directory; buffered
  `Append(record) → Lsn` (LSN = byte offset, matching `types.h` comment);
  `FlushTo(lsn)` → fdatasync.
- 4.3 `DbImpl::Put/Delete`: append record → (if `sync_on_write`) flush log → apply
  to tree through buffer pool. No data-page write on the operation path anymore.
- 4.4 Stamp `page_lsn` on every logged mutation (the header field finally earns
  its 8 bytes).
- 4.5 **The write-ahead rule:** before BufferPool writes any frame (eviction or
  FlushAll), call `log_writer.FlushTo(frame.page.page_lsn)`. One line; test it
  explicitly with a fault hook.
- 4.6 Dual meta pages (v3): meta at pages 0 and 1 with `uint64_t generation`,
  written alternately; open picks the checksum-valid page with higher generation;
  data starts at page 2. Bump `kFormatVersion` to 3; update `kvstore_inspect`.
- 4.7 `wal/log_reader.{h,cc}` + recovery in `DB::Open`: scan from
  `meta.checkpoint_lsn`; redo iff `record.lsn > page.page_lsn` (idempotent).
- 4.8 `DbImpl::Checkpoint()`: FlushAll → Sync data → append `kCheckpoint` → update
  meta checkpoint_lsn → truncate/rotate log. Manual trigger + size threshold in
  `Options` (`max_wal_bytes`).
- 4.9 Fault injection: `PosixFile::FailAfterNWrites(n)` behind a test-only hook.
- 4.10 `tests/crash_test.cc`: fork-based — child does seeded random Puts/Deletes,
  `_exit(1)` at a random point; parent reopens, verifies every key whose commit
  (log flush) completed is present, tree passes full validation. ≥500 seeded
  iterations (short mode for CI, long mode manual).

Tests: log record round-trip + CRC/torn-tail cases; WAL-rule unit test (evicting a
dirty page forces log flush first); recovery replays exactly-once (idempotence:
recover twice → same state); checkpoint truncates log and bounds recovery time;
dual-meta: corrupt either meta copy → open still succeeds on the other.

Invariants added: plan.md §6 WAL rule; after recovery the tree checker passes and
state equals "all records up to last flushed LSN applied, nothing after".

Risks: allocation records vs redo ordering (a redo may target a page beyond
current `num_pages` — `kAllocPage` redo must extend the file first); macOS
`fdatasync` semantics (use `fcntl(F_FULLFSYNC)` note in docs, keep it a
build-time/Options choice since it is drastically slower).

Checkpoint C4: 500-run crash harness green; kill-mid-AllocatePage regression test
(defect #9) passes; recovery-time bound demonstrated with checkpoints;
tag `v0.4-phase4`.

---

## Phase 5 — Concurrency & transactions (each sub-step ships independently)

- 5.1 Thread-safe buffer pool: mutex on page table/free list; per-frame
  `std::shared_mutex`; `FetchPageRead/FetchPageWrite` guard variants. Activate
  `tsan` preset; multithreaded fetch/unpin stress test.
- 5.2 Latch crabbing in the tree: pessimistic-only first (exclusive path locks on
  writes), then optimistic release-when-safe (`CanFit`/`CanFitInternal`);
  pool-level `root_latch` protecting `root_page_id`. Stress: 8 threads mixed
  put/get/scan vs per-thread expected sets, under tsan.
- 5.3 `txn/` module: `TxnManager::Begin/Commit/Abort`; real txids in log records;
  `kBegin/kCommit/kAbort` types; durability = commit record flushed; autocommit
  wraps existing API; group commit deferred to Phase 6.
- 5.4 Undo: Put/Delete records carry before-images; Abort walks backward applying
  inverses; recovery becomes redo-all + undo-losers. Crash harness extended:
  uncommitted keys must be absent after recovery.
- 5.5 Strict 2PL `LockManager`: per-key S/X locks, wait-die (or 1 s timeout →
  `Busy`); release at commit/abort. Anomaly tests: dirty read, lost update,
  write-write conflict.
- 5.6 (Stretch) MVCC: snapshot-isolation reads + 2PL writes; version chain
  newest-in-page with in-memory undo map; GC past oldest snapshot.

Invariants added: no data race reported by tsan across the whole suite; committed
== durable (commit record flushed); after Abort, no trace of the txn's writes.

Risks: latch/lock ordering deadlocks (define global order: root_latch → tree path
top-down → lock manager); recovery interplay with undo (loser txns spanning
checkpoints).

Checkpoint C5: tsan-clean stress suite; anomaly tests demonstrate the isolation
level; crash harness green with concurrent writers; tag `v0.5-phase5`.

---

## Phase 6 — Performance & portfolio polish

- 6.1 `bench/` with Google Benchmark: sequential load, random load, uniform +
  Zipfian read-only, 95/5 mixed, scan-heavy, delete churn; ops/sec + hand-collected
  p50/p99. Baselines: SQLite (WAL mode) and/or LMDB, with a written explanation of
  gaps.
- 6.2 Profile-driven fixes, one at a time, re-measured: replace rebuild-based
  `InsertAt` with true in-place slot shift (defect #10); kill `std::string`
  returns in `EncodeLeafRecord` (encode into stack/page buffer); memcmp-based key
  compare; internal-node separator truncation (measure fanout/height); group
  commit (shared fdatasync window); `DB::BulkLoad(sorted_range)` building leaves
  left-to-right.
- 6.3 CI: GitHub Actions matrix (gcc/clang × debug-asan/release), tests + short
  crash-harness run; README badge.
- 6.4 Docs: `ARCHITECTURE.md` (layers, lifecycles for Get/Put/Commit/Recovery),
  finalized `FORMAT.md` (v3), `DESIGN_DECISIONS.md` (choices table from plan.md
  §4 with outcomes), `BENCHMARKS.md` with charts; README demo: load 1M keys →
  `kill -9` → reopen → verify → benchmark table.

Checkpoint C6: one-command bench run reproduces the README table; every plan.md §4
decision has a written rationale; tag `v0.6`.

---

## Cross-phase rules

- A phase is done at its checkpoint, not when its last feature lands; stretch
  items never block a checkpoint.
- Never mix a format bump, a refactor, and a feature in one commit.
- Every defect fix (plan.md §1 list) lands with the regression test that would
  have caught it.
- `kvstore_inspect` and the tree validator are updated in the same commit as any
  format or structural change.
