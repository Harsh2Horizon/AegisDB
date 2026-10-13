# AegisDB

A standalone, single-node C++20 LSM-tree key-value database. The embedded library and Boost.Beast HTTP server use the same engine. Storage is implemented here: segmented WAL, skip-list memtables, checksummed SSTables, Bloom filters, sparse indexes, manifest edits, MVCC, streaming range merges and leveled compaction.

Read the offline [engineering handbook](docs/architecture.html), [HTTP API](docs/http-api.md) and [implementation checkpoint](docs/implementation-status.md). This is an initial storage-engine implementation, with measured benchmarks and explicit limits; no comparison with other databases is claimed.

## Build

Required: C++20 compiler, CMake 3.24+, Ninja, Boost 1.75+ with Boost.JSON, GoogleTest, Python 3 for TCP tests. The engine itself depends only on C++20 and POSIX APIs.

Ubuntu 24.04 dependency installation:

```sh
sudo apt-get install build-essential cmake ninja-build libboost-dev libboost-json-dev libgtest-dev python3 clang-format clang-tidy
cmake --preset debug
cmake --build --preset debug -j4
ctest --preset debug
```

macOS dependency installation:

```sh
brew install cmake ninja boost googletest llvm
cmake --preset debug -DCMAKE_PREFIX_PATH="$(brew --prefix)"
cmake --build --preset debug -j4
ctest --preset debug
```

If multiple SDKs/toolchains are installed, supply a matching `-DCMAKE_OSX_SYSROOT="$(xcrun --show-sdk-path)"`. A mismatched default CommandLineTools SDK was observed in the development environment; use the SDK corresponding to the active Xcode compiler/linker. External dependencies can also be found using a semicolon-separated `CMAKE_PREFIX_PATH`; no particular username is assumed.

```sh
cmake --preset release
cmake --build --preset release -j4
cmake --preset asan
cmake --build --preset asan -j4
ctest --preset asan
cmake --preset tsan
cmake --build --preset tsan -j4
ctest --preset tsan -R 'AtomicVisibility|ManyWriters|BoundedQueue|HttpRouter'
```

For an engine-only build, configure with `-DAEGISDB_BUILD_SERVER=OFF`; add `-DAEGISDB_BUILD_TESTS=OFF` to omit GoogleTest. Install with `cmake --install build/release --prefix /your/install/path`. Consumers use `find_package(AegisDB CONFIG REQUIRED)` and link `AegisDB::aegisdb`, or use `add_subdirectory` and link `aegisdb`.

## Embedded usage

```cpp
#include <aegisdb/db.h>

auto opened = aegisdb::Database::open("my-data");
if (!opened) { /* inspect opened.status() */ return; }
auto db = std::move(opened).value();
auto status = db->put("x", "A"); // SYNC is the default
if (!status) { /* handle storage error */ return; }
auto snap = db->createSnapshot();
if (!snap) return;
status = db->put("x", "B");
if (!status) return;
auto historical = db->get("x", {snap.value()}); // A
aegisdb::WriteBatch batch;
batch.put("a", "one").remove("x").put("b", "two");
status = db->write(batch); // one atomic visibility and replay unit
```

All keys and values are arbitrary bytes, including empty strings. Check every `Status`/`Result` before accessing its value. `ScanOptions` uses `[start,end)`; absent `end` means unbounded above. `after` is exclusive. For consistent pages, create one snapshot and reuse it on every page. See `apps/embedded_example.cpp` and run `build/debug/aegisdb-example ./example-data`.

Destroy the database after API calls finish. Snapshot copies share one RAII token; handles may outlive the database but cannot be used after reopening. One serialized mutation coordinator supports concurrent readers. Bounded scans hold a shared lock on their mutable memtable, so a long scan can delay writes. A single maintenance worker performs background flush and compaction. Explicit maintenance calls wait; HTTP maintenance calls return a job ID.

## Server

```sh
build/release/aegisdb-server --config config/example.json
# Or:
build/debug/aegisdb-server --db ./my-data --host 127.0.0.1 --port 8080 --workers 4
```

The server binds to localhost by default and has no authentication or TLS. Use it only on local or explicitly trusted networks, including the admin endpoints. HTTP/1.1 connections close after one response. Request parsing and socket I/O are asynchronous; database work runs on bounded worker admission. SIGINT/SIGTERM stops admission, cancels network sessions and finishes already admitted storage/maintenance work before closing the engine. A canceled response does not roll back a committed write.

```sh
curl -s http://127.0.0.1:8080/v1/health
curl -s -H 'Content-Type: application/json' http://127.0.0.1:8080/v1/kv/put \
  -d '{"key_base64":"dXNlcjox","value_base64":"SGVsbG8=","sync":true}'
curl -s -H 'Content-Type: application/json' http://127.0.0.1:8080/v1/kv/get \
  -d '{"key_base64":"dXNlcjox"}'
curl -s -H 'Content-Type: application/json' http://127.0.0.1:8080/v1/kv/delete \
  -d '{"key_base64":"dXNlcjox","sync":true}'
```

All endpoint schemas and snapshot/scan examples are in [docs/http-api.md](docs/http-api.md). Server configuration is JSON; the example describes actual supported options, rather than an unimplemented YAML parser.

## Persistence and resource limits

SYNC acknowledgment follows a complete WAL append, WAL synchronization and atomic memtable publication. ASYNC can lose acknowledged writes after machine failure. A complete unacknowledged batch may be recovered after an I/O error or canceled connection; the API does not provide request deduplication. A persistence error disables writes until reopen. Reads retain the last published view and still report read corruption.

Crash durability assumes a local POSIX filesystem honoring file and directory `fsync`, atomic same-directory rename, advisory locking and storage-device flushes. Existing ancestor directory names must already be durable; the engine synchronizes parent entries for directories it creates. macOS also requests `F_FULLFSYNC` where supported. Hardware/filesystem violations are outside this guarantee. Process-crash tests do not establish power-loss behavior. An incomplete final frame is discarded; a complete frame with a bad checksum is rejected, including at the tail. There is no salvage mode.

Defaults: 4 MiB memtable target, four immutable memtables, 8 MiB WAL segment target, 16 KiB blocks, 4 MiB compaction output target, seven levels, ratio 10, 1% Bloom target, 32 MiB block cache. Keys are limited to 1 MiB, values to 16 MiB, batches to 24 MiB and 100,000 operations. Scan pages have at most 10,000 results and 32 MiB decoded payload. One batch/record/user-key group can exceed a target size; hard format and request limits still apply. The cache budget accounts decoded blocks, not allocator overhead or evicted blocks still pinned by readers.

L0 growth is bounded even when automatic compaction is disabled: writes time out with BUSY at the configured limit, and manual compaction relieves it. Manifest history is append-only in this release; checkpoint rotation is future work. Index/Bloom metadata sections have a 32 MiB format limit. A huge single-key version history can therefore require a larger-than-target table and eventually exceed a metadata limit, producing an explicit storage error. Snapshots are process-local and keep history until released; bottom-level history can be reclaimed with explicit compaction.

## Tests and benchmarks

GoogleTest covers codecs, skip lists, WAL truncation at every byte of a final batch, Bloom behavior, SSTable seeks, snapshots, L0 limits, differential workloads, concurrency, SIGKILL recovery and injected I/O/crash boundaries. CTest also starts the real server for Python HTTP integration tests. If socket binds are prohibited, that suite exits with the designated skip code and is reported as skipped. See the checkpoint for executed test counts and sanitizer results.

```sh
build/release/aegisdb-benchmark --count 10000
build/release/aegisdb-benchmark --count 100000 --value-size 128 --threads 4
build/release/aegisdb-benchmark --count 1000000
build/release/aegisdb-benchmark --count 1000 --sync
```

Benchmarks emit JSON Lines containing throughput, latency percentiles, WAL/SSTable bytes, write amplification excluding the manifest, block-cache misses and process peak RSS. They verify results during measurement. Benchmark writes default to ASYNC; this differs deliberately from the database API default. Fresh random insertion uses a separate empty database; the main database measures sequential insertion followed by random updates. Initially empty engine cache does not imply a cold OS page cache. Methodology and raw measured results are under `benchmarks/`.

## Scope

No clustering, replication, consensus, SQL, indexes, authentication or database-engine dependencies. Linux is the primary target; macOS ARM64 builds and storage tests have been executed. Windows and network filesystems are unsupported. Real TCP behavior still needs execution in an environment that permits local sockets; the development sandbox prohibits binding them. The handbook and checkpoint state the remaining validation work explicitly.
