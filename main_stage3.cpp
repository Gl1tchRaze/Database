#include <chrono>
#include <cstdio>
#include <iostream>
#include "Indexed_table.hpp"

using Clock = std::chrono::steady_clock;

static double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

int main() {
    std::remove("students_idx.db");
    const int N = 10000;
    const int LOOKUPS = 100;

    {
        IndexedTable t("students_idx.db");
        for (int i = 1; i <= N; ++i) t.insert(i, "Student_" + std::to_string(i) + ",CSE-21");

        std::cout << "Inserted " << t.size() << " records in " << t.num_pages() << " pages\n";
        std::cout << "Duplicate key 5 accepted? " << (t.insert(5, "dup") ? "yes" : "no") << "\n";

        std::string out;
        auto t0 = Clock::now();
        for (int i = 0; i < LOOKUPS; ++i) t.find(N, out);
        double idx_ms = ms_since(t0);
        std::cout << "find(" << N << ") = " << out << "\n";

        t0 = Clock::now();
        for (int i = 0; i < LOOKUPS; ++i) t.find_by_scan(N, out);
        double scan_ms = ms_since(t0);

        std::cout << LOOKUPS << " lookups -> index: " << idx_ms << " ms, full scan: " << scan_ms << " ms\n";
    }

    {
        IndexedTable t("students_idx.db");   // reopen: index is rebuilt from the file
        std::string out;
        bool ok = t.find(1234, out);
        std::cout << "After reopen, find(1234) = " << (ok ? out : "NOT FOUND") << ", size = " << t.size() << "\n";
    }
    return 0;
}