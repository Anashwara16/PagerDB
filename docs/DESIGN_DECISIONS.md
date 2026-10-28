# PagerDB — Design Decisions and Outcomes

The decision table from `plan.md` §4, revisited at Phase 6 with what actually
happened. Each row: the choice made up front, the rationale, and the outcome
once it met reality. Checkpoint C6 requires every decision to have a written
rationale — this is that document.

## Storage & integrity

**Checksum: CRC-32, table already in page.cc.**
*Rationale:* good-enough corruption detection with zero new dependencies; the
constexpr table predated Phase 1.
*Outcome:* right call with one big footnote. Verification caught real bugs
(pager tests flip bytes on disk and rely on it), but Phase 6 profiling showed
the byte-at-a-time table CRC eating **~55% of wall time** on the random-load
benchmark (every WAL page image + every page write stamp + every read
verify). Replaced with ARMv8 CRC32 instructions (same reflected polynomial
0xEDB88320 — stored checksums unchanged) plus a slicing-by-8 portable
fallback: random load 3.0x, cold uniform reads 7.9x, delete churn 4.0x
(`BENCHMARKS.md`). Lesson recorded: a per-4KB-pass algorithm choice is a
performance decision, not just a correctness one.

**Page verify toggle: `Options::verify_checksums = true`.**
*Rationale:* benchmarks need the off switch.
*Outcome:* kept, default on. After the hardware-CRC change the toggle no
longer matters much (CRC dropped to ~7% of the profile), which is the
healthier place to be — integrity you don't have to turn off.

**Delete strategy (P2): tombstone-free slot shift + `dead_bytes`,
compact-on-demand; no merge/rebalance initially.**
*Rationale:* underflow handling is stretch; "leaves may go empty" documented.
*Outcome:* held up. `EnsureFit` compacts only when an insert actually needs
the space; the 50k-op interleaved property test found one real interaction
bug (update outgrowing a full leaf) which now splits correctly. Underflow
merge (task 2.12) remains unimplemented and documented; the churn benchmark
shows page count stays flat over delete/reinsert cycles, so the practical
cost is bounded fragmentation, not growth.

**Free list (P2): on-disk singly linked via freed page's first 4 bytes,
head in meta.**
*Rationale:* simplest reuse; no bitmap needed at this scale.
*Outcome:* works, LIFO reuse verified by pager tests and inspected by
`kvstore_inspect`'s free-list walk (typed-kFree, acyclic, disjoint from
tree). Underused in practice because the tree never frees pages (see delete
strategy) — its real consumer arrives with underflow merges.

## API

**Scan API (P2): internal `Cursor` + `DB::Scan(start, end, callback)`.**
*Rationale:* callback keeps the public API small; cursor stays internal.
*Outcome:* good shape. The cursor copies each leaf out of the buffer pool and
holds no pins between steps, which kept Phase 3's pin accounting trivial and
scans immune to pool exhaustion; scan-heavy benchmark does ~20M entries/s
warm (~4x the SQLite baseline), so the copy has not been the bottleneck.

## Caching

**Replacement (P3): plain LRU (map + intrusive list), pinned frames
unevictable. LRU-K a documented non-goal until benchmarks say otherwise.**
*Rationale:* correct before clever.
*Outcome:* benchmarks so far don't say otherwise. The Zipfian read benchmark
at 64 frames (0.25% of the working set) still gets a 65% hit rate from plain
LRU; scan wipe-out (LRU-K's justification) hasn't hurt because scans bypass
pins and the pool is small relative to the OS page cache behind it. Revisit
only with a workload where sequential floods measurably evict the hot set.

**Minimum pool size: `kMinFrames = 4`, `Busy` when all frames pinned.**
*Rationale:* a split holds ≤3 guards + 1 parent per level; 4 frames is the
floor for the deepest single-threaded guard chain.
*Outcome:* the {4, 16, 1024}-frame regression gate kept this honest. One
subtlety surfaced in Phase 5.1's stress test: with N threads each pinning
one page, N > frames makes transient `Busy` legitimate — the test now
retries on `Busy` and fails on anything else.

## Durability

**WAL (P4): redo-only physiological log, LSN = byte offset, idempotent redo
via `page_lsn`; undo deferred to Phase 5.**
*Rationale:* smallest correct recovery story; matches the reserved
`Lsn`/`page_lsn` fields from day one.
*Outcome:* the core held, but "smallest" grew three documented survival
features the crash harness demanded: a `page_id` per record (pure logical
records can't replay into a half-flushed split), op-end grouping
(all-or-nothing application of multi-page structural changes), and full-page
images on first post-checkpoint touch (torn data-page writes; found by
harness run 5, seed 20260710). The redo-only limitation is real: a failed
write op poisons the handle until reopen. Phase 5 undo subsumes it.
*Phase 6 note:* full-page images make random-write workloads
checkpoint-bound — ~60% of random-load wall time is checkpoint fsync/pwrite.
That's the price of torn-page safety at 4 MB `max_wal_bytes`; group commit
and checkpoint tuning are the known levers, deferred with Phase 5.

**Meta durability (P4): dual meta pages + generation counter.**
*Rationale:* fixes the torn-meta-write window (defect #9) without logging
meta writes.
*Outcome:* clean win. Recovery tests corrupt either copy and open still
succeeds; the alternating-write scheme cost ~30 lines in the Pager.

**macOS fsync: `fsync` only; `F_FULLFSYNC` documented but not default.**
*Rationale:* `F_FULLFSYNC` is drastically slower and the tradeoff is
platform-wide, not ours to hide.
*Outcome:* visible in benchmarks — sync-per-put achieves 32k puts/s on an
M2 laptop SSD, which is `fsync`-to-disk-cache, **not** power-loss-safe on
macOS. `BENCHMARKS.md` states this explicitly next to the number.

## Concurrency (Phase 5)

**Latch crabbing, pessimistic first; strict 2PL with wait-die; MVCC as
stretch.**
*Rationale:* correct before clever.
*Outcome:* the full ladder landed. 5.1: thread-safe buffer pool (pool mutex
+ per-frame `shared_mutex` content latches, read/write guard variants).
5.2: reads crab with shared latches; deletes exclusive-couple (never split);
Puts got the "then optimistic" refinement — a shared-`root_latch` descent
with an exclusive latch on the leaf only, falling back to the pessimistic
full-exclusive path only when the leaf must split. The fallback trigger
deviates from the sketch: "InsertLeaf failed" rather than `CanFit` (CanFit
ignores reclaimable dead bytes, and separator lengths are unknowable at
descent time; try-then-restart is simpler and more precise). 5.3/5.4:
transactions with txid-stamped WAL records, lazy kBegin, in-memory undo
lists + logged before-images; recovery is repeat-history redo + newest-first
undo of losers. 5.5: strict 2PL per-key S/X lock manager, wait-die on
transaction age with a 1 s backstop; locks release only at Commit/Abort.
Age is a priority decoupled from the WAL txid: `Begin(retry_of)` lets a
wait-die victim retry under its original age (classical wait-die's
starvation-freedom depends on restarts keeping their timestamp) while still
logging under a fresh txid, since reusing the old txid would make a crash
mid-retry look like an already-ended transaction to recovery.

The resulting isolation level, stated plainly ("strict 2PL" alone would
overpromise): explicit transactions are serializable **against each other**
for **point operations only** — locks cover key names, not ranges, so there
is no phantom protection. `Scan` takes no key locks and is read-uncommitted
(it can see uncommitted writes or a rollback in progress). Autocommit ops
take no key locks either: single atomic durable operations that ignore
transaction boundaries — an autocommit write can land on an X-locked key,
and the transaction's later rollback would erase it, so autocommit traffic
and transactions must use disjoint keys. This is a deliberate boundary
(single-op latency, no retry loop in the plain API); the honest fix for
scans and phantoms is MVCC (5.6, future work), not more locks.
MVCC (5.6) stayed a stretch goal, not started. The whole suite is
tsan-clean; the crash harness gained a 4-writer concurrent variant.
Group commit is now unblocked but still deferred (needs contention data).

Checkpoints vs transactions (post-C5 fix): originally a transaction held the
checkpoint latch shared for its whole lifetime — one idle open handle blocked
every checkpoint and let the log grow without bound, and `std::shared_mutex`'s
lack of writer preference let sustained autocommit traffic starve the
checkpointer. Now the latch is per-operation with a writer-parking gate in
front of a pending checkpoint, and the transaction's only lasting footprint is
a counter of begun-but-unfinished transactions: while it is nonzero a
checkpoint flushes pages as usual but skips WAL truncation, since steal may
already have written uncommitted pages and the log is the only undo source.
This is the first step toward ARIES-style fuzzy checkpoints; the remaining
refinement (truncate up to the oldest active transaction's kBegin LSN instead
of keeping the whole log) is documented future work. A deliberately rejected
alternative: aborting transactions past an age/size budget — it trades a
liveness bug for artificial aborts without removing the fundamental
log-retention requirement.

## Performance (Phase 6)

**Insert perf: keep rebuild-based `InsertAt` until profiled.**
*Rationale:* it was the *suspected* hotspot (defect #10), but measure first.
*Outcome:* measuring first was the entire story. The profile ranked the
suspects: CRC-32 ~55%, checkpoint fsync ~22%, and the rebuild-based insert
only ~5% (the ~30-record leaves make O(n) rebuilds cheap in absolute terms).
Optimization order followed the profile, not the plan's guess:
1. **CRC-32 → hardware/slice-by-8** (biggest win, table above).
2. **`InsertAt` → in-place slot shift** (`Page::InsertRecordAt`): the
   directory shift replaces a full page rebuild through `vector<string>`.
   Side effect: inserts no longer implicitly compact dead bytes — the
   deferred-compaction contract (`EnsureFit`) now owns reclamation alone.
3. **`string_view::compare` was already memcmp-based** in libc++ — the
   planned "memcmp key compare" item was a no-op; verified in the profile
   (`_platform_memcmp` <1%) and dropped.
4. **`UpdateLeaf` → in-place update** (post-C6 review catch: it still did the
   `vector<string>` rebuild that item 2 removed from inserts, invisible
   because no workload isolated updates). Added `BM_Overwrite` (100% updates)
   first, per the measure-first rule: p50 −21% (4.25→3.38 µs), throughput
   +6% — modest because the WAL append dominates a full update, but the old
   cost scaled with page occupancy and the new one is O(record). Same
   deferred-compaction contract as inserts: shrinks/grows leave dead bytes
   that `EnsureFit`/`Compact` reclaim.
Deferred with written rationale: group commit (needs Phase 5 threads),
separator truncation and `BulkLoad` (stretch; fanout is not the current
bottleneck — checkpoint I/O is).

**Baseline choice: SQLite (WAL mode, `WITHOUT ROWID`), not LMDB.**
*Rationale:* plan.md allows "SQLite and/or LMDB"; SQLite is on every machine
and its WAL+B-tree shape is the honest comparison. LMDB wasn't installed on
the benchmark machine and a vendored copy wasn't worth the dependency.
*Outcome:* see `BENCHMARKS.md` for numbers and the fairness notes (what
SQLite does that we don't: SQL layer, full ACID, decades of tuning).
