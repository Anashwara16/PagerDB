// ==============================================================================
// src/main.cc - kvstore_demo: smoke test + crash-demo workload (Phase 6)
// ==============================================================================
//
//   kvstore_demo                     - original tiny smoke test in ./testdb
//   kvstore_demo load <dir> <n>      - sequentially Put n keys (16-byte keys,
//                                      100-byte deterministic values), meant
//                                      to be killed mid-run (tools/crash_demo.sh)
//   kvstore_demo verify <dir> <n>    - after reopening (which runs recovery),
//                                      checks the survivors are EXACTLY a
//                                      prefix of the load order with correct
//                                      values, prints how many survived
//
// The load intentionally uses sync_on_write=false: the demo shows that a
// kill -9 loses at most the un-flushed WAL tail and never corrupts the tree,
// not that every acknowledged write is durable (use sync_on_write=true for
// that guarantee).
//
// ==============================================================================

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

#include "kvstore/db.h"

namespace {

std::string MakeKey(uint64_t index) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llu",
                  static_cast<unsigned long long>(index));
    return std::string(buf, 16);
}

std::string MakeValue(uint64_t index) {
    std::string value(100, '\0');
    uint64_t x = index * 0x9E3779B97F4A7C15ULL + 1;
    for (size_t i = 0; i < value.size(); ++i) {
        x ^= x >> 33;
        x *= 0xFF51AFD7ED558CCDULL;
        value[i] = static_cast<char>('a' + (x % 26));
    }
    return value;
}

int RunLoad(const std::string& dir, uint64_t n) {
    kvstore::Options options;
    options.create_if_missing = true;
    options.sync_on_write = false;
    auto db = kvstore::DB::Open(dir, options);
    if (!db.has_value()) {
        std::cerr << "open failed: " << db.error().message() << "\n";
        return 1;
    }
    for (uint64_t i = 0; i < n; ++i) {
        auto status = (*db)->Put(MakeKey(i), MakeValue(i));
        if (!status.ok()) {
            std::cerr << "Put " << i << " failed: " << status.message() << "\n";
            return 1;
        }
        if ((i + 1) % 100000 == 0) {
            std::cout << "loaded " << (i + 1) << " keys\n" << std::flush;
        }
    }
    std::cout << "load complete: " << n << " keys\n";
    return 0;
}

int RunVerify(const std::string& dir, uint64_t n) {
    kvstore::Options options;  // create_if_missing=false: must already exist
    auto db = kvstore::DB::Open(dir, options);
    if (!db.has_value()) {
        std::cerr << "reopen failed: " << db.error().message() << "\n";
        return 1;
    }

    // Recovery must leave exactly a prefix of the sequential load: keys
    // 0..K-1 present with correct values, K..n-1 absent, nothing else.
    uint64_t expected = 0;
    bool mismatch = false;
    auto status = (*db)->Scan(
        {}, {}, [&](std::string_view key, std::string_view value) {
            if (key != MakeKey(expected) || value != MakeValue(expected)) {
                std::cerr << "MISMATCH at scan position " << expected << "\n";
                mismatch = true;
                return false;
            }
            ++expected;
            return true;
        });
    if (!status.ok() || mismatch) {
        std::cerr << "verify FAILED\n";
        return 1;
    }
    const uint64_t survivors = expected;
    // The rest of the keyspace must be absent (no partial/garbled tail).
    for (uint64_t probe = survivors; probe < n;
        probe += (n - survivors) / 128 + 1) {
        if ((*db)->Contains(MakeKey(probe))) {
            std::cerr << "verify FAILED: key " << probe
                      << " present beyond the durable prefix\n";
            return 1;
        }
    }
    if ((*db)->ApproximateKeyCount() != survivors) {
        std::cerr << "verify FAILED: num_keys=" << (*db)->ApproximateKeyCount()
                  << " but scan saw " << survivors << "\n";
        return 1;
    }
    std::cout << "verify OK: " << survivors << " of " << n
              << " puts survived as a clean prefix (num_keys agrees)\n";
    return 0;
}

int RunSmoke() {
    std::cout << "=== KVStore Test ===\n\n";

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

    std::cout << "\n--- Inserting keys ---\n";
    auto s1 = db->Put("name", "Alice");
    std::cout << "Put(name, Alice): " << (s1.ok() ? "OK" : s1.message()) << "\n";
    auto s2 = db->Put("age", "30");
    std::cout << "Put(age, 30): " << (s2.ok() ? "OK" : s2.message()) << "\n";
    auto s3 = db->Put("city", "Seattle");
    std::cout << "Put(city, Seattle): " << (s3.ok() ? "OK" : s3.message()) << "\n";

    std::cout << "\n--- Reading keys ---\n";
    auto v1 = db->Get("name");
    std::cout << "Get(name): "
              << (v1.has_value() ? v1.value() : std::string(v1.error().message()))
              << "\n";
    auto v2 = db->Get("age");
    std::cout << "Get(age): "
              << (v2.has_value() ? v2.value() : std::string(v2.error().message()))
              << "\n";

    std::cout << "\n--- Checking existence ---\n";
    std::cout << "Contains(name): " << (db->Contains("name") ? "yes" : "no") << "\n";
    std::cout << "Contains(missing): " << (db->Contains("missing") ? "yes" : "no") << "\n";

    std::cout << "\n--- Updating value ---\n";
    auto s4 = db->Put("age", "31");
    std::cout << "Put(age, 31): " << (s4.ok() ? "OK" : s4.message()) << "\n";
    auto v3 = db->Get("age");
    if (v3.has_value()) {
        std::cout << "Get(age): " << v3.value() << "\n";
    }

    std::cout << "\n--- Stats ---\n";
    std::cout << "Approximate key count: " << db->ApproximateKeyCount() << "\n";
    std::cout << "\n=== Test Complete ===\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 4 && std::strcmp(argv[1], "load") == 0) {
        return RunLoad(argv[2], std::strtoull(argv[3], nullptr, 10));
    }
    if (argc == 4 && std::strcmp(argv[1], "verify") == 0) {
        return RunVerify(argv[2], std::strtoull(argv[3], nullptr, 10));
    }
    if (argc != 1) {
        std::cerr << "usage: kvstore_demo [load|verify <dir> <n>]\n";
        return 2;
    }
    return RunSmoke();
}
