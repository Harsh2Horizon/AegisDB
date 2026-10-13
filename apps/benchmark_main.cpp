#include <aegisdb/db.h>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <sys/resource.h>
#include <thread>
#include <unistd.h>

using namespace aegisdb;
using Clock = std::chrono::steady_clock;
namespace {
void check(Status s) {
    if (!s)
        throw std::runtime_error(s.message);
}
template <class T> T take(Result<T> r) {
    if (!r)
        throw std::runtime_error(r.status().message);
    return std::move(r).value();
}
struct Measurement {
    std::string name, unit;
    std::size_t operations{};
    double seconds{};
    std::vector<double> latency;
    std::uint64_t bytes{};
};
template <class F>
Measurement measure(std::string name, std::string unit, std::size_t count, F operation) {
    Measurement m{std::move(name), std::move(unit), count, 0, {}, 0};
    m.latency.reserve(count);
    const auto begin = Clock::now();
    for (std::size_t i = 0; i < count; ++i) {
        const auto start = Clock::now();
        operation(i);
        m.latency.push_back(
            std::chrono::duration<double, std::micro>(Clock::now() - start).count());
    }
    m.seconds = std::chrono::duration<double>(Clock::now() - begin).count();
    return m;
}
void output(Measurement m) {
    std::sort(m.latency.begin(), m.latency.end());
    const auto quantile = [&](double p) {
        return m.latency.empty() ? 0
                                 : m.latency[static_cast<std::size_t>(
                                       p * static_cast<double>(m.latency.size() - 1))];
    };
    const auto average = m.latency.empty()
                             ? 0
                             : std::accumulate(m.latency.begin(), m.latency.end(), 0.0) /
                                   static_cast<double>(m.latency.size());
    std::cout << "{\"workload\":\"" << m.name << "\",\"unit\":\"" << m.unit
              << "\",\"operations\":" << m.operations << ",\"seconds\":" << m.seconds
              << ",\"ops_per_second\":" << static_cast<double>(m.operations) / m.seconds
              << ",\"average_us\":" << average << ",\"p50_us\":" << quantile(.50)
              << ",\"p95_us\":" << quantile(.95) << ",\"p99_us\":" << quantile(.99)
              << ",\"bytes_processed\":" << m.bytes << "}\n";
}
std::size_t parse(std::string_view text) {
    std::size_t n{};
    auto r = std::from_chars(text.data(), text.data() + text.size(), n);
    if (r.ec != std::errc{} || r.ptr != text.data() + text.size())
        throw std::runtime_error("invalid numeric option");
    return n;
}
} // namespace
int main(int argc, char **argv) {
    std::filesystem::path directory =
        std::filesystem::temp_directory_path() / ("aegis-benchmark-" + std::to_string(getpid()));
    std::size_t count = 10000, keySize = 16, valueSize = 128, threads = 4;
    bool sync = false;
    try {
        for (int i = 1; i < argc; ++i) {
            std::string_view flag = argv[i];
            if (flag == "--sync") {
                sync = true;
                continue;
            }
            if (flag == "--help") {
                std::cout << "aegisdb-benchmark [--count N] [--key-size N] [--value-size N] "
                             "[--threads N] [--directory EMPTY_PATH] [--sync]\n";
                return 0;
            }
            if (i + 1 == argc)
                throw std::runtime_error("missing option value");
            const std::string_view value = argv[++i];
            if (flag == "--count")
                count = parse(value);
            else if (flag == "--key-size")
                keySize = parse(value);
            else if (flag == "--value-size")
                valueSize = parse(value);
            else if (flag == "--threads")
                threads = parse(value);
            else if (flag == "--directory")
                directory = std::string(value);
            else
                throw std::runtime_error("unknown option");
        }
        if (count == 0 || count > 10000000 || keySize < 8 || keySize > 1024 ||
            valueSize > 1024 * 1024 || threads == 0 || threads > 64)
            throw std::runtime_error("invalid benchmark sizes");
        if (std::filesystem::exists(directory) && !std::filesystem::is_empty(directory))
            throw std::runtime_error("benchmark directory must be empty");
        Options options;
        options.automaticCompaction = false;
        options.l0StopFiles = std::max<std::size_t>(
            16, 3 * count * (keySize + valueSize + 200) / options.memtableBytes + 32);
        auto db = take(Database::open(directory, options));
        WriteOptions write{sync};
        std::string value(valueSize, 'v');
        std::vector<std::string> keys;
        keys.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            auto key = std::to_string(i);
            if (key.size() > keySize)
                throw std::runtime_error("key size too small for count");
            keys.push_back(std::string(keySize - key.size(), '0') + key);
        }
        std::vector<std::size_t> order(count);
        std::iota(order.begin(), order.end(), 0);
        std::mt19937_64 rng(615);
        std::shuffle(order.begin(), order.end(), rng);
        std::cout << std::fixed << std::setprecision(3) << "{\"configuration\":{\"count\":" << count
                  << ",\"key_bytes\":" << keySize << ",\"value_bytes\":" << valueSize
                  << ",\"sync\":" << (sync ? "true" : "false")
                  << ",\"seed\":615,\"threads\":" << threads << "}}\n";
        // Use a separate empty database so random insertion is distinct from later updates.
        {
            auto fresh = take(Database::open(directory / "random-insert", options));
            output(measure("fresh_random_insert", "record", count, [&](std::size_t i) {
                check(fresh->put(keys[order[i]], value, write));
            }));
            check(fresh->flush());
            for (const auto &key : keys)
                if (take(fresh->get(key)) != value)
                    throw std::runtime_error("fresh random insertion mismatch");
        }
        output(measure("sequential_insert", "record", count,
                       [&](std::size_t i) { check(db->put(keys[i], value, write)); }));
        output(measure("random_update", "record", count,
                       [&](std::size_t i) { check(db->put(keys[order[i]], value, write)); }));
        check(db->flush());
        output(measure("random_read_warm_engine_cache", "record", count, [&](std::size_t i) {
            if (take(db->get(keys[order[i]])) != value)
                throw std::runtime_error("incorrect value");
        }));
        const auto beforeNeg = db->statistics();
        output(measure("negative_lookup_within_ranges", "record", count, [&](std::size_t i) {
            auto got = db->get(keys[order[i]] + std::string(1, '\0'));
            if (got.status().code != Code::not_found)
                throw std::runtime_error("negative lookup failed");
        }));
        const auto afterNeg = db->statistics();
        std::cout << "{\"negative_bloom_checks\":" << afterNeg.bloomChecks - beforeNeg.bloomChecks
                  << ",\"negative_bloom_rejections\":"
                  << afterNeg.bloomNegatives - beforeNeg.bloomNegatives << "}\n";
        ScanOptions scan;
        scan.limit = 1000;
        std::size_t scanned = 0, pages = 0;
        std::vector<double> pageLatency;
        auto scanStart = Clock::now();
        do {
            auto start = Clock::now();
            auto page = take(db->scan(scan));
            scanned += page.entries.size();
            scan.after = page.nextAfter;
            ++pages;
            pageLatency.push_back(
                std::chrono::duration<double, std::micro>(Clock::now() - start).count());
        } while (scan.after);
        if (scanned != count)
            throw std::runtime_error("scan mismatch");
        output({"range_scan", "page", pages,
                std::chrono::duration<double>(Clock::now() - scanStart).count(),
                std::move(pageLatency), scanned * (keySize + valueSize)});
        const auto batches = (count + 99) / 100;
        output(measure("write_batch_100", "batch", batches, [&](std::size_t i) {
            WriteBatch b;
            for (std::size_t j = i * 100; j < std::min(count, (i + 1) * 100); ++j)
                b.put(keys[order[j]], value);
            check(db->write(b, write));
        }));
        auto stats = db->statistics();
        auto flush = measure("explicit_flush", "call", 1, [&](std::size_t) { check(db->flush()); });
        flush.bytes = db->statistics().flushBytesWritten - stats.flushBytesWritten;
        output(std::move(flush));
        stats = db->statistics();
        auto compact = measure("leveled_compaction_sweep", "call", 1,
                               [&](std::size_t) { check(db->compact()); });
        compact.bytes = db->statistics().compactionBytesRead - stats.compactionBytesRead;
        output(std::move(compact));
        stats = db->statistics();
        const auto written =
            stats.walBytes + stats.flushBytesWritten + stats.compactionBytesWritten;
        const auto logical = 3 * count * (keySize + valueSize);
        std::cout << "{\"wal_and_sstable_bytes_written\":" << written
                  << ",\"logical_mutation_bytes\":" << logical
                  << ",\"write_amplification_excluding_manifest\":"
                  << static_cast<double>(written) / static_cast<double>(logical) << "}\n";
        db.reset();
        output(measure("recovery_with_table_verification", "open", 1,
                       [&](std::size_t) { db = take(Database::open(directory, options)); }));
        db.reset();
        auto coldOptions = options;
        coldOptions.verifyTablesOnOpen = false;
        db = take(Database::open(directory, coldOptions));
        auto coldBefore = db->statistics();
        output(measure("random_read_initially_empty_engine_cache", "record", count,
                       [&](std::size_t i) {
                           if (take(db->get(keys[order[i]])) != value)
                               throw std::runtime_error("incorrect cold read");
                       }));
        auto coldAfter = db->statistics();
        std::cout << "{\"cold_block_cache_misses_per_get\":"
                  << static_cast<double>(coldAfter.cacheMisses - coldBefore.cacheMisses) /
                         static_cast<double>(count)
                  << ",\"os_page_cache_evicted\":false}\n";
        std::vector<std::vector<double>> local(threads);
        std::atomic<bool> failed = false;
        std::vector<std::jthread> readers;
        const auto concurrentStart = Clock::now();
        for (std::size_t t = 0; t < threads; ++t)
            readers.emplace_back([&, t] {
                for (std::size_t i = t; i < count; i += threads) {
                    const auto start = Clock::now();
                    auto got = db->get(keys[order[i]]);
                    if (!got || got.value() != value)
                        failed = true;
                    local[t].push_back(
                        std::chrono::duration<double, std::micro>(Clock::now() - start).count());
                }
            });
        for (auto &reader : readers)
            reader.join();
        if (failed)
            throw std::runtime_error("concurrent read failed");
        Measurement concurrent{
            "concurrent_read",
            "record",
            count,
            std::chrono::duration<double>(Clock::now() - concurrentStart).count(),
            {},
            0};
        for (auto &lat : local)
            concurrent.latency.insert(concurrent.latency.end(), lat.begin(), lat.end());
        output(std::move(concurrent));
        rusage usage{};
        if (getrusage(RUSAGE_SELF, &usage) == 0) {
#if defined(__APPLE__)
            const auto rss = usage.ru_maxrss;
#else
            const auto rss = usage.ru_maxrss * 1024;
#endif
            std::cout << "{\"process_peak_rss_bytes\":" << rss
                      << ",\"engine_cache_charge_bytes\":" << db->statistics().cacheBytes << "}\n";
        }
        db.reset();
        std::filesystem::remove_all(directory);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "benchmark failed: " << e.what() << '\n';
        return 1;
    }
}
