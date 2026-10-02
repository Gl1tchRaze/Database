#pragma once
#include <algorithm>
#include <cstddef>
#include <iostream>
#include <vector>
#include "table.hpp"   // for RecordId

// In-memory B+ tree: int key -> RecordId.
//  - Internal nodes only route (keys + child pointers).
//  - Leaf nodes hold the (key, RecordId) pairs and are linked left to right.
//  - kids[i] has keys < keys[i];  kids[i+1] has keys >= keys[i].
class BPlusTree {
    struct Node {
        bool leaf;
        std::vector<int> keys;
        std::vector<RecordId> vals;   // leaf only
        std::vector<Node*> kids;      // internal only
        Node* next = nullptr;         // leaf only: next leaf
        explicit Node(bool l) : leaf(l) {}
        ~Node() { for (Node* k : kids) delete k; }
    };

    struct Split {
        bool happened = false;
        int key = 0;
        Node* right = nullptr;
    };

public:
    explicit BPlusTree(int max_keys = 64) : max_keys_(max_keys), root_(new Node(true)) {
        if (max_keys_ < 3) throw std::invalid_argument("max_keys must be >= 3");
    }
    ~BPlusTree() { delete root_; }
    BPlusTree(const BPlusTree&) = delete;
    BPlusTree& operator=(const BPlusTree&) = delete;

    // Returns false if the key already exists
    bool insert(int key, RecordId rid) {
        dup_ = false;
        Split s = insert_rec(root_, key, rid);
        if (dup_) return false;
        if (s.happened) {                       // root split: tree grows one level
            Node* new_root = new Node(false);
            new_root->keys.push_back(s.key);
            new_root->kids.push_back(root_);
            new_root->kids.push_back(s.right);
            root_ = new_root;
        }
        ++size_;
        return true;
    }

    bool find(int key, RecordId& out) const {
        const Node* leaf = find_leaf(key);
        auto it = std::lower_bound(leaf->keys.begin(), leaf->keys.end(), key);
        if (it == leaf->keys.end() || *it != key) return false;
        out = leaf->vals[idx(it, leaf->keys.begin())];
        return true;
    }

    // Removes `key`. Returns false if it was not present.
    // This is a deliberately simple deletion: the key/value is dropped from its
    // leaf, but leaves are NOT merged or rebalanced (underfull leaves are
    // tolerated). Correctness is unaffected — removing a key never relocates the
    // others, and the parent separators still partition the key space the same
    // way, so find() and range() keep working. Only tree balance can degrade.
    bool remove(int key) {
        Node* leaf = find_leaf_mut(key);
        auto it = std::lower_bound(leaf->keys.begin(), leaf->keys.end(), key);
        if (it == leaf->keys.end() || *it != key) return false;
        size_t pos = idx(it, leaf->keys.begin());
        leaf->keys.erase(it);
        leaf->vals.erase(leaf->vals.begin() + static_cast<std::ptrdiff_t>(pos));
        --size_;
        return true;
    }

    // Calls fn(key, rid) for every key in [lo, hi], in sorted order
    template <typename Fn>
    void range(int lo, int hi, Fn fn) const {
        const Node* leaf = find_leaf(lo);
        while (leaf) {
            for (size_t i = 0; i < leaf->keys.size(); ++i) {
                if (leaf->keys[i] > hi) return;
                if (leaf->keys[i] >= lo) fn(leaf->keys[i], leaf->vals[i]);
            }
            leaf = leaf->next;                  // follow the leaf chain
        }
    }

    size_t size() const { return size_; }

    int height() const {
        int h = 1;
        for (const Node* n = root_; !n->leaf; n = n->kids[0]) ++h;
        return h;
    }

    // Print the tree level by level (use with small trees)
    void dump() const {
        std::vector<const Node*> level{root_};
        int depth = 0;
        while (!level.empty()) {
            std::cout << "  Level " << depth++ << ": ";
            std::vector<const Node*> next;
            for (const Node* n : level) {
                std::cout << "[";
                for (size_t i = 0; i < n->keys.size(); ++i)
                    std::cout << n->keys[i] << (i + 1 < n->keys.size() ? " " : "");
                std::cout << "] ";
                if (!n->leaf) for (const Node* k : n->kids) next.push_back(k);
            }
            std::cout << "\n";
            level = next;
        }
    }

private:
    // Index of iterator `it` within a container starting at `begin` (avoids
    // sign-conversion warnings from raw `it - begin` arithmetic).
    template <typename It>
    static size_t idx(It it, It begin) {
        return static_cast<size_t>(it - begin);
    }

    const Node* find_leaf(int key) const {
        const Node* n = root_;
        while (!n->leaf) {
            n = n->kids[idx(std::upper_bound(n->keys.begin(), n->keys.end(), key), n->keys.begin())];
        }
        return n;
    }

    Node* find_leaf_mut(int key) {
        Node* n = root_;
        while (!n->leaf) {
            n = n->kids[idx(std::upper_bound(n->keys.begin(), n->keys.end(), key), n->keys.begin())];
        }
        return n;
    }

    Split insert_rec(Node* n, int key, RecordId rid) {
        if (n->leaf) {
            auto it = std::lower_bound(n->keys.begin(), n->keys.end(), key);
            if (it != n->keys.end() && *it == key) { dup_ = true; return {}; }
            size_t pos = idx(it, n->keys.begin());
            n->keys.insert(it, key);
            n->vals.insert(n->vals.begin() + static_cast<std::ptrdiff_t>(pos), rid);
            if (n->keys.size() <= static_cast<size_t>(max_keys_)) return {};

            // Leaf overflow: split in half. The first key of the right half is COPIED up.
            size_t mid = n->keys.size() / 2;
            Node* r = new Node(true);
            r->keys.assign(n->keys.begin() + static_cast<std::ptrdiff_t>(mid), n->keys.end());
            r->vals.assign(n->vals.begin() + static_cast<std::ptrdiff_t>(mid), n->vals.end());
            n->keys.resize(mid);
            n->vals.resize(mid);
            r->next = n->next;
            n->next = r;
            return {true, r->keys[0], r};
        }

        size_t i = idx(std::upper_bound(n->keys.begin(), n->keys.end(), key), n->keys.begin());
        Split s = insert_rec(n->kids[i], key, rid);
        if (!s.happened) return {};

        n->keys.insert(n->keys.begin() + static_cast<std::ptrdiff_t>(i), s.key);
        n->kids.insert(n->kids.begin() + static_cast<std::ptrdiff_t>(i + 1), s.right);
        if (n->keys.size() <= static_cast<size_t>(max_keys_)) return {};

        // Internal overflow: the middle key is MOVED up (it does not stay below).
        size_t mid = n->keys.size() / 2;
        int up = n->keys[mid];
        Node* r = new Node(false);
        r->keys.assign(n->keys.begin() + static_cast<std::ptrdiff_t>(mid + 1), n->keys.end());
        r->kids.assign(n->kids.begin() + static_cast<std::ptrdiff_t>(mid + 1), n->kids.end());
        n->keys.resize(mid);
        n->kids.resize(mid + 1);
        return {true, up, r};
    }

    int max_keys_;
    Node* root_;
    size_t size_ = 0;
    bool dup_ = false;
};