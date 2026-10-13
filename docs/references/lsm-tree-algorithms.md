# Mathematical and algorithm reference for this implementation

The separately described original reference was not supplied. This document was authored from the project requirements during implementation; it is not a reproduction of that missing document. It records the actual invariants and their source mappings for continuation.

## Definitions

An internal record is `(key, sequence, kind, value)`. Keys/values are binary byte strings. The order is ascending bytewise user key, then descending sequence. A tombstone is a DELETION record with an empty value, distinct from a VALUE with an empty value.

`visible(r,s) := r.sequence <= s`. The logical winner at key `k` and snapshot `s` is the maximum-sequence visible record across all sources. A tombstone winner yields absence.

A committed batch has a consecutive sequence interval. Its WAL representation is one independently checked frame. Publication exposes all nodes and advances the committed frontier as one state transition. Snapshots never capture a frontier inside a batch.

For each data block B, its sparse separator is `max(B)` under the internal comparator. Therefore `lower_bound(separators,target)` selects the first block whose last record is at or beyond the target. Every block has exactly one index entry; adjacent block coverage is contiguous and all records sort strictly.

For every level above L0, adjacent inclusive user-key intervals satisfy `previous.largest < next.smallest`. Output splitting occurs only between distinct user keys.

## Algorithm mappings

1. **Skip-list search:** `SkipList::lowerBound` descends from the maximum level, advancing while the successor precedes the internal target, and returns the level-zero successor.
2. **Skip-list insertion:** `SkipList::prepare` allocates randomized-height nodes and geometrically reserves owner capacity; `publish` retains predecessor links and links each prepared node without allocation.
3. **Memtable reconstruction:** `FramedLog::replay` validates complete frames; `decodeBatch` validates the entire batch; `DBImpl::recover` applies complete batches to ordered memtables in segment order.
4. **Durable write:** `DBImpl::write` validates, serializes admission, reserves sequences/nodes, appends WAL, synchronizes if requested, then publishes nodes/frontier together. Persistence failure disables writes.
5. **Rotation and flush:** `DBImpl::rotate` registers a synced successor WAL before swapping associations. `flushOne` streams the oldest frozen table, syncs/finalizes output and installs AddFile/DeleteLog atomically before source retirement.
6. **Bloom construction:** `BloomFilter::build` computes `m=ceil(-n ln(epsilon)/(ln 2)^2)` and bounded probe count `h=round((m/n)ln 2)` using one hash pair per distinct sorted user key.
7. **Bloom lookup:** `mayContain` checks `(H1+i*H2) mod m` with defined unsigned 64-bit wraparound; any zero bit returns false. A positive answer is only a candidate.
8. **Sparse block selection:** `SSTableReader::selectBlock` binary-searches final internal-key separators with the canonical comparator.
9. **Snapshot point read:** `DBImpl::get` captures a consistent read view, asks every applicable source for its latest visible candidate and selects the maximum sequence.
10. **Range scan:** `MergingIterator` orders one current record per source in a heap. `DBImpl::scan` consumes all versions of each key, selects the first visible one, skips tombstones and bounds page size/bytes.
11. **Leveled compaction:** `CompactionPicker` selects source/overlap inputs. `CompactionExecutor` performs a streaming k-way merge, snapshot-safe retention and user-key-safe output splitting. `compactOne` installs additions/deletions in one durable edit.
12. **PUT:** `Database::put` builds a one-operation WriteBatch and invokes the shared durable write algorithm.
13. **DELETE:** `Database::remove` builds a deletion operation and invokes the same durable write algorithm; it never removes older files in place.
14. **GET:** `Database::get` delegates to the snapshot point-read algorithm; the server only translates bytes and status.
15. **End-to-end recovery:** `DBImpl::recover` reads manifest-authoritative files/logs, reconstructs sequence/memtable state, creates/registers a successor log, drains replayed segments, cleans orphans and starts maintenance.

## Preservation arguments

**Atomic visibility:** Every reader captures the frontier and source associations under `stateMutex_`. Batch publication updates all links and the frontier under that lock plus the memtable lock. A reader either sees the prior frontier or the complete batch.

**No rotation/flush gap:** Rotation changes active/immutable associations atomically. Flush installs the durable replacement Version and removes the immutable source together. A prior read view pins the old sources.

**Compaction visibility:** Let `cutoff` be the oldest active snapshot or the captured current frontier. For each selected key, retain every record above cutoff and the first record at/below it. Every live snapshot's best selected candidate is retained. Unselected sources are unchanged. New snapshots cannot require a sequence below the captured frontier.

**Tombstone safety:** A base tombstone at/below cutoff is discarded only if no unprocessed installed file range contains its key. All older selected versions are also discarded. Mutable/queued sources have later sequences than installed input data because writes serialize and flushes install FIFO. Range uncertainty conservatively retains tombstones.

**Durable installation:** Outputs are synchronized and their names made durable before a checked manifest edit is synchronized. Before that edit, old sources are authoritative. After it, new sources are authoritative. Runtime publication follows commit. Retirement never precedes commit.

**Recovery classification:** Only an incomplete newest tail is accepted as uncommitted. Complete frame headers and payloads are checksummed independently. Complete checksum errors and incomplete older required segments are rejected. Corruption is never reinterpreted as a missing key.

See `../architecture.html` for formats, ownership, concurrency, durability assumptions and current verification limits.
