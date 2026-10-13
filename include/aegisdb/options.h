#pragma once
#include <aegisdb/filesystem.h>
#include <chrono>
#include <cstddef>
#include <memory>

namespace aegisdb {
struct Options {
    std::size_t memtableBytes{4 * 1024 * 1024};
    std::size_t walSegmentBytes{8 * 1024 * 1024};
    std::size_t maxImmutableMemtables{4};
    std::size_t blockBytes{16 * 1024};
    std::size_t tableBytes{4 * 1024 * 1024};
    std::size_t blockCacheBytes{32 * 1024 * 1024};
    std::size_t maxKeyBytes{1024 * 1024};
    std::size_t maxValueBytes{16 * 1024 * 1024};
    std::size_t maxBatchBytes{24 * 1024 * 1024};
    std::size_t maxBatchOperations{100000};
    std::size_t maxScanResults{10000};
    std::size_t maxScanBytes{32 * 1024 * 1024};
    std::size_t maxSnapshots{1024};
    std::size_t skipListHeight{16};
    double skipListPromotion{0.25};
    double bloomFalsePositiveRate{0.01};
    std::size_t levelCount{7};
    std::size_t levelOneBytes{16 * 1024 * 1024};
    double levelRatio{10};
    std::size_t l0CompactionFiles{4};
    std::size_t l0StopFiles{16};
    bool automaticCompaction{true};
    bool verifyTablesOnOpen{true};
    std::chrono::milliseconds writeTimeout{5000};
    std::shared_ptr<FileSystem> filesystem;
};

struct WriteOptions {
    bool sync{true};
};

} // namespace aegisdb
