// ===========================================================================
// Correctness tests for the whole engine. Build with:
//     g++ -std=c++17 -O2 -I. tests.cpp -o tests && ./tests
// These check behaviour, not just that things don't crash:
//   - slotted page: insert / erase / tombstone reuse / compaction / capacity
//   - a long random insert+erase churn with byte-exact verification
//   - B+ tree remove keeps every surviving key findable and ranges sorted
//   - the full DBEngine round-trips through close+reopen and space is reused
// ===========================================================================
#include <cassert>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "slotted_page.hpp"
#include "bplus_tree.hpp"
#include "db_engine.hpp"

// --------------------------------------------------------------------------
static void test_slotted_page() {
    std::vector<char> buf(PAGE_SIZE, 0);
    SlottedPage p(buf.data());
    p.init();
    assert(p.free_space() == PAGE_SIZE - HEADER_SIZE);

    std::string rec(100, 'A');
    uint16_t f = p.free_space();
    assert(p.insert(rec.data(), 100) == 0);
    assert(p.free_space() == f - 100 - SLOT_SIZE);

    assert(p.insert(rec.data(), 100) == 1);
    assert(p.insert(rec.data(), 100) == 2);

    uint16_t before = p.free_space();
    assert(p.erase(1));
    assert(p.free_space() == before + 100);   // exactly one record reclaimed
    assert(p.is_deleted(1));
    assert(!p.erase(1));                       // double erase is a no-op
    assert(!p.is_deleted(0) && !p.is_deleted(2));

    const char* d; uint16_t l;
    assert(!p.get(1, d, l));                   // tombstone reads as absent
    assert(p.get(0, d, l) && l == 100);
    assert(p.get(2, d, l) && l == 100);

    std::string small(50, 'B');
    uint16_t f2 = p.free_space();
    assert(p.insert(small.data(), 50) == 1);   // recycles the lowest tombstone
    assert(p.free_space() == f2 - 50);         // no new slot, only new bytes
    assert(p.get(1, d, l) && l == 50 && d[0] == 'B');
    assert(p.get(0, d, l) && l == 100 && d[0] == 'A');   // neighbours intact
    assert(p.get(2, d, l) && l == 100 && d[0] == 'A');

    // Fill until full, then confirm insert refuses rather than corrupting
    int rc;
    while ((rc = p.insert(rec.data(), 100)) >= 0) {}
    assert(p.free_space() < 100 + SLOT_SIZE);
    uint16_t live = 0;
    for (uint16_t s = 0; s < p.num_slots(); ++s)
        if (p.get(s, d, l)) { assert(l == 100 || l == 50); ++live; }
    assert(live == p.live_count());

    printf("  slotted page: OK\n");
}

// --------------------------------------------------------------------------
static void test_churn() {
    std::vector<char> buf(PAGE_SIZE, 0);
    SlottedPage p(buf.data());
    p.init();
    std::map<int, std::string> live;
    unsigned seed = 12345;
    auto rnd = [&] { seed = seed * 1103515245u + 12345u; return (seed >> 16) & 0x7fff; };

    for (int it = 0; it < 100000; ++it) {
        if (live.empty() || rnd() % 2) {
            uint16_t len = static_cast<uint16_t>(1 + rnd() % 300);
            std::string s(len, static_cast<char>('a' + rnd() % 26));
            int slot = p.insert(s.data(), len);
            if (slot >= 0) {
                live[slot] = s;
                const char* d; uint16_t l;
                assert(p.get(static_cast<uint16_t>(slot), d, l) && std::string(d, l) == s);
            }
        } else {
            auto it2 = live.begin();
            std::advance(it2, rnd() % live.size());
            assert(p.erase(static_cast<uint16_t>(it2->first)));
            live.erase(it2);
        }
        assert(p.live_count() == live.size());
    }
    for (auto& kv : live) {
        const char* d; uint16_t l;
        assert(p.get(static_cast<uint16_t>(kv.first), d, l) && std::string(d, l) == kv.second);
    }
    printf("  page churn (100k ops, byte-exact): OK\n");
}

// --------------------------------------------------------------------------
static void test_bplus() {
    BPlusTree t(4);
    std::set<int> live;
    unsigned seed = 7;
    auto rnd = [&] { seed = seed * 1103515245u + 12345u; return (seed >> 16) & 0x7fff; };
    for (int i = 0; i < 2000; ++i) {
        int k = static_cast<int>(rnd() % 500u);
        if (t.insert(k, RecordId{static_cast<uint32_t>(k), 0})) live.insert(k);
    }
    std::vector<int> v(live.begin(), live.end());
    for (size_t i = 0; i < v.size(); i += 2) { assert(t.remove(v[i])); live.erase(v[i]); }
    assert(!t.remove(999999));
    assert(t.size() == live.size());
    for (int k = 0; k < 500; ++k) {
        RecordId r;
        assert(t.find(k, r) == (live.count(k) > 0));
    }
    int prev = -1, cnt = 0;
    t.range(100, 400, [&](int k, RecordId) { assert(k > prev); prev = k; assert(live.count(k)); ++cnt; });
    int expect = 0;
    for (int k : live) if (k >= 100 && k <= 400) ++expect;
    assert(cnt == expect);
    printf("  b+ tree remove + range: OK\n");
}

// --------------------------------------------------------------------------
static void test_engine() {
    std::remove("tests_engine.db");
    std::map<int, std::string> ref;
    {
        DBEngine db("tests_engine.db", 8);
        for (int i = 1; i <= 3000; ++i) {
            std::string v = "val_" + std::to_string(i * 7);
            assert(db.insert(i, v));
            ref[i] = v;
        }
        assert(!db.insert(1, "dup"));                       // duplicate rejected
        for (int i = 3; i <= 3000; i += 3) { assert(db.erase(i)); ref.erase(i); }
        for (int i = 5; i <= 3000; i += 5)
            if (ref.count(i)) { std::string v = "UPD_" + std::to_string(i); assert(db.update(i, v)); ref[i] = v; }
        for (int i = 3; i <= 300; i += 3) { std::string v = "re_" + std::to_string(i); assert(db.insert(i, v)); ref[i] = v; }
        for (auto& kv : ref) { std::string out; assert(db.find(kv.first, out)); assert(out == kv.second); }
        std::string out; assert(!db.find(999999, out));
        assert(db.size() == ref.size());
    }
    {
        DBEngine db("tests_engine.db", 8);                  // reopen: rebuilt from disk
        assert(db.size() == ref.size());
        for (auto& kv : ref) { std::string out; assert(db.find(kv.first, out)); assert(out == kv.second); }
        int prev = 0, cnt = 0;
        db.range(1, 500, [&](int k, const std::string&) { assert(k > prev); prev = k; assert(ref.count(k)); ++cnt; });
        int expect = 0;
        for (auto& kv : ref) if (kv.first >= 1 && kv.first <= 500) ++expect;
        assert(cnt == expect);
    }
    std::remove("tests_engine.db");
    printf("  engine round-trip + reopen: OK\n");
}

int main() {
    printf("Running tests...\n");
    test_slotted_page();
    test_churn();
    test_bplus();
    test_engine();
    printf("ALL TESTS PASSED\n");
    return 0;
}
