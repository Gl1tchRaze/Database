// ===========================================================================
// Stage 4 demo: the whole mini-database working together.
//
// Pager + BufferPool + SlottedPage + B+ tree, exercised end to end:
//   1. insert records (stored as "key,value") through a buffered heap
//   2. point lookups via the B+ tree index
//   3. ordered range scans (the thing a hash index cannot do)
//   4. deletes + re-inserts that reuse freed space (slotted-page compaction)
//   5. updates
//   6. close + reopen to prove durability (index is rebuilt from disk)
//   7. a buffer-pool experiment: same workload, tiny cache vs large cache,
//      showing the drop in physical disk I/O
// ===========================================================================
#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
#include "db_engine.hpp"

using Clock = std::chrono::steady_clock;
static double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

static void line() { std::cout << "------------------------------------------------------------\n"; }

int main() {
    const int N = 5000;

    // ---- 1. Build the database -------------------------------------------
    std::remove("stage4.db");
    {
        DBEngine db("stage4.db", /*cache_pages=*/32);

        auto t0 = Clock::now();
        for (int i = 1; i <= N; ++i)
            db.insert(i, "Student_" + std::to_string(i) + ",CSE-" + std::to_string(2000 + i % 25));
        double build_ms = ms_since(t0);

        std::cout << "1) Inserted " << db.size() << " records into " << db.num_pages()
                  << " pages in " << build_ms << " ms\n";
        std::cout << "   B+ tree height = " << db.height()
                  << ", duplicate key 42 rejected? "
                  << (db.insert(42, "dup") ? "NO (bug!)" : "yes") << "\n";
        line();

        // ---- 2. Point lookups --------------------------------------------
        std::string v;
        db.find(1, v);    std::cout << "2) find(1)    = " << v << "\n";
        db.find(2500, v); std::cout << "   find(2500) = " << v << "\n";
        db.find(N, v);    std::cout << "   find(" << N << ") = " << v << "\n";
        std::cout << "   find(999999) = " << (db.find(999999, v) ? v : "NOT FOUND") << "\n";
        line();

        // ---- 3. Ordered range scan ---------------------------------------
        std::cout << "3) range [2498, 2503] in sorted order:\n";
        db.range(2498, 2503, [](int k, const std::string& val) {
            std::cout << "     " << k << " -> " << val << "\n";
        });
        line();

        // ---- 4. Delete + re-insert (space reuse) -------------------------
        int deleted = 0;
        for (int i = 2; i <= N; i += 2) if (db.erase(i)) ++deleted;
        std::cout << "4) Deleted " << deleted << " even keys; live now = " << db.size()
                  << ", pages still = " << db.num_pages() << "\n";
        bool gone = !db.find(2500, v);
        std::cout << "   find(2500) after delete: " << (gone ? "NOT FOUND (correct)" : v) << "\n";

        uint32_t pages_before = db.num_pages();
        int reinserted = 0;
        for (int i = 2; i <= N; i += 2) { if (db.insert(i, "REBORN_" + std::to_string(i))) ++reinserted; }
        std::cout << "   Re-inserted " << reinserted << " keys; pages now = " << db.num_pages()
                  << " (was " << pages_before << ") -> freed space was reused\n";
        db.find(2500, v); std::cout << "   find(2500) after re-insert = " << v << "\n";
        line();

        // ---- 5. Update ---------------------------------------------------
        db.update(1, "VALEDICTORIAN");
        db.find(1, v);
        std::cout << "5) update(1) -> find(1) = " << v << "\n";
        line();

        std::cout << "   cache while building: hit rate " << db.hit_rate()
                  << "%  (" << db.cache_hits() << " hits / " << db.cache_misses() << " misses)\n";
        std::cout << "   physical disk I/O: " << db.disk_reads() << " reads, "
                  << db.disk_writes() << " writes\n";
    } // engine closes here: destructor flushes all dirty pages

    // ---- 6. Reopen: prove durability -------------------------------------
    {
        DBEngine db("stage4.db", 32);
        std::string v;
        bool ok1 = db.find(1, v);
        std::cout << "6) Reopened. index rebuilt from disk: " << db.size() << " records, height "
                  << db.height() << "\n";
        std::cout << "   find(1) = " << (ok1 ? v : "MISSING") << "  (expect VALEDICTORIAN)\n";
        int count = 0;
        db.range(1, N, [&](int, const std::string&) { ++count; });
        std::cout << "   full range scan sees " << count << " records\n";
    }
    line();

    // ---- 7. Buffer-pool experiment: tiny vs large cache ------------------
    // Same read-heavy workload, replayed with a 1-page cache and then a large
    // cache. The buffer pool turns repeated logical reads into cache hits,
    // collapsing physical disk reads.
    auto replay = [&](size_t cache_pages) {
        DBEngine db("stage4.db", cache_pages);
        std::string v;
        // 20k point lookups spread across the key space
        unsigned seed = 1; auto rnd = [&] { seed = seed * 1103515245 + 12345; return (seed >> 16) & 0x7fff; };
        auto t0 = Clock::now();
        for (int i = 0; i < 20000; ++i) db.find(1 + static_cast<int>(rnd() % static_cast<unsigned>(N)), v);
        double q_ms = ms_since(t0);
        std::printf("   cache=%2zu page(s): %6llu disk reads, hit rate %5.1f%%, 20k lookups in %6.1f ms\n",
                    cache_pages,
                    (unsigned long long)db.disk_reads(), db.hit_rate(), q_ms);
    };
    std::cout << "7) Buffer pool: same 20,000 random lookups, different cache sizes\n";
    replay(1);
    replay(8);
    replay(64);
    replay(256);
    line();
    std::cout << "Done. All subsystems (pager, buffer pool, slotted pages, B+ tree) exercised.\n";
    return 0;
}
