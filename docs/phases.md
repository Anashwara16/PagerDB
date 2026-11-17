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

## Phase 1 — Foundation hardening (format v2, checksums, bug fixes, real tests) ✅ DONE (2026-07-05)

All work uncommitted in the working tree by explicit instruction (no git
commits during this phase); the C1 tag is therefore deferred.

### 1A Format v2 (one batched breaking change) ✅
- [x] 1.1 `page_format.h`: split `PageHeader.reserved` into `PageId rightmost_child;
  uint16_t dead_bytes; uint16_t reserved2;` keep `sizeof == 40` static_assert.
- [x] 1.2 `node.h`: `InitInternal`/`GetRightmostChild`/`SetRightmostChild` use
  `rightmost_child`; internal nodes stop touching `next_page`. Audited
  `SplitInternal`, `InsertInternalAfterSplit`, `CreateNewRoot`; `InsertAt`'s
  page rebuild now preserves `rightmost_child`.
- [x] 1.3 `pager.h`: `MetaPage` gains `free_list_head` + `checksum`;
  `kFormatVersion = 2`; Open() on version≠2 → `Corruption`.
- [x] 1.4 `docs/FORMAT.md` describes v2 byte-for-byte.

### 1B Checksums ✅
- [x] 1.5 `page.cc`: `Page::ComputeChecksum()` + shared `Crc32WithZeroedField()`
  (also used for the meta page) + `Page::IsZeroed()` (all-zero pages exempt —
  allocated but never written).
- [x] 1.6 `Pager::WritePage` stamps (signature now takes `Page&`);
  `Pager::ReadPage` verifies → `Corruption("Checksum mismatch on page N")`;
  meta stamped in `WriteMetaPage`/creation, verified in `Open`. Fixed move
  ctor/assign dropping `verify_checksums_`.
- [x] 1.7 `Options::verify_checksums = true` threaded `DB::Open` → `Pager::Open`.

### 1C Correctness fixes (test-first) ✅
- [x] 1.8 Unified descent via new `NodeView::FindChildIndex()` used by both
  `FindChild` (Get) and `FindLeafPath` (Put); equal keys descend right.
  Regression: `BTreeTest.UpdateEveryKeyAfterSplitsIsVisibleToGet` (watched fail,
  then pass).
- [x] 1.9 `SplitLeaf` preserves the left page's `prev_page`. Regression:
  `BTreeTest.LeafChainIsConsistentInBothDirections` (watched fail, then pass).
- [x] 1.10 `meta.num_keys` incremented on the three fresh-insert paths of Put
  (empty tree, plain insert, post-split); updates don't count. Regression:
  `BTreeTest.NumKeysCountsUniqueKeysNotUpdates` + `DbTest.ApproximateKeyCount…`.
- [x] 1.11 All `std::ignore = pager_.ReadPage/WritePage` in btree.cc replaced
  with checked propagation; split-rebuild insert failures now surface as
  `Internal` instead of silently corrupting.
- [x] 1.12 `BPlusTree::Put` no longer syncs; `DbImpl` owns the policy. Test hook
  `PosixFile::TotalSyncs()`; regression `DbTest.NoSyncsWhenSyncOnWriteDisabled`
  (watched fail, then pass) + `DbTest.SyncOnWriteIssuesSyncPerPut`.

### 1D Validation & tooling ✅
- [x] 1.13 `Page::Validate()` (slot-directory/free-space consistency, records in
  bounds, no overlap); `assert(page_.Validate())` after NodeView rebuilds.
- [x] 1.14 `Page::Dump(std::ostream&)` — header + slot directory summary.
- [x] 1.15 `tools/kvstore_inspect.cc` + CMake target: meta dump, recursive tree
  render, leaf-chain forward/back-link + key-order verification; accepts db dir
  or data.db path; exit code reflects integrity.

### 1E Test suite ✅ (8 binaries, all green under `asan` and `debug`)
- [x] 1.16 `file_test.cc`: + short read at EOF, Truncate shrink/extend, move
  assignment, TotalSyncs counter (9 tests).
- [x] 1.17 `page_test.cc`: + Validate accept/reject cases, Dump summary (12 tests).
- [x] 1.18 `pager_test.cc`: meta init/reopen/allocation + corrupt magic, wrong
  version, truncation, flipped byte in data/meta page, verification-off mode,
  zero-page exemption (11 tests).
- [x] 1.19 `node_test.cc`: sorted order, FindKeyIndex edges, empty-value
  round-trip, UpdateLeaf same/smaller/larger, internal descent incl.
  equal-goes-right, rightmost_child non-aliasing (8 tests).
- [x] 1.20 `btree_test.cc`: basics, 200-key splits, separator-equal update
  regression, chain both directions, num_keys (7 tests).
- [x] 1.21 `btree_property_test.cc`: 10k seeded random Puts vs `std::map`; every
  Get matches, misses stay missing, num_keys == unique count, full leaf-chain
  scan equals sorted reference (1 test, ~8s under asan).

Invariants introduced: all of plan.md §6 "Page" and "Tree" invariants.

Risks: v2 change touches header semantics everywhere (mitigate: 1A lands as one
commit with tests); `Wconversion` noise when touching uint16 arithmetic.

Checkpoint C1 ✅ (2026-07-05): full suite green under `asan` (8/8) and `debug`
(8/8); `kvstore_inspect` rendered a real 3-level tree (2000 keys → root + 19
internals + 333 leaves, chain consistent, exit 0, under asan); `docs/FORMAT.md`
written. Tag `v0.1-phase1` NOT created — user instructed no git commits; tag
when the phase is eventually committed.

---

## Phase 2 — Delete, compaction, free list, iterators ✅ DONE (2026-07-05)

All work uncommitted in the working tree by explicit instruction (no git
commits); the C2 tag is therefore deferred, same as C1.

### 2A Leaf delete
- [x] 2.1 `NodeView::DeleteAt(uint16_t index)`: remove slot (shift slot array),
  leave record bytes dead. Deviation: `dead_bytes += record length` only —
  slot bytes are reclaimed immediately by the shift, so counting them would
  overstate reclaimable space (documented in FORMAT.md).
- [x] 2.2 `BPlusTree::Delete(key)`: FindLeaf → FindKeyIndex → DeleteAt;
  `NotFound` if absent; decrements `num_keys`. (Path not needed until 2E.)
- [x] 2.3 `DbImpl::Delete`: calls through; syncs only on successful delete
  when `sync_on_write` is set; NotFound propagates (db.h doc updated).

### 2B In-page compaction
- [x] 2.4 `Page::Compact()`: scratch-page rebuild, live records packed
  back-to-front, tombstoned slots cleared, freed region zero-filled,
  `dead_bytes` reset. `Page::DeleteRecord` now counts dead bytes too.
- [x] 2.5 `NodeView::EnsureFit(needed)` gates `InsertLeaf`/`InsertInternal`:
  compact-and-retry when `FreeSpace() + dead_bytes` covers the shortfall.
  Bonus fix found by the 50k property test: an update whose value outgrows a
  full leaf now deletes the old record and reinserts (splitting if needed,
  `num_keys` unchanged) instead of failing with Internal.

### 2C Free list
- [x] 2.6 `Pager::FreePage(PageId)`: page re-typed `kFree` (checksummed), first
  4 payload bytes = old head; rejects page 0 and out-of-range ids.
- [x] 2.7 `Pager::AllocatePage()`: LIFO pop with corruption check on the head's
  type; recycled pages re-zeroed on disk; extends only when the list is empty.
- [x] 2.8 Decision (documented in plan.md): empty leaves are freed and unlinked from
  the sibling chain, but their separator stays in the parent pointing at… — pick
  the simpler documented option: **do not free empty leaves yet**; free list is
  exercised by page-churn tests and later by Phase 4/6 machinery. Underflow
  merge (2E) is the real consumer.

### 2D Cursor / scan
- [x] 2.9 `cursor.h`/`cursor.cc`: `Valid/Next/key/value/status`, private Page
  copy + index, follows `next_page`, skips emptied leaves (no tombstoned slots
  exist — deletes shift the directory). I/O errors surface via `status()`.
- [x] 2.10 `BPlusTree::Seek(key)` (lower bound) and `SeekToFirst()`.
- [x] 2.11 `DB::Scan(start, end, callback)`: half-open `[start, end)`; empty
  start = from first key, empty end = to last; callback false stops early.

### 2E (Stretch) Underflow handling — NOT STARTED (deliberately out of scope)
- [ ] 2.12 Borrow from sibling; else merge + remove separator from parent, recurse;
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

Checkpoint C2 ✅ (2026-07-05): full suite green under `asan` (9/9, 42s — the
50k-op interleaved property test alone takes ~26s) and `debug` (9/9);
`btree_test` churn test proves num_pages stays constant over 5 full
delete/reinsert rounds; `kvstore_inspect` (now with a free-list walk:
typed-kFree, acyclic, disjoint-from-tree checks) exits 0 on a churned
1500-insert/540-delete DB (960 keys, 166 leaves incl. 32 emptied ones) and on
a Pager-level free-list demo (3 free pages). Free-list reuse is demonstrated
by `pager_test` — the tree itself never frees pages per decision 2.8. Tag
`v0.2-phase2` NOT created — user instructed no git commits; tag when the
phase is eventually committed.

---

## Phase 3 — Buffer pool ✅ DONE (2026-07-05)

All work uncommitted in the working tree by explicit instruction (no git
commits); the C3 tag is therefore deferred, same as C1/C2.

### Tasks
- [x] 3.1 `include/kvstore/storage/buffer_pool.h` + `src/storage/buffer_pool.cc`:
  `Frame {Page, PageId, pin_count, dirty}`, page table
  (`unordered_map<PageId, FrameId>`), free-frame list;
  `FetchPage/NewPage → Result<PageGuard>`, `FlushPage`, `FlushAll`;
  pool size in `Options` (frames).
- [x] 3.2 `include/kvstore/storage/page_guard.h`: move-only RAII guard; `page()`,
  `MarkDirty()`, `Drop()`; destructor unpins.
- [x] 3.3 `include/kvstore/storage/lru_replacer.h`: map + intrusive list;
  `RecordAccess`, `Evict() → optional<FrameId>` (unpinned only). Dirty eviction
  writes back via Pager (checksum stamped there, as in Phase 1).
- [x] 3.4 Refactor `BPlusTree` to take `BufferPool&`; every `Page page; ReadPage(...)`
  → `auto guard = pool_.FetchPage(id)`; every `WritePage` → `guard.MarkDirty()`.
  BufferPool exposes `pager()` for meta ops (root id, num_keys).
- [x] 3.5 `DbImpl::Sync` → `pool.FlushAll(); pager.Sync()`; `sync_on_write` unchanged
  in meaning.
- [x] 3.6 Audit guard lifetimes in split paths: leaf split holds leaf + new + old-next
  (3 guards) plus one parent per level. Document and assert minimum pool size
  (`tree depth + 3`); return `Busy` when all frames pinned.
- [x] 3.7 Hit/miss/eviction counters; `DB::GetStats()` (small struct) for Phase 6.

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

Checkpoint C3 ✅ (2026-07-05): full suite (10 binaries incl. new
`buffer_pool_test`, 8 tests) green under asan at KVSTORE_POOL_FRAMES=4, 16,
and 1024, and under debug; Cursor reads via the pool (copies the page out
while the guard is held, so scans see dirty frames and hold no pins);
`Options::buffer_pool_frames` (0 = env `KVSTORE_POOL_FRAMES` or 64;
clamped to BufferPool::kMinFrames = 4); DbImpl::Sync = FlushAll + fsync,
also used by sync_on_write Put/Delete; `DB::GetStats()` exposes
hits/misses/evictions. Repeat-read microbench: 28.4k gets/sec (4 frames,
thrashing ≈ Phase 2 path) vs 1.47M gets/sec (fully cached) — ~52x, recorded
in `docs/BENCHMARKS.md`. Notes: split paths hold ≤3 guards + 1/level
(documented in buffer_pool.h); test fixtures FlushAll before raw Pager
reads. Tag `v0.3-phase3` NOT created — user instructed no git commits.

---

## Phase 4 — WAL & crash recovery (format v3) ✅ DONE (2026-07-06)

All work uncommitted in the working tree by explicit instruction (no git
commits); the C4 tag is therefore deferred, same as C1-C3.

### Tasks
- [x] 4.1 `include/kvstore/wal/log_record.h`:
  `[lsn:8][txid:8][type:1][key_len:2][val_len:2][key][value][crc:4]`;
  types `kPut, kDelete, kAllocPage, kFreePage, kRootChange, kCheckpoint`
  (txid written as 0 until Phase 5). Serialize/Deserialize + CRC; a torn/invalid
  tail record means clean end-of-log.
- [x] 4.2 `wal/log_writer.{h,cc}`: append-only `wal.log` in the db directory; buffered
  `Append(record) → Lsn` (LSN = byte offset, matching `types.h` comment);
  `FlushTo(lsn)` → fdatasync.
- [x] 4.3 `DbImpl::Put/Delete`: append record → (if `sync_on_write`) flush log → apply
  to tree through buffer pool. No data-page write on the operation path anymore.
- [x] 4.4 Stamp `page_lsn` on every logged mutation (the header field finally earns
  its 8 bytes).
- [x] 4.5 **The write-ahead rule:** before BufferPool writes any frame (eviction or
  FlushAll), call `log_writer.FlushTo(frame.page.page_lsn)`. One line; test it
  explicitly with a fault hook.
- [x] 4.6 Dual meta pages (v3): meta at pages 0 and 1 with `uint64_t generation`,
  written alternately; open picks the checksum-valid page with higher generation;
  data starts at page 2. Bump `kFormatVersion` to 3; update `kvstore_inspect`.
- [x] 4.7 `wal/log_reader.{h,cc}` + recovery in `DB::Open`: scan from
  `meta.checkpoint_lsn`; redo iff `record.lsn > page.page_lsn` (idempotent).
- [x] 4.8 `DbImpl::Checkpoint()`: FlushAll → Sync data → append `kCheckpoint` → update
  meta checkpoint_lsn → truncate/rotate log. Manual trigger + size threshold in
  `Options` (`max_wal_bytes`).
- [x] 4.9 Fault injection: `PosixFile::FailAfterNWrites(n)` behind a test-only hook.
- [x] 4.10 `tests/crash_test.cc`: fork-based — child does seeded random Puts/Deletes,
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

Documented deviations (rationale in FORMAT.md and log_record.h):
- Records gained a `page_id` field and a `kPageImage` type: physiological
  redo (one record = one page, `redo iff lsn > page_lsn`) — purely logical
  records cannot replay safely into a partially flushed multi-page split.
- Op-end flag (type-byte high bit): recovery applies op groups
  all-or-nothing; a mid-op WAL flush followed by a crash must not replay
  half a structural change (found by the fault-injected recovery test).
- Full-page writes: first mutation of a page after a checkpoint logs a full
  image, so a torn data-page write (half-old/half-new, checksum-dead) is
  reconstructed from the log (found by crash-harness run 5, seed 20260710).
- Record emission lives in BPlusTree (not DbImpl as sketched in 4.3): only
  the tree knows which pages an op touched. DbImpl still owns the flush
  policy (sync_on_write) and checkpoints. Write-ahead ordering is enforced
  in the BufferPool (task 4.5) and applies to in-memory application vs
  page writes, not vs in-memory mutation.
- Redo-only limitation until Phase 5 undo: a write op failing mid-flight
  poisons the handle (further writes get Internal); reopen recovers the
  last consistent state.

Checkpoint C4 ✅ (2026-07-06): 500-run fork crash harness green under asan
(seeded torn-write faults firing mid-split/mid-checkpoint/mid-WAL-flush;
child `_exit`s with no shutdown checkpoint; parent verifies committed prefix
+ single in-flight ambiguity + sorted-scan structural sanity);
kill-mid-AllocatePage regression (defect #9) covered by
recovery_test.CrashMidAllocationHeavySplitsRecoversConsistently (fault sweep
at 5 depths) plus the harness; recovery-time bound demonstrated by
recovery_test.CheckpointsBoundTheWalDuringAWorkload (64 KB max_wal_bytes:
WAL never exceeded 128 KB across 2000 puts, checkpoint empties it); full
suite 13/13 green under asan and debug. Tag `v0.4-phase4` NOT created —
user instructed no git commits.

---

## Phase 5 — Concurrency & transactions ✅ DONE (2026-07-08)

All work uncommitted in the working tree by explicit instruction (no git
commits); the C5 tag is therefore deferred, same as C1-C4/C6.

- [x] 5.1 Thread-safe buffer pool: one pool mutex (page table/free list/replacer/
  pins/stats); per-frame `std::shared_mutex` content latches;
  `FetchPageRead/FetchPageWrite` guard variants (`FetchPage` kept, unlatched,
  for recovery and old tests). `tsan` preset active;
  `buffer_pool_concurrent_test` fetch/unpin/evict stress.
- [x] 5.2 Latch crabbing: reads crab with shared latches (root_latch shared just
  long enough to latch the root); deletes exclusive-couple releasing each
  parent immediately (never split); Puts try an OPTIMISTIC fast path first —
  shared-latch descent with root_latch held shared (every split runs under an
  exclusive root_latch, so a shared hold freezes the tree shape), exclusive
  latch on the leaf only, falling back to the pessimistic full-exclusive-path
  descent only when the leaf can't absorb the write. Deviation from the task
  sketch: the fallback trigger is "InsertLeaf/UpdateLeaf failed" rather than
  `CanFit`/`CanFitInternal` — CanFit ignores reclaimable dead bytes and split
  keys have unknowable lengths at descent time, so try-then-restart is both
  simpler and more precise. Stress: 8 threads mixed put/get/scan/delete vs
  per-thread `std::map` models (`btree_concurrent_test`), plus full-DbImpl
  concurrent autocommit/transaction suites (`db_concurrent_test`), under tsan.
- [x] 5.3 `txn/` module: `TxnManager` (monotone txids; wait-die age order),
  `DB::Begin() → Transaction` (Get/Put/Delete/Commit/Abort; destructor
  aborts); real txids in every WAL record; `kBegin/kCommit/kAbort` types
  (kBegin logged lazily at first write); durability = commit record flushed;
  autocommit ops are single-op txns (fresh txid, no markers needed); recovery
  groups interleaved records by (txid, op_end) instead of byte-contiguity.
  Group commit deferred to future work (needs contention data first).
- [x] 5.4 Undo: kPut/kDelete records carry before-images (`before_len` field;
  0xFFFF = key was absent); Abort walks the in-memory undo list backward
  applying logged inverses (rollback is itself durable); recovery = repeat-
  history redo + undo-losers (a txn with kBegin but no kCommit/kAbort),
  newest-first, under a fresh committed txid so a crash mid-undo converges.
  Crash harness extended: deterministic loser-txn test + seeded txn-stream
  runs (`SeededTransactionCrashesUndoLosers`) — uncommitted keys absent after
  recovery, committed prefix intact.
- [x] 5.5 Strict 2PL `LockManager`: per-key S/X locks (one table mutex + condvar;
  wait-die on transaction age, 1 s timeout backstop → `Busy`); S→X upgrade when
  sole holder; acquired by Transaction Get (S) / Put / Delete (X), all held until
  `ReleaseAll` at Commit/Abort (also on a poisoned mid-txn failure — a leaked
  lock starves every later txn on the key). Age is a `priority` separate from
  the txid: `DB::Begin(retry_of = txn->id())` lets a wait-die victim retry
  under its ORIGINAL age (fresh WAL txid — reusing it would break recovery's
  loser detection), which is what makes wait-die starvation-free; a plain
  `Begin()` retry would restart ever-younger and could die forever. Isolation
  level stated precisely (in db.h and DESIGN_DECISIONS — "strict 2PL" alone
  overpromises): explicit txns are serializable AGAINST EACH OTHER for POINT
  operations only; no phantom protection (locks cover key names, not ranges);
  Scan is read-uncommitted; autocommit ops take no key locks and ignore txn
  boundaries entirely (an autocommit write on an X-locked key is erased by
  that txn's rollback — keep the key spaces disjoint). Anomaly tests in
  `txn_test`: dirty read,
  lost update, write-write conflict, delete-vs-write conflict,
  older-waits/younger-dies, timeout, retry-inherits-age (watched fail→pass:
  without inheritance the retry dies against a newer holder), plus LockManager
  unit tests. Regression (watched fail→pass): sequential txns on one key must
  not conflict — caught the ReleaseAll-never-called leak.
- [ ] 5.6 (Stretch) MVCC: snapshot-isolation reads + 2PL writes — NOT STARTED
  (deliberately out of scope, per cross-phase rules; 2PL gives the isolation
  story, MVCC is a documented future direction).

Invariants added: no data race reported by tsan across the whole suite; committed
== durable (commit record flushed); after Abort, no trace of the txn's writes;
locks held exactly for the transaction's lifetime (strict 2PL).

Checkpoints are fuzzy w.r.t. transactions (post-C5 fix): the checkpoint latch
is taken per OPERATION, never for a transaction's lifetime, so an idle open
transaction blocks nothing (before: one forgotten handle deadlocked every
checkpoint → unbounded log growth). A gate in front of the shared side parks
new writers while a checkpoint is pending — std::shared_mutex promises no
writer preference, so sustained autocommit traffic could otherwise starve the
checkpointer forever. What an in-flight (begun) transaction still pins is the
log TAIL: checkpoints flush pages but skip WAL truncation while
active_begun_txns_ > 0, because steal may have put its uncommitted pages on
disk and the log holds the only undo. Bound: log growth ≤ transaction
lifetime. Future work: partial truncation up to the oldest active kBegin LSN.
A poisoned handle refuses checkpoints outright (flushing — and truncating the
log out from under — a possibly half-applied tree would make the damage
permanent; reopen + recovery is the repair path).

Lock order (deadlock freedom, enforced by construction): lock manager (key
locks first — may block on wait-die, so never while holding a latch) →
checkpoint gate → checkpoint_mu_ (bounded per-op shared holds) → root_latch →
page latches top-down (siblings rightward) → pool latch_ → WAL writer mutex →
pager meta mutex.

Checkpoint C5 ✅ (2026-07-08): full suite 17/17 green under tsan (no races),
asan, and debug; anomaly tests demonstrate the isolation level (2PL for
explicit txns, wait-die deadlock avoidance); crash harness green with 4
concurrent autocommit writers (`ConcurrentWritersRecoverEveryCommittedOp`:
committed==durable per thread, ≤1 in-flight op per thread, exact num_keys,
sorted scan) alongside the Phase 4 single-writer and 5.4 txn-loser runs.
Tag `v0.5-phase5` NOT created — user instructed no git commits.

---

## Phase 6 — Performance & portfolio polish ✅ DONE (2026-07-06)

All work uncommitted in the working tree by explicit instruction (no git
commits); the C6 tag is therefore deferred, same as C1-C5.

- [x] 6.1 `bench/` with Google Benchmark: sequential load, random load, uniform +
  Zipfian (YCSB scrambled, theta 0.99) read-only, 95/5 mixed, scan-heavy,
  delete churn; ops/sec + hand-collected p50/p99 (steady_clock per op). Baseline:
  SQLite (WAL mode, `WITHOUT ROWID`, 32 MB cache). LMDB omitted — not installed
  on the machine, a vendored copy wasn't worth the dependency (plan.md 6.1
  allows "and/or"); gaps written up in `BENCHMARKS.md`. Shared workload machinery
  (`bench/bench_util.h`) makes db_bench and sqlite_bench byte-identical.
  `KVSTORE_BUILD_BENCH` option + `bench` preset; `tools/run_bench.sh` is the
  one-command runner (saves raw txt/json + `environment.txt` under
  `docs/bench_results/<date>-<label>/`).
- [x] 6.2 Profile-driven fixes, one at a time, re-measured (macOS `sample` on a
  RelWithDebInfo random-load driver). Profile disagreed with the plan's guess:
  1. **CRC-32 was ~55% of wall time** (not the insert path). Byte-table →
     ARMv8 `__crc32b/__crc32d` + slicing-by-8 fallback; same reflected
     polynomial (checksums unchanged), cross-checked bit-identical over 2000
     buffers + the standard check vector. Biggest win.
  2. **In-place `InsertAt` slot shift** (defect #10): `Page::InsertRecordAt`
     replaces the vector<string> page rebuild. Contract change — inserts no
     longer implicitly compact; `EnsureFit`/`Compact` sole owner of dead-byte
     reclamation (node_test updated, watched fail→pass).
  3. **WAL `AppendTo` quadratic `reserve()`** (latent bug, found by profiling at
     raised `max_wal_bytes`): exact per-record reserve defeated string
     geometric growth → O(n²) memmove between checkpoints; a 64 MB threshold
     went from minutes to 0.95 s. Removed the reserve.
  Net 2.6–4.9× over the phase-start baseline (`BENCHMARKS.md` progression table).
  Deferred with rationale: memcmp compare (already memcmp in libc++, no-op),
  group commit (needs Phase 5 threads), separator truncation / `BulkLoad`
  (checkpoint I/O is the bound, not tree height).
- [x] 6.3 CI: `.github/workflows/ci.yml` — matrix {gcc-14, clang-18} ×
  {asan (full suite + 100-run crash harness), release (build + bench smoke)};
  release jobs build the portable slice-by-8 CRC path the ARM dev machine never
  compiles. README badge added.
- [x] 6.4 Docs: `ARCHITECTURE.md` (layers + Get/Put/Eviction/Checkpoint/Recovery/
  Scan lifecycles), `FORMAT.md` confirmed v3 (Phase 6 changed no bytes — noted),
  `DESIGN_DECISIONS.md` (plan.md §4 table with outcomes), `BENCHMARKS.md`
  (methodology + comparison + progression). README rewritten with the
  load→`kill -9`→reopen→verify demo (`tools/crash_demo.sh` + `kvstore_demo
  load/verify`) and the benchmark table. (Demo uses 300k–500k keys, not 1M — a
  0.6 s kill window catches ~17k-key prefixes, which proves the property without
  a minute-long load.)

Checkpoint C6 ✅ (2026-07-06): `tools/run_bench.sh` reproduces the README table
one-command (`docs/bench_results/2026-07-06-phase6-final/`); every plan.md §4
decision has a written rationale in `DESIGN_DECISIONS.md`; full suite 14/14
green under asan and debug after all three optimizations; crash_demo shows a
kill -9 mid-load recovering to a clean, structurally-valid prefix. Tag `v0.6`
NOT created — user instructed no git commits.

---

## Post-C5/C6 fix (2026-07-08): WAL op-group atomicity vs the write-ahead rule

Found by review: recovery drops a record group whose op-end record is not
durable, assuming none of the group's pages reached `data.db` — but the
buffer pool didn't guarantee that. A split logged its page images without
op_end and then unpinned the new right leaf / relinked sibling; eviction
could write one of those pages after a `FlushTo(page_lsn)` that a concurrent
commit had already rendered a no-op (durable prefix past the image, op-end
still buffered). Crash in that window ⇒ half a split on disk that recovery
never repairs (broken backward chain / orphaned page). Fix, test-first
(`WalRuleTest.DirtyEvictionFlushesTheWholeAppendedLog`, watched fail→pass):

- `BufferPool::EnforceWalRule` now flushes the WHOLE appended log
  (`Flush()`), not just through the victim's `page_lsn`.
- The tree keeps every page an op rewrites PINNED until the group's op-end
  record is appended: `SplitLeaf`/`SplitInternal` return the new page's
  guard (callers use it instead of re-fetching) and park the relinked
  sibling's guard in an `op_guards` vector that lives past the final Log*
  call. Worst-case pin chain is now ~2*depth+2, so DbImpl (and the tree
  test fixtures) clamp the pool to 16 frames minimum (was 4).
- Crash harness gained `VerifyStructuralIntegrity`: full tree walk +
  backward/forward chain agreement + orphan-page detection after every
  recovery, in all four crash tests (the old checks — committed-prefix and
  sorted forward scan — could not see this bug class).
- FORMAT.md / ARCHITECTURE.md / header comments updated to state the
  pinning + whole-log-flush pair as the invariant.

## Post-C5/C6 fix (2026-07-08): unlatched root read in Seek()

Found by review: `Seek` short-circuited through an unlatched `IsEmpty()`,
reading `root_page_id` while writers update it under the exclusive
`root_latch` — a data race, and functionally a scan racing the first insert
or a root split could spuriously report an empty tree. Fix: the early check
is gone; `FindLeaf` performs the emptiness check under the shared latch and
`Seek` maps its NotFound back to the "invalid cursor, Ok status" contract.
Regression test `BTreeConcurrent.SeekRacingFirstInsertAndRootSplitsIsLatched`
scans while the tree grows from empty in the SAME key space (the existing
stress tests use disjoint namespaces and pre-grown trees, which is why they
never caught it); with the old code it trips tsan on `SetRootPageId`,
watched fail→pass.

---

## Phase 7 candidates (from post-C6 review; unscheduled)

Ordered by user impact, each with its evidence gate:

- 7.1 Overflow pages + byte-aware leaf split. Today key+value > ~4 KB is
  rejected up front (`InvalidArgument`, no mutation — was: Internal failure
  deep in the split path that poisoned the handle), and records above ~1 KB
  can still fail mid-split on unlucky packing because the count-based split
  point gives no byte guarantee. The first limitation a real user hits.
- 7.2 Group commit: batch concurrent commit records into one fsync. Gate:
  `BM_SeqLoadSync`-style benchmark with N committer threads showing the
  N-serialized-fsync cost first.
- 7.3 `root_latch` scalability: version-validated optimistic root read (OLC
  B-tree style) to stop every operation bouncing one cache line. Gate: a
  high-thread-count read benchmark showing the latch as ceiling.
- 7.4 Partial WAL truncation up to the oldest active transaction's begin-LSN
  (today: truncation skipped entirely while any written-to txn is open).
- 7.5 MVCC (carried 5.6 stretch): snapshot reads fix scan isolation and
  phantoms.

## Cross-phase rules

- A phase is done at its checkpoint, not when its last feature lands; stretch
  items never block a checkpoint.
- Never mix a format bump, a refactor, and a feature in one commit.
- Every defect fix (plan.md §1 list) lands with the regression test that would
  have caught it.
- `kvstore_inspect` and the tree validator are updated in the same commit as any
  format or structural change.
