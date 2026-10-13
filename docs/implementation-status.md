# AegisDB implementation checkpoint

Updated 2026-10-11. Implementation and the executable macOS validation checkpoint are finished. Full project acceptance still requires the supplied real TCP suite outside this environment. The repository was empty at task start. The original mathematical reference was absent; `docs/references/lsm-tree-algorithms.md` now explicitly documents invariants derived from the supplied requirements, not a copy of the missing reference.

## Architecture and implemented modules

- C++20 embedded `Database` facade with private `DBImpl`; Status/Result, binary-safe mutations and bounded atomic batches.
- Explicit little-endian Encoder/Decoder and deterministic CRC32C in `src/core`.
- Injected `FileSystem` / `File` / `DirectoryLock` interfaces and POSIX implementation with short-I/O/EINTR loops, file/directory synchronization, advisory exclusive lock and macOS full-device synchronization where supported.
- `SkipList` with randomized heights, canonical internal ordering, preallocated publication, geometric owner-vector growth and shared-mutex `Memtable` iterators.
- Segmented framed WAL. Header is 16 bytes; frame envelope is 12 bytes (payload length, payload CRC32C, CRC32C of those first eight bytes). Payload is one entire atomic batch. Complete corruption is rejected; incomplete newest tail is truncated/synced.
- `DataBlockBuilder/Reader`, `SSTableBuilder/Reader`, persisted Bloom policy, final-internal-key sparse separators, checksummed metadata/footer and bounded LRU decoded-block cache.
- `Manifest`, `VersionEdit` and pinned immutable `Version` hierarchies. AddFile/DeleteFile, WAL registration/retirement, frontier and allocator edits are durable. CURRENT is synchronized/renamed. Required WAL identities are explicit, not inferred from directory gaps. Live SSTables are manifest-authoritative.
- Snapshot tokens and ordered reference registry; snapshot-aware point reads and streaming heap-merged bounded scan pages. `TableSequenceIterator` lazily concatenates disjoint deeper files, loading one current block per level rather than one per file.
- One owned maintenance jthread serializes background flush and compaction. `CompactionPicker/Executor` implement level scores, overlap coverage, heap merging, snapshot retention, safe tombstone checks and user-key-safe output splitting. Manual compaction can rewrite the bottom level after snapshot release and produce no outputs.
- Immutable queue and L0 bounds, timed write admission/backpressure, fail-stop writes on persistence/background failure, measured counters, safe delayed obsolete-file reclamation.
- Boost.Asio/Beast/JSON standalone server, socket-free `ApiRouter` boundary, strict Base64 API, bounded admission and maintenance registry, injectable clock for snapshot TTL, generation-qualified handles, owned thread pools and signal shutdown.
- Config JSON, standalone benchmark and embedded example, CMake Debug/Release/ASan/TSan presets and installable CMake package.
- Offline 27-chapter handbook generator with source excerpts and checked file/symbol mappings; HTTP API, README, benchmark methodology and MIT license.

## Correctness invariants and review decisions

1. User keys ascend, sequences descend using one comparator everywhere. Empty values differ from tombstones.
2. One batch is one checksummed WAL frame. Allocate nodes before WAL append; append and requested sync precede atomic node/frontier publication.
3. A complete unacknowledged batch can recover. A torn batch cannot partially replay. Persistence failure disables further writes until reopen.
4. Sync SSTables and directory names before a durable manifest edit. Publish replacement Version and remove immutable source in one state transition.
5. WAL retirement follows AddFile/DeleteLog installation, never mere output-file existence.
6. Rotation queue capture is serialized with flush installation. A reviewed race that could restore a flushed source was fixed before final validation.
7. Retain all versions above the oldest snapshot and the first at/below it. Drop a base tombstone only with full absence of unprocessed candidate ranges; conservatively retain on uncertainty.
8. L0 can overlap; L1+ user ranges are strictly disjoint. Never split one user key across adjacent outputs.
9. Read views pin memtables and installed versions. Weak obsolete-file entries defer deletion until table-reader ownership expires. Snapshot history is retained in outputs rather than pinning old physical files indefinitely.
10. Manifest installation and allocations serialize separately from the read-view lock. WAL sync and complete table I/O do not hold the state lock. A mutable scan does hold its memtable shared lock and can delay writes.
11. One maintenance worker is a deliberate initial simplification. Background failure retains recoverable source state and enters fail-stop; there is no automatic blind retry.
12. File numbers are durably reserved and orphan identities are respected. Complete frame headers protect length against corruption. Only an incomplete newest tail can be discarded.
13. Missing CURRENT can recover an incomplete initialization header only when no numbered WAL/SSTable exists. Unregistered WALs at or above the retirement floor containing record bytes cause corruption, preventing silent data loss after manifest-history rollback. Cleanup preserves unrelated filenames.
14. SkipList and SnapshotToken explicitly prohibit copy/move operations that could invalidate links or duplicate release responsibility. MergingIterator cannot move its self-referencing heap comparator.

## Final validation executed

Debug foundation/storage/engine/recovery/router tests passed in incremental runs. The deterministic differential workload has 1,200 operations with seed 0xA615 and prints the trace on failure. Tests cover multiple versions, tiny block boundaries, binary keys, snapshots, restart, non-overlap, empty output, manual L0 pressure, gated immutable-queue timeout, atomic scan visibility, multiple writers/readers and snapshots during maintenance.

Recovery tests passed: SIGKILL after acknowledged batch; injected exit after SSTable synchronization and manifest installation during flush/compaction; WAL sync failure; torn append; failed table/manifest write; missing CURRENT/WAL/SSTable; complete corruption; incomplete manifest tail; reserved orphan cleanup with unrelated-file preservation; sequence preservation; every incomplete initialization-header length; manifest rollback rejection. The WAL final-byte truncation matrix preserves only whole prior batches. A cache-miss assertion verifies lazy deeper-level iteration.

| Configuration | Passed | Failed | Skipped | Scope |
|---|---:|---:|---:|---|
| Debug | 32 | 0 | 1 | 31 GoogleTest cases plus benchmark smoke; TCP suite skipped |
| ASan + UBSan | 32 | 0 | 1 | Same full suite |
| TSan | 4 | 0 | 0 | Atomic scan visibility, many writers/readers/snapshots, bounded queue, real-engine HTTP router |

Final Debug completed in 28.59 seconds, ASan/UBSan in 34.51 seconds, selected TSan in 3.05 seconds. An earlier run reported two timeouts after unusually long host elapsed times; both cases completed successfully and passed on immediate rerun and in the fresh full runs. Machine-readable results and text logs are in `output/verification/{debug,asan,tsan}.xml` and `*-tests.txt`. CTest resolves a relative `--output-junit` beneath its build directory; those final XML files were copied to the common verification directory. Future runs should pass an absolute output path.

Real TCP test execution initially failed on startup because the sandbox rejects localhost `bind()` with EPERM, confirmed using a direct Python socket probe. The suite now explicitly preflights binding and returns CTest skip code 77. Its three HTTP integration cases are present but UNEXECUTED here. Socket-free router tests use the real storage engine; they do not establish TCP correctness.

Debug, Release, ASan/UBSan, TSan and engine-only builds succeeded. The installed CMake package was consumed by a separate `find_package(AegisDB)` application which wrote/read persisted data successfully. Example server JSON parses; invalid deadline configuration rejects before opening storage. Unit/integration build targets were checked by Ninja dry run.

The final server review added cancellation of the pending signal wait when shutdown starts without signal delivery, so an I/O-worker error can drain the event loop. All four server builds and affected router tests were rerun successfully after that change. The socket shutdown path itself remains unexecuted because binding is prohibited.

Clang-format's final `--dry-run --Werror` check passed. Clang-tidy ran across implementation sources. Remaining advice includes safe constant widening, enum sizing, intentional snapshot/stop-token copies, guarded optional access and callback exception boundaries; it is not claimed as a zero-warning audit. Relevant reserve/exception-boundary findings were reviewed and improved.

Final Release benchmark runs at 10,000 and 100,000 ASYNC records and 1,000 SYNC records succeeded without concurrent build/test work. Raw JSON Lines are under `benchmarks/results`; methodology is in `benchmarks/README.md`. Fresh random insertion uses a separate empty database; the main database measures sequential insert, random update, reads, scans, batches, flush, compaction, recovery and concurrent reads. A separate earlier before-growth run is retained but also predates framing changes, so it is not an isolated A/B comparison. No performance superiority is claimed. The million-record workload has not been run.

The offline handbook has 27 chapters, 107 checked links and 40 unique HTML/SVG IDs, with no external runtime assets. Source paths/symbols and excerpts are verified by its generator. Desktop layout was visually inspected in Chrome; responsive CSS was reviewed, but mobile layout was not visually exercised. README, HTTP schema examples and the handbook match the final source.

## Environment and reproducible continuation

Apple Silicon macOS, Apple Clang 21, CMake 4.4.2, Ninja, Boost 1.92. GoogleTest 1.11 is available from a local Conda prefix, with an imported-target-derived BUILD_RPATH workaround. Shell network access is restricted; no dependencies were vendored or downloaded successfully. LLVM clang-format/clang-tidy are installed.

The default CommandLineTools SDK is incompatible with the active linker. Configure this machine using the matching Xcode MacOSX26.5 SDK and set CMAKE_PREFIX_PATH to the local Boost/GoogleTest installations. Generic dependency/build commands are in README and assume no developer username. Linux deployment code is implemented but Linux execution has not occurred.

## Known limitations and unresolved validation

- Local socket binds are prohibited; full standalone server validation is pending outside this sandbox.
- No physical power-loss/device fault or Linux execution. Durability assumes local POSIX filesystem/device synchronization guarantees and already durable existing ancestor directory entries.
- Append-only manifest has no checkpoint rotation. Full table verification on open is the default and can increase recovery time.
- Scans block publication to their pinned mutable memtable. GET materializes owned candidates/values. Cache charge excludes allocator/map/list overhead and evicted but pinned blocks. Table metadata, file handles and references remain resident and grow with installed file count.
- Oversized record/batch/user-key groups may exceed target sizes within hard format bounds. Metadata sections cap at 32 MiB. Large historical groups can hit that explicit limit.
- Maintenance jobs have no hard execution deadline; socket deadlines do not cancel a disk syscall. Shutdown waits for admitted storage work.
- Snapshots/jobs are process-local. Old generation-qualified snapshot IDs are rejected. Pagination without reusing one snapshot is not repeatable.
- Background errors fail stop instead of retrying automatically. Failed/unacknowledged operations can recover completely; no request deduplication exists.
- Obsolete files can persist until another maintenance pass or restart after readers release them; this is safe but conservative.

## Pending acceptance and further validation

1. Execute the three real HTTP/TCP integration cases in an environment permitting localhost binding. Acceptance J and the TCP portion of L remain unverified here.
2. Execute Linux builds, the full test suite and available sanitizers; assess filesystem/device failure behavior on intended deployment storage.
3. Run the million-record benchmark and repeated performance trials, and visually exercise mobile documentation navigation.
4. Preserve the current implementation and tests. Add manifest checkpoint rotation only with crash tests around durable CURRENT transitions.

## Next recommended implementation steps

First execute `ctest --preset debug -R Server.RealTCP` in an environment permitting localhost sockets. Resolve any network/shutdown errors before performance work. Then run the full Linux build/test/sanitizer matrix. Add durable manifest checkpoint rotation after crash tests for CURRENT transitions. Retain the existing storage implementation and incremental tests; do not recreate components or substitute another engine.
