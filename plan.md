# PagerDB — Implementation Plan

Design-level plan for evolving PagerDB from the current MVP (Get/Put on an on-disk
B+ tree, no delete, no cache, no WAL) into a crash-safe, concurrent storage engine.
Phased task breakdown lives in `phases.md`. No code changes are implied by this
document alone.

## 1. Current state (as of commit `196a6ac`, branch `pager`)

Layering is already clean and worth preserving:

```
DB (include/kvstore/db.h) → DbImpl (src/db.cc)
  └─ BPlusTree (include/kvstore/btree/btree.h, src/btree/btree.cc)
       └─ NodeView (include/kvstore/btree/node.h, src/btree/node.cc)   [view over Page]
            └─ Pager (include/kvstore/storage/pager.h, src/storage/pager.cc)
                 └─ Page / PageHeader / SlotEntry (include/kvstore/storage/page_format.h)
                 └─ PosixFile (include/kvstore/storage/file.h, src/storage/file.cc)
```

What exists and works:
- `PosixFile`: RAII fd wrapper, positioned I/O (`pread`/`pwrite`), `Sync`, move-only,
  `std::expected<T, FileError>` error handling.
- `Page`: 4 KB slotted page (`kPageSize = 4096`), 40-byte `PageHeader`, 8-byte
  `SlotEntry`, `InsertRecord`/`DeleteRecord` (delete = tombstone flag only).
- `Pager`: single file `data.db`, page 0 = `MetaPage` (magic `0x4B565354`,
  `kFormatVersion = 1`, `root_page_id`, `num_pages`, `num_keys`), append-only
  `AllocatePage`, no free list, no cache.
- `BPlusTree`: Get / Put with leaf and internal splits, root split via
  `CreateNewRoot`, leaf sibling links (`next_page`/`prev_page`).
- `DbImpl`: directory + `data.db`, `sync_on_write` option; `Delete` returns
  `Status::Internal("Delete not implemented in MVP")`.
- Error model: `Status` (`kOk, kNotFound, kCorruption, kIoError, kInvalidArgument,
  kInternalError, kBusy, kNoSpace`) and `Result<T> = std::expected<T, Status>`.
- C++23 throughout (deducing this, `std::expected`); Clang builds use libc++.

**Important:** `PageHeader` already reserves `page_lsn` (8 B), `checksum` (4 B),
and `reserved` (8 B); `SlotEntry` has 2 B reserved. The earlier design sketch that
said "add checksum and page_lsn fields" is stale — the fields exist, they are just
never computed, verified, or maintained. Likewise `page.cc` already contains a
constexpr CRC32 table (`kCrc32Table`), currently `[[maybe_unused]]`.

### Known defects found during review (fix in Phase 1, test-first)

1. **Separator-key descent mismatch** — `BPlusTree::FindLeafPath`
   (src/btree/btree.cc:365-376) descends into `GetChild(index)` when the search key
   *equals* a separator key, while `NodeView::FindChild` (src/btree/node.cc:445)
   descends to the right of an equal key. `Get` uses `FindChild`, `Put` uses
   `FindLeafPath`, so a Put of a key equal to a separator (e.g. re-inserting the
   split key after a split) can land in a different leaf than Get searches.
   Invariant to enforce: *left child < K, right child ≥ K, both paths identical*.
2. **`SplitLeaf` drops `prev_page`** — src/btree/btree.cc:432 reinitializes the
   original page and sets `next` but never restores the old `prev_page`; the
   backward leaf chain breaks after the first split of a non-first leaf.
3. **`meta.num_keys` never updated** — `Pager::SetNumKeys` exists but no caller;
   `ApproximateKeyCount()` always returns 0.
4. **`tests/` does not exist** — CMakeLists.txt registers `tests/types_test.cc`,
   `file_test.cc`, `page_test.cc`; configuring with `-DKVSTORE_BUILD_TESTS=ON`
   fails. Nothing in the repo is tested today.
5. **Checksums unused** — `PageHeader::checksum` always 0; corruption on data
   pages is undetectable (only meta magic/version/size are checked at open).
6. **Swallowed I/O errors** — split paths use `std::ignore = pager_.WritePage(...)`
   / `ReadPage` (btree.cc:223-232, 256-278, 441-463, 529-542); a failed write mid-split
   silently corrupts the tree.
7. **Redundant/unconditional syncs** — `BPlusTree::Put` ends every path with
   `pager_.Sync()` regardless of `Options::sync_on_write`, and `DbImpl::Put` syncs
   again. `sync_on_write = false` currently buys nothing.
8. **Internal nodes overload `next_page` as rightmost child**
   (node.h `InitInternal`/`GetRightmostChild`) — works but confuses tooling and
   blocks any future use of sibling links on internal nodes.
9. **Torn-meta-write window** — `AllocatePage` writes the new page, then rewrites
   the meta page, with no ordering or atomicity; a crash between the two (or mid
   meta write) leaves inconsistent `num_pages`/`root_page_id`. Fully fixed by WAL +
   dual meta in Phase 4; Phase 1 only adds detection (checksums) and tests.
10. **O(n) page rebuild per insert** — `NodeView::InsertAt`, `UpdateLeaf`, and
    `InsertInternalAfterSplit` copy every record into `std::vector<std::string>`
    and rebuild the page. Correct but slow; kept until Phase 6 (measure first).

## 2. Target architecture

```
DB API  (Get/Put/Delete/Scan/Begin/Commit)
  ├─ TxnManager + LockManager        (Phase 5, new txn/ module)
  ├─ BPlusTree + Cursor              (Phase 2 delete/scan, Phase 5 latch crabbing)
  ├─ BufferPool + PageGuard + LRU    (Phase 3, new storage/buffer_pool.*)
  ├─ WAL: LogWriter/LogReader        (Phase 4, new wal/ module)
  └─ Pager (meta, allocation, free list) → PosixFile
```

Module ownership after Phase 4: `DbImpl` owns `Pager`, `BufferPool` (holds
`Pager&`), `LogWriter`, `BPlusTree` (holds `BufferPool&`). All tree page access
goes through RAII `PageGuard`s; the tree never calls `WritePage` directly again.

## 3. On-disk format plan (minimize version churn: v1 → v2 → v3)

**v2 (Phase 1, one breaking bump, `MetaPage::kFormatVersion = 2`):**
- `PageHeader.reserved` (8 B) is split into `PageId rightmost_child` (4 B, internal
  nodes stop overloading `next_page`), `uint16_t dead_bytes` (used from Phase 2),
  `uint16_t reserved2`. Header stays 40 bytes; `static_assert` unchanged.
- `PageHeader.checksum` becomes real: CRC32 (existing `kCrc32Table`) over the page
  with the checksum field zeroed; set in `Pager::WritePage`, verified in
  `Pager::ReadPage` → `Status::Corruption`. Meta page gets a checksum carved from
  its reserved padding.
- `MetaPage` gains `PageId free_list_head` (used from Phase 2) from its reserved
  padding; `sizeof(MetaPage) == kPageSize` assert unchanged.
- No migration tool: v1 files are pre-release; opening one reports
  `Corruption("unsupported version")`.

**v3 (Phase 4):** dual meta pages (page 0 and page 1) with `uint64_t generation`,
written alternately; data pages start at page 2. Plus `wal.log` beside `data.db`.

Format is documented in `docs/FORMAT.md` starting in Phase 1, versioned per change.

## 4. Design decisions (defaults; revisit only with data)

| Decision | Choice | Why |
|---|---|---|
| Checksum | CRC32, table already in page.cc | Good-enough detection, zero new deps |
| Page verify toggle | `Options::verify_checksums = true` | Benchmarks need the off switch |
| Delete strategy (P2) | Tombstone slot + `dead_bytes`, compact-on-demand; no merge/rebalance initially | Underflow handling is stretch; document "leaves may go empty" |
| Free list (P2) | On-disk singly linked via freed page's first 4 bytes, head in meta | Simplest reuse; no bitmap needed at this scale |
| Scan API (P2) | `Cursor` (Seek/SeekToFirst/Next) + `DB::Scan(start, end, callback)` | Callback keeps DB API small; cursor stays internal |
| Replacement (P3) | Plain LRU (map + intrusive list), pinned frames unevictable | LRU-K is a documented non-goal until benchmarks say otherwise |
| WAL (P4) | Redo-only physiological log, LSN = byte offset, idempotent redo via `page_lsn`; undo added in Phase 5 | Smallest correct recovery story; matches existing `Lsn`/`page_lsn` fields |
| Meta durability (P4) | Dual meta + generation counter | Fixes torn meta without logging meta writes |
| Concurrency (P5) | Latch crabbing, pessimistic first; strict 2PL with wait-die; MVCC as stretch | Correct before clever |
| Insert perf (P6) | Keep rebuild-based `InsertAt` until profiled | It is the likely hotspot, but measure first |

## 5. Testing & tooling strategy

- GoogleTest via existing FetchContent path; `tests/` created in Phase 1 with
  per-module executables (matches CMakeLists' existing pattern).
- Reference-model property tests: seeded RNG workloads checked against `std::map`
  after every phase (extended with delete/scan in P2, threads in P5).
- Sanitizer presets: CMakePresets with `debug`, `release`, `asan`
  (address+undefined), `tsan` (activated in Phase 5).
- `tools/kvstore_inspect.cc` (Phase 1): read-only dump of meta, tree structure,
  leaf-chain order check. Doubles as the debugging aid for every later phase.
- Crash-injection harness (Phase 4): fork-based kill-child-mid-write tests plus a
  `FailAfterNWrites` hook on `PosixFile` behind a test-only interface.
- The existing `src/main.cc` smoke binary (`kvstore_test` target) gets renamed
  `kvstore_demo` in Phase 1 to free the name for real tests.

## 6. Invariants (checked by `Page::Validate()` / tree checker from Phase 1 on)

- Page: `sizeof(PageHeader) + num_slots*sizeof(SlotEntry) == free_space_start
  <= free_space_end <= kPageSize`; every live slot's `[offset, offset+length)` lies
  within `[free_space_end, kPageSize)` and live slots do not overlap;
  `header.page_id` matches the ID it was read as; checksum verifies.
- Tree: keys strictly ascending within every node; for internal entry `(K_i, C_i)`,
  all keys under `C_i` < `K_i` and all keys under `rightmost_child` ≥ last key;
  leaf chain visits all leaves in ascending key order and `prev` mirrors `next`;
  Get and Put descend to the same leaf for every key (defect #1).
- Meta: `num_pages * kPageSize <= file size`; `root_page_id < num_pages` or
  invalid; after Phase 2, free-list pages are typed `kFree` and unreachable from
  the root; after Phase 1, `num_keys` equals live key count.
- WAL (Phase 4): no page with `page_lsn > flushed_lsn` is ever written to
  `data.db` (the write-ahead rule — tested explicitly).

## 7. Top risks

1. **Split correctness debt** (defects #1, #2, #6) — must be fixed and pinned by
   tests before any refactor sits on top; otherwise Phase 3's refactor inherits
   silent corruption.
2. **Format churn** — mitigated by batching all v2 field changes into one Phase 1
   bump and reserving `dead_bytes`/`free_list_head` before their features land.
3. **Buffer-pool refactor breadth** (Phase 3) — every `ReadPage/WritePage` call in
   btree.cc changes; the tiny-pool (4-frame) regression run of the full P1–P2 suite
   is the gate. Splits hold up to 3 guards + path; document minimum pool size.
4. **WAL subtleties** (Phase 4) — torn tail records, idempotent redo, allocation
   records interleaved with page redo. Crash harness with 500 seeded runs is the
   acceptance bar, not code review.
5. **Scope creep in Phase 5/6** — MVCC and prefix truncation are explicitly
   stretch; each phase exits through its checkpoint, not through feature count.
