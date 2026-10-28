# PagerDB — Architecture

How the pieces fit together as of Phase 6 (single-threaded engine with a
buffer pool, write-ahead logging, and crash recovery). Companion documents:
`FORMAT.md` (byte-level on-disk layout), `DESIGN_DECISIONS.md` (why these
shapes and not others), `BENCHMARKS.md` (measured behavior).

## Layering

```
DB (include/kvstore/db.h)  — public API: Get/Put/Delete/Contains/Scan/Sync/Checkpoint
 └─ DbImpl (src/db.cc)     — owns everything below; sync & checkpoint policy
      ├─ wal::LogWriter    — append-only wal.log, buffered, FlushTo(lsn)
      ├─ BPlusTree         — tree logic; emits WAL records for its mutations
      │    ├─ Cursor       — leaf-chain iterator backing Scan
      │    └─ NodeView     — typed view over a Page (leaf/internal records)
      └─ BufferPool        — frame cache, RAII PageGuards, write-ahead rule
           └─ Pager        — page-granular file I/O, dual meta, free list, checksums
                └─ PosixFile — RAII fd, positioned read/write, Sync
```

Rules that keep the layering honest:

- The tree never touches the data file directly; every page access goes
  through a `PageGuard` from the buffer pool. Only the `Pager` calls
  `PosixFile` on `data.db`; only the `LogWriter`/`LogReader` touch `wal.log`.
- The `Pager` stamps and verifies page checksums; nothing above it thinks
  about CRCs. The buffer pool is the single enforcement point for the
  write-ahead rule; nothing above it thinks about flush ordering.
- `DbImpl` owns policy (when to fsync, when to checkpoint); `BPlusTree` owns
  mechanism (what to log, which pages to mutate).

## Error model

Every fallible call returns `Status` / `Result<T>` (`std::expected`); there
are no exceptions. Errors propagate unmodified up the stack — an I/O error
in a split surfaces from `Put` as `Internal`/`IoError`, and Phase 4's
redo-only limitation means a failed write op poisons the handle (subsequent
writes get `Internal`); reopening recovers the last consistent state.

## Lifecycles

### Get(key)

1. `BPlusTree::Get` reads `root_page_id` from the pager's cached meta.
2. Descend: fetch page via `BufferPool::FetchPage` (hit: bump LRU, pin;
   miss: evict an unpinned frame — writing it back first if dirty, WAL rule
   observed — then `Pager::ReadPage` into the frame, CRC verified).
3. `NodeView::FindChild` binary-searches separators (equal key → right
   subtree) until a leaf; `FindKeyIndex` binary-searches the leaf.
4. Value bytes are copied out before the guard drops (pins released).

### Put(key, value)

1. `BPlusTree::Put` walks to the target leaf with `FindLeafPath` (same
   descent rule as Get — defect #1's fix), remembering the path.
2. No-split case: `NodeView::InsertLeaf`/`UpdateLeaf` mutates the leaf page
   in place (slot-shift insert since Phase 6), a `kPut` record goes to the
   WAL buffer, and the frame's `page_lsn` is stamped with the record's LSN.
   The frame is marked dirty — **no data-page write happens on the Put path**.
3. Split case: leaf (and possibly ancestors) are rewritten; every rewritten
   page is logged as a full `kPageImage` after-image, `kAllocPage` records
   cover new pages, `kRootChange` covers root moves. The last record of the
   operation carries the op-end bit so recovery applies the group
   all-or-nothing.
4. `DbImpl` then applies policy: `sync_on_write=true` → `LogWriter::FlushTo`
   (fdatasync/fsync on the log — this is the durability point); the data
   pages ride along later. If the WAL exceeds `max_wal_bytes`, checkpoint.

### Eviction and the write-ahead rule

Before **any** dirty frame reaches `data.db` (eviction, `FlushPage`,
`FlushAll`), the pool calls `LogWriter::Flush()` — everything appended so
far becomes durable: no page whose changes aren't durable in the log can
overwrite its on-disk ancestor. The flush deliberately covers the *whole*
log, not just the frame's `page_lsn`: recovery drops record groups whose
op-end record isn't durable, so the durable log must always contain the
complete group of an evicted page's last mutation. The tree's side of the
contract is pinning — every page an operation rewrites stays pinned until
the group's op-end record is appended, so an evictable page's group is
always fully in the buffer when the flush runs. This pair of rules is what
makes "redo from the log" always sufficient.

### Checkpoint

`FlushAll` (WAL rule per frame) → fsync `data.db` → append `kCheckpoint` →
write meta (alternating dual meta page, bumped generation, new
`checkpoint_lsn`) → truncate `wal.log`. Runs on manual `Checkpoint()`, on
WAL growth past `Options::max_wal_bytes`, and on clean close.

### Recovery (inside DB::Open)

1. `Pager::Open` reads both meta pages (0 and 1), keeps the checksum-valid
   one with the higher generation (a torn meta write loses at most the
   newest of the two copies).
2. `LogReader` scans `wal.log` from `meta.checkpoint_lsn`. A missing/CRC-
   invalid record marks the clean end of log (torn tail).
3. Records are grouped by op-end bit; incomplete trailing groups are
   discarded. Within complete groups, redo applies **iff
   `record.lsn > page.page_lsn`** (idempotent — recovering twice is a
   no-op). `kAllocPage` extends the file before any redo touches the new
   page; full-page images reconstruct pages whose in-place write tore.
4. The rebuilt state is exactly "every operation whose log records were
   durable, and nothing after" — verified by the fork-based crash harness
   (`tests/crash_test.cc`, 500 seeded runs for the C4 gate).

### Scan(start, end, callback)

`BPlusTree::Seek` positions a `Cursor` at the lower bound; the cursor copies
the current leaf out of the pool (holding no pins between steps) and follows
`next_page` sibling links, invoking the callback per entry until `end`, the
callback returns false, or the chain ends.

## Modules in one paragraph each

**PosixFile** (`storage/file.h`): move-only RAII fd; `pread`/`pwrite`
positioned I/O; `Sync` is `fsync` (macOS note: `F_FULLFSYNC` is documented
but not default — see DESIGN_DECISIONS); test hooks `TotalSyncs()` and
`FailAfterNWrites(n)` power the sync-policy tests and the crash harness.

**Page / PageHeader / SlotEntry** (`storage/page_format.h`): 4 KB slotted
page; 40-byte header (type, LSN, free-space bounds, sibling links, CRC-32,
`rightmost_child`, `dead_bytes`); slot directory grows forward, record heap
grows backward. `InsertRecord`/`InsertRecordAt`/`DeleteRecord`/`Compact`,
plus `Validate()` (structural invariants) and `Dump()`.

**Pager** (`storage/pager.h`): opens/creates `data.db`; dual meta pages
(format v3) with magic/version/root/num_pages/num_keys/free-list
head/checkpoint LSN/generation; allocation pops the free list (LIFO,
linked through freed pages' first bytes) before extending the file; stamps
CRC on write, verifies on read (`Options::verify_checksums`).

**BufferPool + PageGuard + LruReplacer** (`storage/buffer_pool.h`): fixed
frame array (`Options::buffer_pool_frames`, min 4), page table, LRU over
unpinned frames, hit/miss/eviction counters surfaced as `DB::GetStats()`.
Guards are move-only RAII pins; `MarkDirty()` is the only mutation
bookkeeping the tree does. Phase 5.1 added per-frame `shared_mutex` content
latches and `FetchPageRead/FetchPageWrite` guard variants; the tree crabs
with them (reads share, writes exclude), and plain unlatched `FetchPage`
remains for single-threaded callers (recovery, old tests). The pool-level
`root_latch` (shared_mutex) guards `root_page_id`: every structural change
runs under it exclusively, so writers holding it shared see a frozen tree
shape (the basis of Put's optimistic fast path).

**BPlusTree + NodeView + Cursor** (`btree/`): order-agnostic (page-space
bound) B+ tree; leaves hold `[key_len][key][val_len][value]` records,
internals hold `[key_len][key][child]` with a separate `rightmost_child`;
splits move the upper half right and push the middle key up; leaf sibling
links maintain a doubly-linked chain for scans. Deletes shift the slot
directory and count `dead_bytes`; `EnsureFit` compacts on demand. No
underflow merge yet (documented decision — empty leaves may persist).

**WAL** (`wal/`): `LogRecord` (format in `log_record.h`), buffered
`LogWriter` with LSN = byte offset in `wal.log`, `LogReader` for recovery.
Physiological redo-only records; op-end grouping; full-page image on a
page's first mutation after a checkpoint (torn-page protection).

**DbImpl** (`src/db.cc`): glues the above; enforces `sync_on_write`,
auto-checkpoint threshold, clean-close checkpoint; `kvstore_inspect`
(`tools/`) is the read-only structure/integrity dumper used by tests and
debugging.

## Invariants (enforced, not aspirational)

The full list lives in `plan.md` §6; the enforcement points are
`Page::Validate()` (asserted after every NodeView mutation in debug builds),
the tree/leaf-chain/free-list walker in `kvstore_inspect` (exit code gates
test scripts), checksum verification on every page read, and the crash
harness for the WAL rule ("no page with `page_lsn > flushed_lsn` reaches
`data.db`").
