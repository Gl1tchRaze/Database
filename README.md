# Mini Database Engine

A small disk-backed database engine written in C++17, built up in stages. Each
stage adds one real database subsystem and each is a standalone program you can
compile and run on its own. Stage 4 puts them all together into one engine.

Everything lives in header files (`.hpp`); each stage has its own `main_stageN.cpp`
driver.

## The stages

| Stage | Program | What it adds | Key files |
|-------|---------|--------------|-----------|
| 1 | `main.cpp` | A raw **pager**: read/write 4 KB pages of a file | `pager.hpp` |
| 2 | `main_stage2.cpp` | **Slotted pages** + a heap **Table** of variable-length records | `slotted_page.hpp`, `table.hpp` |
| 3 | `main_stage3.cpp` | A **hash index** for O(1) point lookups | `Indexed_table.hpp` |
| 3b | `main_stage3b.cpp` | A **B+ tree** index: point lookups *and* sorted range scans | `bplus_tree.hpp` |
| 3c | `buffer_pool.hpp` | An **LRU buffer pool**: page cache with pinning & dirty tracking | `buffer_pool.hpp` |
| 4 | `main_stage4.cpp` | **Capstone**: all of the above in one engine | `db_engine.hpp` |

Stage 3c has no driver of its own; it is exercised by Stage 4.

## How it fits together

The layers stack cleanly, from the disk up:

```
        DBEngine / Table / IndexedTable      <- keys, values, queries
                     |
             BPlusTree  (in-memory index)    <- ordered key -> RecordId
                     |
   SlottedPage   (variable-length records)   <- insert / get / erase / compact
                     |
   BufferPool    (LRU cache, dirty pages)    <- fetch / unpin, in Stage 4
                     |
   Pager         (fixed 4 KB pages on disk)  <- read_page / write_page
                     |
                  your .db file
```

A record is addressed by a **RecordId** — `{page_id, slot_id}` — the same idea as
a real database's page/slot tuple id.

### Page format (`slotted_page.hpp`)

A 4 KB page holds a header followed by a slot array growing forward from the
front, with record bytes packed backward from the end:

```
[ num_slots(2) | free_end(2) | live_bytes(2) | slot0 | slot1 | ... | free | ... rec1 | rec0 ]
                                              (off,len)
```

Deletion is handled with **tombstones**: a deleted slot keeps its number but its
length is set to 0, and its bytes are *not* moved — so every other `RecordId` on
the page stays valid. The freed space is reused by the next insert, and if the
tombstone holes are needed to fit a new record the page is **compacted** first
(live records are re-packed; slot numbers, and thus RecordIds, do not change).

### Buffer pool (`buffer_pool.hpp`)

A fixed-capacity cache of pages sitting in front of the pager. `fetch(id)` pins a
page and returns a pointer; `unpin(id, dirty)` releases it. When the pool is full
it evicts the least-recently-used *unpinned* page, writing it back first if dirty.
`flush_all()` (and the destructor) write every dirty page to disk. It reports
hits, misses, disk reads and disk writes, which is what makes the cache effect
visible in the Stage 4 demo.

### B+ tree (`bplus_tree.hpp`)

An in-memory ordered index, `int key -> RecordId`. Internal nodes route; leaves
hold the pairs and are linked left-to-right so `range(lo, hi)` walks the leaf
chain in sorted order — something a hash index cannot do. It supports
`insert`, `find`, `range`, and `remove`. For a small fanout, `dump()` prints the
tree level by level so you can see splits happen.

Note on deletion: `remove()` drops the key from its leaf but does **not** merge or
rebalance leaves (underfull leaves are tolerated). This keeps correctness — the
parent separators still partition the key space identically, so lookups and range
scans keep working — while keeping the implementation small. Only balance can
degrade, never correctness.

### The Stage 4 engine (`db_engine.hpp`)

`DBEngine` wires all of them together. Every page access goes through the
`BufferPool`, records live in slotted pages, and a B+ tree gives point lookups
(`find`) and ordered range scans (`range`). It also supports `insert`, `erase`
and `update` (delete-then-insert). Records are stored as `"key,value"`; the index
is rebuilt from the heap on open, so data survives closing and reopening.

Inserts use a **first-fit** search that tries the last page, then every other
page, before allocating a new one — this is what actually reuses space freed by
deletes.

## Building

You need a C++17 compiler. On Windows with MinGW (the setup this project's
`.vscode` config assumes):

```
build.bat
```

On Linux/macOS/WSL:

```
chmod +x build.sh
./build.sh
```

Both scripts compile each stage with `-O2` and the same strict warning set the
editor uses (`-Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wcast-align
-Wconversion -Wsign-conversion -Wnull-dereference`). The code is warning-clean
under all of them.

To build a single stage by hand:

```
g++ -std=c++17 -O2 -Wall -Wextra main_stage4.cpp -o main_stage4
```

## Running

Each program deletes and rebuilds its own `.db` file, so you can run them
repeatedly in any order:

```
./main_stage4        # capstone: the whole engine
./main_stage3b       # B+ tree, including a visible small tree
```

## What the Stage 4 demo shows

1. Insert 5,000 records and report pages used and B+ tree height.
2. Point lookups through the index.
3. An ordered range scan.
4. Delete half the records, then re-insert them — the page count does **not**
   grow, proving freed space is reused.
5. An update.
6. Close and reopen the file — data and range scans still correct (durability).
7. The same 20,000 random lookups with cache sizes of 1, 8, 64 and 256 pages,
   showing disk reads collapse as the cache grows (roughly 19,400 reads with a
   1-page cache down to ~37 with 64, and a big speed-up in wall-clock time).

## Files

- `pager.hpp` — fixed-size page I/O
- `slotted_page.hpp` — variable-length records in a page
- `table.hpp` — heap of pages + `RecordId`
- `Indexed_table.hpp` — hash-indexed table
- `bplus_tree.hpp` — B+ tree index
- `buffer_pool.hpp` — LRU page cache
- `db_engine.hpp` — the integrated engine
- `main*.cpp` — one driver per stage
