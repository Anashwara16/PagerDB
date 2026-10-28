# PagerDB On-Disk Format

**Current version: 3** (`MetaPage::kFormatVersion`, include/kvstore/storage/pager.h)

All multi-byte integers are little-endian (record encodings are explicitly LE;
header structs are host-endian — PagerDB files are not portable across
endianness, which is acceptable for this project and documented here).

## File layout

A database is a directory containing the data file and (from v3) the WAL:

```
<db>/data.db   — array of fixed 4096-byte pages
<db>/wal.log   — write-ahead log (truncated at every checkpoint)
```

| Page | Contents |
|------|----------|
| 0 | Meta page copy A (`MetaPage`) |
| 1 | Meta page copy B (`MetaPage`) |
| 2… | Data pages (B+ tree nodes, free pages) |

Page *N* lives at byte offset `N * 4096`. `PageId` is `uint32`, so the file can
address 16 TB. `kInvalidPageId = 0xFFFFFFFF` means "no page".

## Meta pages (pages 0 AND 1, format v3) — `MetaPage`, 4096 bytes each

v3 keeps two meta copies. Every persist bumps `generation` and writes to
page `generation % 2`, alternating, so a torn meta write can destroy at most
the newest copy — open reads both, validates each (magic, version,
checksum), and uses the valid copy with the higher generation. Data pages
start at page 2.

Meta persistence is deferred (v3): setters update memory only; the meta is
written by `Pager::Sync()` / checkpoints / clean close. The WAL covers the
gap after a crash.

| Offset | Size | Field | Notes |
|-------:|-----:|-------|-------|
| 0 | 4 | `magic` | `0x4B565354` ("KVST") |
| 4 | 4 | `version` | must equal 3 |
| 8 | 4 | `root_page_id` | `kInvalidPageId` when the tree is empty |
| 12 | 4 | `num_pages` | total pages in file, including both meta pages |
| 16 | 8 | `num_keys` | live key count (recounted during recovery) |
| 24 | 4 | `free_list_head` | head of freed-page list; `kInvalidPageId` if empty |
| 28 | 4 | `checksum` | CRC32 of the whole meta page with this field as zero |
| 32 | 8 | `generation` | v3: bumped on every persist; higher valid copy wins at open |
| 40 | 8 | `checkpoint_lsn` | v3: WAL replay starts at this LSN |
| 48 | 4048 | `reserved` | zero |

Per-copy open-time validation: magic, version, checksum (when
`Options::verify_checksums`, default on); then `file_size >= num_pages *
4096` for the chosen copy.

## Page header — `PageHeader`, 40 bytes, at offset 0 of every non-meta page

| Offset | Size | Field | Notes |
|-------:|-----:|-------|-------|
| 0 | 4 | `page_id` | must match the page's position in the file |
| 4 | 1 | `type` | `PageType`: 0 meta, 1 internal, 2 leaf, 3 free, 4 overflow |
| 5 | 1 | `flags` | reserved, zero |
| 6 | 2 | `num_slots` | slot-directory entry count |
| 8 | 8 | `page_lsn` | LSN of last logged modification (0 until Phase 4 WAL) |
| 16 | 2 | `free_space_start` | first free byte after the slot directory; invariant: `== 40 + num_slots*8` |
| 18 | 2 | `free_space_end` | one past the last free byte, records start here |
| 20 | 4 | `next_page` | leaf pages only: next leaf in key order |
| 24 | 4 | `prev_page` | leaf pages only: previous leaf in key order |
| 28 | 4 | `checksum` | CRC32 of the whole page with this field as zero |
| 32 | 4 | `rightmost_child` | internal pages only: child for keys ≥ last key (v2: no longer aliased onto `next_page`) |
| 36 | 2 | `dead_bytes` | record bytes held by deleted entries, reclaimable by compaction (see below) |
| 38 | 2 | `reserved2` | zero |

`dead_bytes` semantics (Phase 2): counts **record bytes only** — slot bytes are
not included because a delete shifts the slot directory and reclaims its 8
bytes immediately. Deviation from the original task note ("record length +
slot size"): including slot bytes would overstate reclaimable space and let a
compact-then-insert believe a record fits when it does not.
`FreeSpace() + dead_bytes` is exactly the free space after `Page::Compact()`.
The counter is owned by the delete paths (`NodeView::DeleteAt`,
`Page::DeleteRecord`); any full page rebuild (insert/update) packs the records
and resets it to zero.

Checksum algorithm: standard CRC-32 (polynomial `0xEDB88320`, init/final-xor
`0xFFFFFFFF`) over all 4096 bytes with the 4 checksum bytes treated as zero.
Stamped on every `Pager::WritePage` / meta write; verified on every read unless
`Options::verify_checksums` is off. A page that is entirely zero (allocated,
never written) is exempt from verification.

## Slot directory — `SlotEntry`, 8 bytes each, starting at offset 40

| Offset | Size | Field |
|-------:|-----:|-------|
| 0 | 2 | `offset` — record start, from page start |
| 2 | 2 | `length` — record length in bytes |
| 4 | 2 | `flags` — bit 0: deleted (tombstone) |
| 6 | 2 | `reserved` |

Slots grow forward from the header; record data grows backward from the end of
the page. The page is full when they meet. Slot order is key order (ascending).

## Record encodings (inside leaf/internal pages)

Leaf record (one per key-value pair):

```
[key_len:2 LE][key:key_len][value_len:2 LE][value:value_len]
```

Internal record (one per separator key):

```
[key_len:2 LE][key:key_len][child:4 LE]
```

`child` holds keys `< key`; the header's `rightmost_child` holds keys `≥` the
last separator. Tree invariant: for entry *i*, all keys under `child_i` are
`< key_i`, and lookups descend right on equality.

## Free pages (`PageType::kFree`, Phase 2)

`Pager::FreePage` rewrites a page as type `kFree` with a normal (checksummed)
header and the previous `free_list_head` in the first 4 payload bytes:

```
[PageHeader:40][next_free:4 LE][zeros to end of page]
```

`MetaPage::free_list_head` points at the most recently freed page (LIFO);
`kInvalidPageId` terminates the list. `Pager::AllocatePage` pops the head if
the list is non-empty (re-zeroing the page on disk so it reads like a fresh
allocation) and only extends the file when the list is empty. Free pages must
be acyclic and unreachable from the root (`kvstore_inspect` verifies both).
In Phase 2 the tree itself never frees pages — emptied leaves stay in the
sibling chain (decision 2.8); underflow handling is the future consumer.

## Version history

| Version | Change |
|---------|--------|
| 1 | Initial MVP format. Header `reserved` (8B) unused; internal nodes stored `rightmost_child` in `next_page`; no checksums; meta had no `free_list_head`/`checksum`. |
| 2 | Header `reserved` split into `rightmost_child`/`dead_bytes`/`reserved2`; meta gains `free_list_head` + `checksum`; CRC32 stamped and verified on all pages. v1 files are rejected (`Corruption: unsupported version`) — no migration, v1 was pre-release. |
| 3 | Dual meta pages (0 and 1) with `generation` + `checkpoint_lsn`; data pages start at page 2; `wal.log` beside `data.db`. v2 files are rejected (their page 1 is a data page, never a valid meta copy). |

Phase 6 changed no bytes: the CRC-32 implementation moved to ARMv8 hardware
instructions / slicing-by-8 tables, but both compute the same reflected-
0xEDB88320 function, so checksums written by any version verify under any
other (cross-checked implementation-vs-implementation in Phase 6).

## Write-ahead log (`wal.log`, Phase 4)

Record layout (all integers little-endian):

```
[lsn:8][txid:8][type+flags:1][page_id:4][key_len:2][val_len:2][key][value][crc:4]
```

- **LSN** = a global byte offset: `base + offset in file`, where the base is
  the `checkpoint_lsn` at the last truncation. LSNs stay monotonic across
  checkpoint truncations; `page_lsn` in page headers refers to this space.
- **crc** = CRC-32 over all preceding bytes of the record. A missing or
  CRC-invalid record is a *clean end of log* (torn tail), not an error.
- **type+flags**: low 7 bits = record type; high bit (`0x80`) = *op-end* —
  this record completes a logical operation. Recovery applies records in op
  groups, all-or-nothing: a group whose op-end record never made it to disk
  (crash after a mid-op WAL flush) is discarded entirely, keeping multi-page
  structural changes atomic. Discarding is only sound if none of the group's
  pages reached `data.db`; two runtime rules guarantee that: (1) the tree
  keeps every page an operation rewrites **pinned** (unevictable) until the
  group's op-end record is appended, and (2) the buffer pool's write-ahead
  flush covers the **whole appended log** — not just the victim's `page_lsn`
  — so any flush that lets a page reach disk also makes its group's op-end
  durable.
- **Deviation from the original task sketch** (documented intentionally):
  the 4-byte `page_id` field and the `kPageImage` type were added so records
  are *physiological* — each applies to exactly one page, guarded by that
  page's `page_lsn` (`redo iff record.lsn > page_lsn`). Purely logical
  records cannot be replayed safely into a partially flushed multi-page
  split.

Types: `kPut(1)`/`kDelete(2)` — single-leaf deltas; `kAllocPage(3)` — redo
extends the file; `kFreePage(4)` — reserved, never emitted yet;
`kRootChange(5)` — new root id for the meta; `kCheckpoint(6)` — marker;
`kPageImage(7)` — full 4096-byte after-image (value field), used for every
page a structural change rewrites **and** for the first mutation of any page
after a checkpoint (*full-page writes*: a torn data-page write destroys the
base state deltas would need; the image, already durable by the write-ahead
rule, reconstructs the page during recovery).

The write-ahead rule: before a dirty frame reaches `data.db` (eviction,
FlushPage, FlushAll), the buffer pool flushes **everything appended to the
log so far**. Flushing only through the frame's own `page_lsn` is not
enough: a concurrent commit can flush the log mid-op-group, covering the
frame's LSN but not the group's later op-end record, turning a bounded
flush into a no-op — the page would land on disk while recovery (which
drops op-end-less groups) could never repair it. Combined with the pinning
rule above, a whole-log flush always carries the op-end of any evictable
page's group. Checkpoint order: flush log → flush dirty pages → fsync data
→ append+flush `kCheckpoint` → persist meta (new `checkpoint_lsn`) + fsync
→ truncate `wal.log`.

macOS note: `PosixFile::Sync` uses `fsync`, which on macOS does not
guarantee platter durability; `fcntl(F_FULLFSYNC)` does but is drastically
slower. Left as a future build-time/Options choice.
