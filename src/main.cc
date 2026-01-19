// src/main.cc - Simple test for kvstore
#include <iostream>
#include <string>
#include "kvstore/db.h"

int main() {
    std::cout << "=== KVStore Test ===\n\n";

    // 1. Open/create database
    kvstore::Options options;
    options.create_if_missing = true;
    options.sync_on_write = true;

    auto result = kvstore::DB::Open("./testdb", options);
    if (!result.has_value()) {
        std::cerr << "Failed to open DB: " << result.error().message() << "\n";
        return 1;
    }

    auto db = std::move(result.value());
    std::cout << "✓ Database opened at: " << db->GetPath() << "\n";

    // 2. Insert some key-value pairs
    std::cout << "\n--- Inserting keys ---\n";
    
    auto s1 = db->Put("name", "Alice");
    std::cout << "Put(name, Alice): " << (s1.ok() ? "OK" : s1.message()) << "\n";

    auto s2 = db->Put("age", "30");
    std::cout << "Put(age, 30): " << (s2.ok() ? "OK" : s2.message()) << "\n";

    auto s3 = db->Put("city", "Seattle");
    std::cout << "Put(city, Seattle): " << (s3.ok() ? "OK" : s3.message()) << "\n";

    // 3. Read them back
    std::cout << "\n--- Reading keys ---\n";

    auto v1 = db->Get("name");
    if (v1.has_value()) {
        std::cout << "Get(name): " << v1.value() << "\n";
    } else {
        std::cout << "Get(name): " << v1.error().message() << "\n";
    }

    auto v2 = db->Get("age");
    if (v2.has_value()) {
        std::cout << "Get(age): " << v2.value() << "\n";
    } else {
        std::cout << "Get(age): " << v2.error().message() << "\n";
    }

    // 4. Check Contains
    std::cout << "\n--- Checking existence ---\n";
    std::cout << "Contains(name): " << (db->Contains("name") ? "yes" : "no") << "\n";
    std::cout << "Contains(missing): " << (db->Contains("missing") ? "yes" : "no") << "\n";

    // 5. Update a value
    std::cout << "\n--- Updating value ---\n";
    auto s4 = db->Put("age", "31");
    std::cout << "Put(age, 31): " << (s4.ok() ? "OK" : s4.message()) << "\n";

    auto v3 = db->Get("age");
    if (v3.has_value()) {
        std::cout << "Get(age): " << v3.value() << "\n";
    }

    // 6. Stats
    std::cout << "\n--- Stats ---\n";
    std::cout << "Approximate key count: " << db->ApproximateKeyCount() << "\n";

    std::cout << "\n=== Test Complete ===\n";
    return 0;
}