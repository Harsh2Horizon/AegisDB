# Benchmark methodology

`aegisdb-benchmark` is a standalone executable linked to the actual embedded engine. It checks operation results and scan counts while measuring them. JSON Lines are suitable for loading into analysis tools.

## Methodology

Release build, deterministic seed 615, default 16-byte zero-padded decimal keys and 128-byte values. Keys/permutation are generated before insertion measurement. Fresh random insertion uses a separate empty database, then flushes and verifies every value. The main database measures sequential insertion followed by randomized updates of the same keys. The engine uses default storage sizes and seven levels. Automatic compaction is disabled to measure maintenance explicitly; a finite L0 stop threshold is sized for the benchmark workload. Background flushing remains active.

Write calls use ASYNC by default. `--sync` measures the engine's synchronous policy. Percentiles are order statistics of per-call steady-clock durations. Insert/GET units are records; batch units are batches of up to 100 mutations; scan units are pages of up to 1,000 rows; flush/compaction/open units are calls. Multiply processed bytes by inverse elapsed seconds to derive maintenance byte throughput. Single-call maintenance/recovery percentiles have one observation and do not characterize a distribution.

Warm random reads follow a flush whose default table verification has populated some engine cache entries. Negative keys append a zero byte to a real key to fall within stored ranges. Explicit compaction sweeps levels and eliminates superseded versions. Recovery timing includes default full-table verification. An additional reopen disables only data verification to start with an empty engine block cache; OS page-cache contents are retained. The resulting read workload warms that initially empty cache as it progresses. It is not an OS-cold benchmark, and it occurs after compaction, unlike the earlier L0 workload.

Storage write bytes are successful WAL appends/headers plus completed flush/compaction SSTable outputs in the main database. Write amplification divides them by its three passes of logical key/value bytes: sequential insert, random update and batch update. The separate fresh-random-insert database is excluded from that ratio. Manifest edits, directory operations, failed partial I/O, filesystem metadata and device-level amplification are excluded. Compaction processed bytes are logical input-file sizes, not measured physical reads. Block-cache misses per GET approximate data-block reads; they are not total storage reads, because metadata was loaded on open. Peak RSS is the whole benchmark process, including workload buffers, runtime, metadata and cached blocks, not exclusively engine memory.

## Reproduction

```sh
cmake --preset release
cmake --build --preset release -j4
build/release/aegisdb-benchmark --count 10000
build/release/aegisdb-benchmark --count 100000 --threads 4
build/release/aegisdb-benchmark --count 1000000
build/release/aegisdb-benchmark --count 1000 --sync
```

`--directory` must identify an absent or empty directory. A successful benchmark removes only the directory it initialized. Avoid concurrent builds/workloads for comparable results and retain compiler/OS/filesystem context. Run multiple repetitions for a performance study; checked-in observations are single runs, not confidence intervals.

## Recorded observations

`results/macos-arm64-10000-async.jsonl`, `results/macos-arm64-100000-async.jsonl` and `results/macos-arm64-1000-sync.jsonl` contain actual measurements on Apple Silicon macOS 27.0.1, Apple Clang 21, Release CMake preset, local filesystem. Final observations are regenerated without concurrent build/test work. Exact chip identification was unavailable in the restricted environment. `macos-arm64-10000-before-growth.jsonl` preserves a quiet earlier run before geometric owner-vector capacity growth; it also predates framing hardening and the fresh-random-insert workload, so it is not an isolated A/B experiment. No comparison with RocksDB or LevelDB is claimed.

The million-record workload is supported by configuration but has not been executed in this session. Linux benchmark runs, OS-cold-cache tests and repeated statistical studies remain pending.
