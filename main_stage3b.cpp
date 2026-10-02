#include <algorithm>
#include <cstdio>
#include <iostream>
#include <numeric>
#include <random>
#include "bplus_tree.hpp"

int main() {
    // ---- Part 1: small tree so you can SEE the splits (max 4 keys per node) ----
    {
        BPlusTree small(4);
        int keys[] = {10, 20, 5, 6, 12, 30, 7, 17, 3, 25};
        for (int k : keys) small.insert(k, RecordId{0, 0});

        std::cout << "Small tree after inserting 10,20,5,6,12,30,7,17,3,25:\n";
        small.dump();
        std::cout << "  height = " << small.height() << "\n";

        std::cout << "  range [6, 20]: ";
        small.range(6, 20, [](int k, RecordId) { std::cout << k << " "; });
        std::cout << "\n\n";
    }

    // ---- Part 2: B+ tree as an index over the real Table ----
    std::remove("students_bpt.db");
    const int N = 10000;

    Table table("students_bpt.db");
    BPlusTree index(64);

    std::vector<int> ids(N);
    std::iota(ids.begin(), ids.end(), 1);
    std::shuffle(ids.begin(), ids.end(), std::mt19937(42));   // random insert order

    for (int id : ids) {
        RecordId rid = table.insert(std::to_string(id) + ",Student_" + std::to_string(id) + ",CSE-21");
        index.insert(id, rid);
    }
    std::cout << "Inserted " << index.size() << " shuffled records, tree height = " << index.height() << "\n";
    std::cout << "Duplicate key 5 accepted? " << (index.insert(5, RecordId{0, 0}) ? "yes" : "no") << "\n";

    // Point lookup
    RecordId rid;
    if (index.find(7777, rid)) std::cout << "find(7777) = " << table.get(rid) << "\n";

    // Range query: this is what a hash index cannot do
    std::cout << "Range [9995, 10000]:\n";
    index.range(9995, 10000, [&](int, RecordId r) { std::cout << "  " << table.get(r) << "\n"; });

    // Correctness: full range scan must return 1..N in sorted order
    int expected = 1;
    bool sorted_ok = true;
    index.range(1, N, [&](int k, RecordId) { if (k != expected++) sorted_ok = false; });
    std::cout << "Full range scan sorted and complete? " << ((sorted_ok && expected == N + 1) ? "yes" : "NO") << "\n";
    return 0;
}