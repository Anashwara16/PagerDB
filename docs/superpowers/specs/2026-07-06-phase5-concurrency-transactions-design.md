# Phase 5 — Concurrency & Transactions (5.2–5.5)

**Status:** design approved 2026-07-06. Scope: core 5.2–5.5; MVCC (5.6) deferred.
**Standing constraint:** no git commits — all work stays uncommitted in the
working tree; the `v0.5-phase5` tag is deferred, same as C1–C6.

## 1. Goal & scope

Turn the single-writer, autocommit engine into a concurrent, transactional one:

- **5.2** Lock-safe B+ tree via latch crabbing + a pool-level `root_latch`.
- **5.3** A `txn/` module: `TxnManager`, an explicit `Txn` handle
  (`Begin/Get/Put/Delete/Commit/Abort`), real txids and `kBegin/kCommit/kAbort`
  log records; existing `Get/Put/Delete` become autocommit one-op transactions.
- **5.4** Undo via before-images: in-session `Abort`, and recovery becomes
  **redo-all + undo-losers**. Removes the `needs_reopen_` hack.
- **5.5** A strict (rigorous) 2PL `LockManager`: per-key S/X locks with wait-die,
  giving **serializable** isolation for point operations.

Reaches **Checkpoint C5**: tsan-clean stress + anomaly suite; crash harness green
with concurrent writers.

**Out of scope (deferred, with written rationale):**
- **5.6 MVCC** — snapshot-isolation reads + version chains + GC. Explicitly a
  stretch goal; deferred to keep C5 clean.
- **Phantom/range protection** — scans take no predicate/range locks; documented.
- **Group commit** — needs a shared fsync window; already deferred to Phase 6.

## 2. Current state (what we build on)

- **5.1 done:** `BufferPool` is thread-safe. `FetchPageRead` (shared latch),
  `FetchPageWrite` (exclusive latch), `FetchPage` (pin only, no latch — recovery
  / single-thread callers). Per-frame `std::shared_mutex`; one pool `latch_`
  guarding the page table / free list / replacer / pins / stats. Documented lock
  ordering: pool `latch_` is a leaf below all page content latches; the WAL
  writer's mutex is below the pool latch. `PinCount`/`IsResident` helpers exist.
- `TxId` (`uint64`, `kInvalidTxId = 0`) exists in `types.h`. Every `LogRecord`
  already has a `txid` field, currently written as 0.
- `StatusCode::kBusy` exists (used for wait-die "die" and pool exhaustion).
- The `tsan` preset (`KVSTORE_SANITIZER=thread`) is wired in CMakePresets.
- The tree descends with **unlatched** `FetchPage` everywhere: `FindLeaf`
  (Get/Delete/Seek), `FindLeafPath` (Put). Root id lives in pager meta
  (`pool_.pager().root_page_id()`), mutated via `SetRootPageId`.
- WAL is **redo-only, physiological**, LSN = byte offset, idempotent redo via
  `page_lsn`, op-group all-or-nothing (`kOpEndBit`), full-page images for the
  first post-checkpoint mutation of each page. Recovery replays complete op-groups
  forward and drops a torn/incomplete trailing group.
- `db.cc` has a `needs_reopen_` flag set when a write op fails half-applied
  (exists *only* because there is no undo yet) — removed in 5.4.

## 3. Architecture

```
DB::Begin() → Txn { Get/Put/Delete/Commit/Abort }     public API (db.h)
  TxnManager        atomic txid counter, owns LockManager, hands out Txns
  LockManager       per-key S/X locks, rigorous 2PL, wait-die
  Txn undo stack    in-memory before-images for Abort
        │  (holds key locks; holds NO page latches when it calls down)
        ▼
  BPlusTree         latch crabbing: root_latch → path top-down, release-when-safe
        ▼
  BufferPool        FetchPageRead/Write + a pool-owned root_latch (shared_mutex)
        ▼
  WAL               +kBegin/kCommit/kAbort, real txids, before-images on kPut/kDelete
```

`DbImpl` gains a `TxnManager` (which owns the `LockManager`). Ownership order in
`DbImpl` must keep `wal_writer_` before `pool_`/`tree_` (unchanged) and add the
`TxnManager` after the tree it drives.

## 4. 5.2 — Latch crabbing

**Reads** (`Get`, `Contains`, `Seek`, `FindLeaf`): descend taking `FetchPageRead`
(shared). Latch-couple: acquire the child's latch, then release the parent
immediately (a read never causes a split, so no ancestor can be affected).

**Writes** (`Put`, `Delete`, `FindLeafPath`): descend taking `FetchPageWrite`
(exclusive) and latch-couple, **releasing all held ancestors as soon as the
current node is "safe"** — i.e. the child's pending change cannot propagate into
it. Safety predicate:
- Insert: node `CanFit` the largest possible incoming separator ⇒ it won't split
  ⇒ safe.
- Delete: because we do **no** merge/rebalance (decision 2.8), a delete never
  modifies an internal node ⇒ *every* ancestor is trivially safe. So the delete
  path exclusive-latch-couples straight down, releasing each ancestor the moment
  the child below it is latched, and ends holding only the target leaf exclusive.

Unifying: both insert and delete descend with `FetchPageWrite`; they differ only
in the safety predicate (insert: `CanFit`; delete: always-safe). This gives a
stable, race-free leaf without any read-then-upgrade window.

**`root_latch`**: a `std::shared_mutex` **owned by `BufferPool`**, exposed via
`root_latch()`. The tree acquires it to make "read `root_page_id` + latch the
root" atomic against a concurrent root change:
- Read / non-splitting descent: hold `root_latch` **shared** just long enough to
  read the root id and latch the root node, then release once the root is deemed
  safe.
- Root change (first insert into an empty tree; root split; `SetRootPageId`):
  hold `root_latch` **exclusive** across the whole root mutation.

**Latch order (deadlock-free):** `root_latch` → tree path **top-down**. Split
allocation of new pages via `NewPage` returns an exclusive latch; new pages are
latched *after* the node being split, preserving top-down order. Sibling-chain
latching is only done by scans and is handled in §7.

**Test — `tests/btree_concurrent_test.cc`** (tree level, no txn layer yet; uses
latched fetches directly): 8 threads. (a) Disjoint key ranges per thread, each
checked against its own `std::map`. (b) Shared keys with a structural-invariant
check (sorted order, leaf-chain linkage, no torn reads). Run under `tsan`.

## 5. 5.3 — `txn/` module

**New files:** `include/kvstore/txn/txn.h`, `include/kvstore/txn/lock_manager.h`,
`src/txn/txn.cc`, `src/txn/lock_manager.cc`. `Txn` and `TxnManager` are declared
in `txn.h`; `db.h` includes it so `DB::Begin()` can return a `Txn`.

**`TxnManager`**: holds `std::atomic<TxId> next_txid_` (starts at 1), the
`LockManager`, and references to the tree + WAL writer. `Begin()` allocates a txid
and constructs a `Txn`. Also the entry point autocommit uses.

**`Txn`** — the public handle:
- `Result<std::string> Get(key)` — S-lock `key`, then tree read.
- `Status Put(key, value)` — X-lock `key`, capture before-image, tree write, push
  undo entry.
- `Status Delete(key)` — X-lock `key`, capture before-image, tree delete, push undo.
- `Status Commit()` — write `kCommit` (op_end), flush WAL iff `sync_on_write`,
  release all locks.
- `Status Abort()` — apply undo stack backward (logged inverses), write `kAbort`,
  release all locks.
- Carries: `txid_`, `TxnManager*`, lock set, undo stack, a `began_logged_` flag.

**Log records:** add `LogRecordType::kBegin = 8, kCommit = 9, kAbort = 10`.
Payload-free (page_id = `kInvalidPageId`, key/value empty). `kBegin` is written
**lazily on the first write** of a txn (read-only txns log nothing and are
invisible to recovery, which is correct — they have nothing to undo/redo).

**Real txids:** the current txid threads into the tree's WAL emission. The tree's
`Put`/`Delete` take a `TxId txid` parameter (default `kInvalidTxId` for the
no-WAL/test path); `LogKeyRecord`/`LogPageImage`/`LogPageEvent` stamp it into the
record's `txid` field.

**Autocommit:** `db->Put/Delete/Get` = `Begin(); op; Commit()` (a lighter inline
equivalent is fine as long as it takes the same lock + logs `kBegin`/`kCommit`).
Preserves today's single-op semantics and makes concurrent autocommit writes
mutually serializable.

**Durability contract unchanged:** commit = commit record flushed. Data pages
reach disk lazily behind the log (write-ahead rule, already enforced by the pool).

## 6. 5.4 — Undo & recovery *(the crux)*

### 6.1 Before-images in the log
`kPut` and `kDelete` records carry a **before-image**: the key's prior value, or
an "absent" marker (key did not exist).

Encoding: widen the fixed record header by one field, `before_len:2`, placed after
`val_len`; the before-image bytes go after `value`, before the CRC. New on-disk
layout:

```
[lsn:8][txid:8][type:1][page_id:4][key_len:2][val_len:2][before_len:2]
       [key][value][before][crc:4]
```

`before_len` uses a sentinel to distinguish "empty value" from "key was absent":
`0xFFFF` = absent (no prior record), any other value = length of the prior value
(0 allowed, an empty prior value). All record types carry the field (0 for those
that don't use before-images) so the parser stays uniform. `kHeaderSize` grows by
2 bytes. **Only the WAL record format changes; the v3 data-file format is
untouched.** Documented in `FORMAT.md`.

The before-image is captured cheaply during the write's existing leaf visit: the
tree already does `FindKeyIndex` on the target leaf, so the prior value (or
absence) is read from the slot before it is overwritten — no extra descent.

Inverses (all idempotent, absolute operations):
- undo of insert (key was absent) ⇒ **delete** key.
- undo of update (key had old value) ⇒ **put** old value.
- undo of delete (key had old value) ⇒ **put** old value.

### 6.2 In-session Abort
`Txn` keeps an in-memory undo stack (per write: key + before-image). `Abort()`
walks it newest→oldest, applying each inverse **through the tree as a normal
logged record under the same txid**, then writes `kAbort` (op_end) and releases
locks. Because inverses are absolute puts/deletes, replaying them is idempotent.

### 6.3 Recovery: redo-all + undo-losers (ARIES-lite, no CLRs)
1. **Redo forward.** Replay every *complete* physical op-group exactly as today
   (`page_lsn`-guarded, torn trailing group dropped), **including uncommitted
   txns**. While replaying, tally per-txid whether a `kCommit` **or** `kAbort`
   record was seen.
2. **Undo losers backward.** A **loser** = a txid that has data records but
   **neither** `kCommit` **nor** `kAbort`. For each loser's `kPut`/`kDelete`
   record, newest→oldest, apply the logical inverse (re-descend, put-old / delete)
   using the record's before-image.
3. **Recount `num_keys`** from the leaf chain (moves to *after* the undo pass).

**Winners** (have `kCommit` or `kAbort`) are redo-only; an aborted txn's inverses
were logged in 6.2, so redoing them reproduces the rolled-back state — never
undone again. Rule: **undo iff (has records) ∧ ¬commit ∧ ¬abort.**

**Correctness / restart-safety argument (to be documented inline in `db.cc`):**
`Open()` runs recovery and then *immediately* checkpoints (flush-all + WAL
truncate). Logical undo via absolute put/delete is idempotent, so a crash *during*
recovery re-runs the same redo+undo from the same checkpoint on the next `Open()`
with the identical result. Therefore **no compensation-log records (CLRs) or
LSN-chaining are required** — the immediate post-recovery checkpoint plus
idempotent logical undo suffice.

**Removes `needs_reopen_`:** a mid-op write failure now triggers `Abort()`
(rollback of the partial txn) instead of poisoning the handle.

**Test — `tests/txn_recovery_test.cc`** + a demo mode: a writer loops
`Begin; Put×k; Commit`, `kill -9` mid-transaction. After recovery: every fully
committed txn's keys are present as a clean prefix; the interrupted txn's keys are
**absent**. Extends the existing fork crash harness.

## 7. 5.5 — Strict 2PL `LockManager`

**Per-key S/X locks.** `std::unordered_map<std::string, LockEntry>` guarded by a
mutex; `LockEntry` = current mode, holder txids, FIFO wait queue with condition
variables. (Single mutex for correctness first; sharding noted as a perf
follow-up, not required for C5.)

**wait-die** deadlock avoidance (chosen over "1 s timeout → Busy": deterministic,
no wall clock, non-flaky tests). On a conflicting request by txn `T`:
- If `T` is **older** (smaller txid) than *all* conflicting holders → `T` **waits**.
- If `T` is **younger** than any conflicting holder → `T` **dies**: return
  `StatusCode::kBusy`; the caller aborts the txn.

Wait-die is provably **deadlock-free** for any txid assignment. *Retry policy:*
an explicit txn surfaces `kBusy` and the caller decides. **Autocommit** retries
internally in a bounded loop; to preserve wait-die's starvation-freedom (the
oldest txn never dies), a retry **reuses the txn's original txid** rather than
drawing a younger one, so a repeatedly-losing autocommit op ages into being the
oldest and is then guaranteed to win. (Deadlock-freedom holds regardless; txid
retention buys the progress guarantee.)

**Rigorous 2PL:** acquire S on `Get`, X on `Put`/`Delete`; hold **all** locks
(S and X) until commit/abort, then release together ⇒ **serializable** schedules
for point operations. Lock upgrade S→X on the same key by the same txn is allowed
(treated as already-held if no other holder; else conflict via wait-die).

**Scans:** honestly scoped. `DB::Scan` / cursor iteration is **physical-latch
consistent** (no torn pages, valid ordering) but takes **no** logical/range locks
⇒ not phantom-protected and not repeatable across a txn. This is the one place we
do not reach full serializability; documented in `DESIGN_DECISIONS.md`. All
anomaly tests are point-conflict, so this does not weaken them.

**Cursor / Scan latching (avoids leaf-chain deadlock):** `DB::Scan` buffers one
leaf's entries into memory **under that leaf's read latch**, releases the latch,
then invokes the user callback latch-free, then latch-couples to the next leaf.
This (a) never holds a latch across arbitrary user code, and (b) sidesteps
reader/writer lock-coupling deadlocks on the sibling chain. Semantics:
read-committed-per-leaf; documented.

**Test — `tests/lock_manager_test.cc`** (unit: grant/conflict/upgrade/wait-die
victim selection) and **`tests/anomaly_test.cc`** (integration, forced
interleavings via barriers):
- **Dirty read:** T1 X-locks + writes `k` (uncommitted); T2 `Get(k)` must block
  on T1 or die — never sees the uncommitted value.
- **Lost update:** T1, T2 both read-modify-write `k`; 2PL serializes them (the
  second X blocks) ⇒ no lost update.
- **Write-write:** T1 X-locks `k`; T2's X on `k` waits or dies (`kBusy`).

## 8. Deadlock freedom (cross-cutting invariant)

**Never wait on a logical lock while holding a page latch.** Each statement:
1. acquires its key lock first (holding only previously-acquired *locks*, no
   latches — normal 2PL; cycles resolved by wait-die), then
2. performs the latched tree op (lock already granted ⇒ no lock-wait occurs while
   latches are held; latches released at op end).

Latch cycles are independently prevented by crabbing order (`root_latch` →
top-down). Two orthogonal mechanisms: **wait-die** for logical locks, **crabbing
order** for physical latches.

## 9. Design decisions (for `DESIGN_DECISIONS.md`)

| Decision | Choice | Why |
|---|---|---|
| Tree concurrency | Latch crabbing, exclusive-crab with release-when-safe (`CanFit`) | Correct, standard, matches 5.1 latch primitives |
| Root protection | Pool-owned `root_latch` (`shared_mutex`) | Root id is in meta, not a fixed page; makes "read root + latch root" atomic |
| Isolation | Rigorous 2PL → serializable (point ops) | The anomaly tests are point-conflict; simplest correct story |
| Deadlock (locks) | wait-die | Deterministic, no wall clock, non-flaky tests, provably deadlock-free |
| Deadlock (latches) | crabbing order + "no lock-wait under a latch" | Two orthogonal mechanisms, cleanly separated |
| Undo | Logical, before-images in the WAL record | Composes with structural changes; idempotent absolute inverses |
| Recovery | Redo-all + undo-losers, **no CLRs** | Immediate post-recovery checkpoint + idempotent undo ⇒ restart-safe |
| Txn API | Explicit `Begin()`→`Txn`; Get/Put/Delete autocommit | Lets anomaly tests express multi-statement txns |
| Scan isolation | Read-committed-per-leaf, no range locks | Phantom protection is out of scope; stated honestly |

## 10. Isolation guarantees (precise claims — no overclaiming)

- **Serializable** for transactions whose conflicts are on individually locked
  keys (point `Get`/`Put`/`Delete`), via rigorous 2PL.
- **Atomicity**: commit-or-nothing, enforced by in-session undo and recovery
  undo-losers.
- **Durability**: committed ⟺ commit record flushed (recovery replays it).
- **Not provided:** phantom protection / range serializability for scans; snapshot
  isolation (that is 5.6 MVCC, deferred).

## 11. Testing strategy

- New: `btree_concurrent_test.cc` (5.2), `lock_manager_test.cc` +
  `anomaly_test.cc` (5.5), `txn_recovery_test.cc` (5.4).
- Extend the fork crash harness / demo with a transactional writer mode.
- Every existing test stays green under `asan`+`debug`.
- The full suite runs under `tsan`; the concurrent stress + anomaly tests are the
  teeth. CI `tsan` job runs them.

## 12. File plan

**New:** `include/kvstore/txn/{txn,lock_manager}.h`,
`src/txn/{txn,lock_manager}.cc`; tests `btree_concurrent_test.cc`,
`lock_manager_test.cc`, `anomaly_test.cc`, `txn_recovery_test.cc`.

**Modified:**
- `wal/log_record.{h,cc}` — `kBegin/kCommit/kAbort`; before-image field.
- `btree/btree.{h,cc}` — latch crabbing; thread txid; carry before-images.
- `storage/buffer_pool.{h,cc}` — `root_latch()` accessor + member.
- `btree/cursor.*` + `db.cc Scan` — leaf-buffered latch-free callback.
- `db.h` — `Txn` type (via `txn.h`) + `DB::Begin()`.
- `db.cc` — construct `TxnManager`; route autocommit; rewrite `Recover`
  (redo-all + undo-losers); drop `needs_reopen_`.
- `CMakeLists.txt` — txn sources + new test targets.
- Docs: `ARCHITECTURE.md` (concurrency lifecycle), `DESIGN_DECISIONS.md`,
  `FORMAT.md` (WAL record: new types + before-image), `README.md` (status),
  `phases.md` (mark 5.2–5.5 done, C5; note 5.6 deferred).
- `.github/workflows/ci.yml` — ensure the tsan job runs the concurrency suite.

## 13. Implementation order & gating

`5.2 → 5.3 → 5.4 → 5.5`, each shipping independently. After each sub-step: full
suite green under `asan`+`debug`, then the new concurrent tests under `tsan`.
Terminal state: **Checkpoint C5** reached (tag `v0.5-phase5` deferred — no
commits).

## 14. Risks & mitigations

- **Latch/lock ordering deadlocks** → the §8 invariant + crabbing order; verified
  under tsan.
- **Recovery interplay with undo** (losers spanning checkpoints) → losers are only
  txns with post-checkpoint records lacking commit/abort; idempotent undo + the
  immediate post-recovery checkpoint make it restart-safe (§6.3).
- **Scan holding latches across user code** → leaf-buffered, latch-free callback.
- **Test flakiness under concurrency** → deterministic wait-die + barrier-forced
  interleavings instead of sleeps/timeouts.
