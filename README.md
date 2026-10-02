# Mini Database Engine

A small disk-backed database engine written in C++17, built up in stages. Each
stage adds one real database subsystem and each is a standalone program you can
compile and run on its own. Stage 4 puts them all together into one engine, and
`main_repl` lets you type commands at it interactively.

Everything lives in header files (`.hpp`); each stage has its own `main_stageN.cpp`
driver.

## Quick start

```
build.bat            REM Windows (MinGW).  Linux/macOS/WSL:  ./build.sh
main_repl.exe
```

Then type commands at the `db>` prompt:

```
db> insert 1 apple
ok
db> insert 2 banana
ok
db> find 2
2 = banana
db> range 1 5
  1 = apple
  2 = banana
(2 records)
db> exit
```

## The stages

| Stage | Program | What it adds | Key files |
|-------|---------|--------------|-----------|
| 1 | `main.cpp` | A raw **pager**: read/write 4 KB pages of a file | `pager.hpp` |
| 2 | `main_stage2.cpp` | **Slotted pages** + a heap **Table** of variable-length records | `slotted_page.hpp`, `table.hpp` |
| 3 | `main_stage3.cpp` | A **hash index** for O(1) point lookups | `Indexed_table.hpp` |
| 3b | `main_stage3b.cpp` | A **B+ tree** index: point lookups *and* sorted range scans | `bplus_tree.hpp` |
| 3c | `buffer_pool.hpp` | An **LRU buffer pool**: page cache with pinning & dirty tracking | `buffer_pool.hpp` |
| 4 | `main_stage4.cpp` | **Capstone**: all of the above in one engine | `db_engine.hpp` |
| — | `main_repl.cpp` | **Interactive prompt** over the Stage 4 engine | `db_engine.hpp` |

Stage 3c has no driver of its own; it is exercised by Stage 4 and the REPL.

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

The file deliberately avoids C++17-only *syntax* (structured bindings, `if`
init-statements) so that it still parses if the `-std=c++17` flag is ever
missing — see Troubleshooting below for why that matters.

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

Both scripts compile every program with `-O2` and the same strict warning set the
editor uses (`-Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wcast-align
-Wconversion -Wsign-conversion -Wnull-dereference`). The code is warning-clean
under all of them.

To build a single program by hand — note the `-std=c++17`, which is required:

```
g++ -std=c++17 -O2 main_stage4.cpp -o main_stage4
```

## Running

### Interactively (`main_repl`)

`main_repl` is the way to drive the engine yourself. Start it and type commands
at the `db>` prompt:

```
./main_repl                 # data goes in repl.db
./main_repl mydata.db       # ...or a file you name
```

| Command | What it does |
|---------|--------------|
| `insert <key> <value>` | Store a record (key is a whole number, must be unique) |
| `find <key>` | Look up one key |
| `update <key> <value>` | Change an existing key's value |
| `erase <key>` | Delete a key |
| `range <lo> <hi>` | List every key from `lo` to `hi`, in sorted order |
| `count` | How many records are stored |
| `stats` | Pages used, cache hit rate, disk reads/writes |
| `help` | Show the command list |
| `exit` | Save and quit |

Records persist: quit, start it again, and your data is reloaded from the file
(the B+ tree index is rebuilt from the heap on open). Delete `repl.db` to start
over.

### The fixed demos

Each stage program deletes and rebuilds its own `.db` file, so you can run them
repeatedly in any order:

```
./main_stage4        # capstone: the whole engine
./main_stage3b       # B+ tree, including a visible small tree
./tests              # the correctness suite
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

## Tests

```
./tests
```

Four groups, checking behaviour rather than just absence of crashes: slotted-page
insert/erase/tombstone-reuse/compaction/capacity; a 100,000-operation random
insert+erase churn verified byte-for-byte against a reference `std::map`; B+ tree
`remove` keeping every surviving key findable and ranges sorted; and a full
`DBEngine` round-trip through close-and-reopen. It prints `ALL TESTS PASSED`.

## Troubleshooting

**`error: expected unqualified-id before '[' token`** (or a cascade of
`expected ';'`, `was not declared in this scope`, `expected primary-expression`)

The compiler is not in C++17 mode. The code is correct; the `-std=c++17` flag is
missing from whatever invoked the compiler. Build with `build.bat` / `./build.sh`,
which pass it. If the error appears when using the VS Code **C/C++ Runner**
extension's Run button, that extension has its own settings — set both of these
in `.vscode/settings.json` (they default to empty):

```json
"C_Cpp_Runner.cStandard": "c17",
"C_Cpp_Runner.cppStandard": "c++17",
```

For the editor's red squiggles (IntelliSense, a separate engine again), set
`"cppStandard": "c++17"` in `.vscode/c_cpp_properties.json`, then run
"Developer: Reload Window".

**`warning:` lines, but the build finishes**

Warnings are not errors. If the last line says `All stages built`, you have
working executables. The project is warning-clean under the flag set above, but a
newer compiler than the one it was last checked against may surface additional
warnings.

**Crashes, `std::invalid_argument`, or garbage on startup**

Almost always a stale `.db` file from an older page format. The page header is 6
bytes (`num_slots`, `free_end`, `live_bytes`); files written before `live_bytes`
existed are not readable. Delete the `*.db` files and re-run.

## Files

- `pager.hpp` — fixed-size page I/O
- `slotted_page.hpp` — variable-length records in a page, with delete + compaction
- `table.hpp` — heap of pages + `RecordId`
- `Indexed_table.hpp` — hash-indexed table
- `bplus_tree.hpp` — B+ tree index
- `buffer_pool.hpp` — LRU page cache
- `db_engine.hpp` — the integrated engine
- `main*.cpp` — one driver per stage, plus `main_repl.cpp` (interactive)
- `tests.cpp` — correctness suite
- `build.bat` / `build.sh` — build every program with the strict flag set
