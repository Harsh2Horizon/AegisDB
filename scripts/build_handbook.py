#!/usr/bin/env python3
"""Build the offline handbook from authored chapters and excerpts of the actual source."""
from pathlib import Path
from html import escape
import json
import re
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]


def p(text):
    return '<p>' + text + '</p>'


def note(text):
    return '<aside class="note">' + text + '</aside>'


def equation(text):
    return '<div class="equation">' + text + '</div>'


def table(headers, rows):
    return '<div class="table-wrap"><table><thead><tr>' + ''.join('<th>'+escape(x)+'</th>' for x in headers) + '</tr></thead><tbody>' + ''.join('<tr>'+''.join('<td>'+str(x)+'</td>' for x in row)+'</tr>' for row in rows) + '</tbody></table></div>'


def highlight(text):
    pattern = r'//[^\n]*|"(?:\\.|[^"\\])*"|\b(?:class|public|private|const|auto|return|if|else|while|for|void|bool|std|namespace|static|struct|using|throw|try|catch|noexcept)\b|\b\d+\b'
    result, end = [], 0
    for match in re.finditer(pattern, text):
        result.append(escape(text[end:match.start()]))
        token = match.group()
        kind = 'comment' if token.startswith('//') else 'string' if token.startswith('"') else 'number' if token.isdigit() else 'keyword'
        result.append('<span class="'+kind+'">'+escape(token)+'</span>')
        end = match.end()
    result.append(escape(text[end:]))
    return ''.join(result)


def code(text, language='cpp'):
    return '<pre><code class="'+language+'">'+(highlight(text) if language=='cpp' else escape(text))+'</code></pre>'


def snippet(path, marker, maximum=28):
    lines = (ROOT/path).read_text().splitlines()
    start = next(i for i, line in enumerate(lines) if marker in line)
    selected, depth, begun = [], 0, False
    for line in lines[start:start+maximum]:
        selected.append(line)
        depth += line.count('{')-line.count('}')
        begun |= '{' in line
        if begun and depth == 0:
            break
    suffix = '<small>Excerpt from line '+str(start+1)+'. '+('Continued in the linked source.' if depth else 'Current implementation.')+'</small>'
    return code('\n'.join(selected)) + suffix


def refs(*items):
    result = []
    for path, symbol in items:
        source = ROOT/path
        assert source.is_file(), path
        assert symbol.split('::')[-1] in source.read_text(), (path, symbol)
        result.append('<a href="../'+path+'"><b>'+escape(symbol)+'</b><span>'+escape(path)+'</span></a>')
    return '<div class="sources"><span class="label">IMPLEMENTATION</span>'+''.join(result)+'</div>'


def flow(title, labels):
    width, height = 860, 100 + len(labels)*69
    content = []
    for i, label in enumerate(labels):
        y = 45+i*69
        content.append(f'<rect x="115" y="{y}" width="630" height="48" rx="10"/><text x="430" y="{y+29}" text-anchor="middle">{escape(label)}</text>')
        if i+1<len(labels):
            content.append(f'<path d="M430 {y+48}v21" marker-end="url(#arrow)"/>')
    return '<figure><svg role="img" aria-label="'+escape(title)+'" viewBox="0 0 '+str(width)+' '+str(height)+'"><defs><marker id="arrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="6" markerHeight="6" orient="auto"><path d="M0 0L10 5L0 10z"/></marker></defs>'+''.join(content)+'</svg><figcaption>'+escape(title)+'</figcaption></figure>'


def sequence(title, lanes, events):
    width, gap = 900, 900/(len(lanes)+1)
    height=105+len(events)*54
    content=[]
    for i, lane in enumerate(lanes):
        x=(i+1)*gap
        content.append(f'<text x="{x}" y="27" text-anchor="middle">{escape(lane)}</text><path class="lifeline" d="M{x} 40V{height-15}"/>')
    for i, (origin, target, label) in enumerate(events):
        y=77+i*54; a=(origin+1)*gap; b=(target+1)*gap
        content.append(f'<path d="M{a} {y}H{b}" marker-end="url(#seqarrow)"/><text class="annotation" x="{(a+b)/2}" y="{y-9}" text-anchor="middle">{escape(label)}</text>')
    return '<figure><svg role="img" aria-label="'+escape(title)+'" viewBox="0 0 900 '+str(height)+'"><defs><marker id="seqarrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="6" markerHeight="6" orient="auto"><path d="M0 0L10 5L0 10z"/></marker></defs>'+''.join(content)+'</svg><figcaption>'+escape(title)+'</figcaption></figure>'


def skiplist_diagram():
    content=[]
    positions=[90,260,430,600,770]
    labels=['HEAD','a / 9','a / 7','b / 8','d / 2']
    heights=[3,3,1,2,1]
    for level in (2,1,0):
        y=35+(2-level)*70
        content.append(f'<text x="20" y="{y+21}">L{level}</text>')
        present=[i for i,h in enumerate(heights) if h>level]
        for a,b in zip(present,present[1:]):
            content.append(f'<path d="M{positions[a]+95} {y+17}H{positions[b]}" marker-end="url(#skiparrow)"/>')
        for i in present:
            content.append(f'<rect x="{positions[i]}" y="{y}" width="95" height="34" rx="7"/><text x="{positions[i]+47}" y="{y+22}" text-anchor="middle">{escape(labels[i])}</text>')
    return '<figure><svg role="img" aria-label="Skip-list forward links for ordered internal keys" viewBox="0 0 900 245"><defs><marker id="skiparrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="6" markerHeight="6" orient="auto"><path d="M0 0L10 5L0 10z"/></marker></defs>'+''.join(content)+'</svg><figcaption>Internal keys a/9, a/7, b/8, d/2; taller nodes skip intermediate records.</figcaption></figure>'


def mutation_states():
    states=['Validated / admitted','WAL appended','SYNC complete (or ASYNC)','Published frontier','Acknowledged']
    content=[]
    for i,state in enumerate(states):
        y=20+i*66
        content.append(f'<rect x="250" y="{y}" width="400" height="44" rx="11"/><text x="450" y="{y+28}" text-anchor="middle">{state}</text>')
        if i<4:content.append(f'<path d="M450 {y+44}v22" marker-end="url(#statearrow)"/>')
    content.append('<rect x="10" y="20" width="180" height="44" rx="11"/><text x="100" y="48" text-anchor="middle">Rejected / BUSY</text><path d="M250 42H190" marker-end="url(#statearrow)"/><rect x="700" y="118" width="190" height="52" rx="11"/><text x="795" y="149" text-anchor="middle">Fail-stop writes</text><path d="M650 108H720V118" marker-end="url(#statearrow)"/><path d="M650 174H720V170" marker-end="url(#statearrow)"/>')
    return '<figure><svg role="img" aria-label="Mutation state machine with rejection and persistence-failure branches" viewBox="0 0 900 365"><defs><marker id="statearrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="6" markerHeight="6" orient="auto"><path d="M0 0L10 5L0 10z"/></marker></defs>'+''.join(content)+'</svg><figcaption>Validation/admission rejection leaves durable state untouched. Persistence failures stop later writes until recovery.</figcaption></figure>'


chapters=[]
def chapter(slug,title,body):
    chapters.append((slug,title,body))

chapter('overview','Database overview',
    p('AegisDB stores arbitrary byte strings in a durable, ordered key-value database. Applications can use <code>Database</code> directly or send JSON requests through the standalone HTTP server. Both paths use the same C++20 engine. PUT, GET, DELETE, atomic write batches, snapshot reads, bounded scans, explicit flush and compaction are implemented.')+
    p('An LSM tree turns random logical updates into sequential WAL appends and immutable sorted files. That suits a write-oriented engine, while Bloom filters and sparse indexes reduce the work of point reads. C++20 provides explicit resource ownership, predictable binary layouts through codecs, efficient containers and owned worker threads. Networking uses Boost rather than a custom HTTP parser.')+
    note('<b>Validation boundary.</b> Embedded persistence, fault/crash recovery and API routing have been exercised. The development sandbox prohibits localhost socket binds; real TCP integration is supplied but skipped here. Local process-crash tests are evidence for recovery behavior, not a physical power-loss test.')+
    p('This release is single-node. There is no SQL, replication, consensus, authentication, compression or persistence of snapshots. It targets local POSIX filesystems on Linux and macOS; Windows and network filesystems are unsupported. The append-only manifest does not yet checkpoint its history.')+
    refs(('include/aegisdb/db.h','Database'),('src/engine/db.cpp','Database::open'),('src/server/server.cpp','DatabaseServer')))

chapter('architecture','Complete architecture',
    flow('One storage implementation from client request to durable hierarchy', ['HTTP client → DatabaseServer::Session → ApiRouter', 'Embedded caller → Database facade → DBImpl', 'Serialized mutations → framed WAL → skip-list memtable', 'Frozen memtables → shared maintenance worker → SSTableBuilder', 'Manifest durable edit → immutable Version publication', 'Pinned reads → L0 overlaps / L1+ range search → block cache'])+
    p('<code>Database</code> is the public PImpl facade. <code>DBImpl</code> coordinates logical sequence state, mutable/immutable memtables, WAL association, installed tables and the maintenance lifecycle. <code>Manifest</code> maintains authoritative physical edits. <code>Version</code> is an immutable installed hierarchy whose shared table readers keep file handles alive.')+
    p('One <code>std::jthread</code> serializes background flush and compaction. A separate maintenance mutex also serializes explicit compaction with that worker. This avoids conflicting compactions and lost version edits without a scheduler. The server has bounded socket admission, I/O threads, database request workers and one administrative job worker. Administrative jobs call the same embedded methods.')+
    table(['State','Owner','Lifetime'],[('Logical committed sequence','DBImpl, under stateMutex_','Advances with batch publication'),('Volatile records','Memtable shared ownership','Pinned through reads and flush'),('Installed hierarchy','shared_ptr&lt;const Version&gt;','Replaced after durable manifest commit'),('Physical files','File / DirectoryLock RAII','Reclaimed after readers release')])+
    refs(('src/engine/db_impl.h','DBImpl'),('src/storage/manifest.h','Version'),('src/server/server.h','ApiRouter')))

chapter('model','Storage engine mathematical model',
    p('A record is <code>(key, sequence, kind, value)</code>. Bytewise user keys sort ascending; versions of the same key sort descending by sequence. Kind is VALUE (1) or DELETION (2). An empty value is valid and differs from a tombstone. Empty keys and embedded zero bytes are valid.')+
    equation('(a.key, a.seq) &lt; (b.key, b.seq) ⇔ a.key &lt; b.key ∨ (a.key = b.key ∧ a.seq &gt; b.seq)')+
    equation('visible(r,s) ⇔ r.seq ≤ s; winner(k,s) = arg max { r.seq : r.key = k ∧ r.seq ≤ s }')+
    p('If the winner is a value, return its bytes; if it is a tombstone or absent, return NOT_FOUND. All structures use <code>compareInternal</code> or <code>InternalLess</code>. Batch sequences are consecutive, but the committed frontier jumps only after all nodes have been linked. No snapshot captures an intermediate sequence inside a batch.')+
    snippet('src/core/record.h','inline int compareInternal')+
    p('The original accompanying mathematical document was absent. The repository reference now explicitly records equations and algorithm mappings derived from the project requirements; it does not pretend to be the missing external document.')+
    refs(('src/core/record.h','compareInternal'),('docs/references/lsm-tree-algorithms.md','Algorithm')))

chapter('wal','Write-ahead log',
    p('<code>FramedLog</code> shares checked framing between WAL and manifest streams. A WAL file is named by a 20-digit file identity and <code>.wal</code>. Its 16-byte header contains the eight-byte magic <code>AEGWAL01</code>, little-endian version 1 and CRC32C of the preceding 12 bytes. Segments rotate with memtables when either configured target is crossed.')+
    table(['Frame offset','Encoding','Meaning'],[('0','u32','Payload length, at most 32 MiB'),('4','u32','CRC32C of payload'),('8','u32','CRC32C of length and payload-checksum fields'),('12','bytes','Batch payload: type u8=1, first sequence u64, count u32, records')])+
    p('Each batch occupies exactly one frame. There is no fragmentation and no partial replay. The header checksum protects the length, so a flipped length cannot masquerade as an incomplete tail. Decode validates kind, lengths, record count, sequence overflow and consecutive sequences before publishing the reconstructed batch. CRC32C uses the reflected Castagnoli polynomial <code>0x82f63b78</code>, initial/final inversion and a deterministic lookup table.')+
    snippet('src/storage/log.cpp','void FramedLog::append')+
    p('Only an incomplete last frame of the newest required WAL is discarded. Complete checksum failures, including tail failures, are corruption. Older segments must be complete. Recovery truncates and synchronizes the accepted tail before creating a new segment. Rotation synchronizes the sealed segment even for ASYNC writes. Retirement follows a durable manifest edit that deletes the segment from the required-log set; an SSTable merely existing on disk is insufficient.')+
    refs(('src/storage/log.cpp','FramedLog::replay'),('src/storage/log.cpp','decodeBatch'),('src/engine/db_impl.cpp','DBImpl::rotate')))

chapter('skiplist','Skip-list memtable',
    p('Each node owns its record and a vector of non-owning forward links. A vector of <code>unique_ptr&lt;Node&gt;</code> owns every node; growing the owner vector does not move node addresses. The sentinel is a fixed array of links. No node is individually removed while the memtable is alive.')+
    equation('P(H ≥ ℓ) = p<sup>ℓ−1</sup>; default p = 0.25, maximum height = 16; expected seek/insert O(log n)')+
    skiplist_diagram()+
    flow('Skip-list search and insertion', ['Start at highest sentinel level', 'Advance while next internal key precedes target', 'Descend one level; retain predecessor links', 'Seek returns the level-zero successor', 'Insertion links prepared node into each occupied level'])+
    snippet('src/storage/memtable.cpp','const SkipList::Node *SkipList::lowerBound')+
    p('<code>prepare</code> allocates every batch node and reserves owner capacity before WAL append. Capacity grows geometrically rather than reserving exactly one extra pointer per single-key write. <code>publish</code> links already-owned nodes without allocation. <code>Memtable</code> provides a shared mutex: writers take an exclusive lock for publication, GET takes a brief shared lock, and an ordered iterator holds a shared lock for its full lifetime. Immutable tables are frozen by removing the sole mutation path during rotation.')+
    p('Memory accounting includes node/link/string capacities and unused owner slots, but is an estimate rather than allocator RSS. A bounded batch can overshoot the target. A scan’s mutable-table lock can delay publication; this is the deliberate initial concurrency tradeoff.')+
    refs(('src/storage/memtable.h','SkipList'),('src/storage/memtable.cpp','SkipList::prepare'),('src/storage/memtable.cpp','Memtable::Iterator')))

chapter('sstables','SSTable file format',
    flow('Immutable SSTable byte layout', ['16-byte AEGSST01 header: magic, version, CRC32C', 'Consecutive data blocks: record count + records + CRC32C', 'Bloom filter section + CRC32C', 'Properties section + CRC32C', 'Sparse index section + CRC32C', '52-byte AEGEND01 footer: version, three handles, CRC32C'])+
    table(['Section','Portable fields'],[('Record','u32 key length; key bytes; u64 sequence; u8 kind; u32 value length; value bytes'),('Bloom','u32 format=1; u32 hash policy=1; u64 bit count; u32 hash count; bit array'),('Properties','u64 identity; min/max user-key strings; u64 min/max sequence; u64 record count'),('Sparse index','u32 count; repeated separator string, u64 separator sequence, u64 offset, u32 encoded length'),('Footer','magic[8]; u32 version; Bloom/props/index handles (12 bytes each); CRC32C')])+
    p('Data blocks target 16 KiB and may contain a safely bounded oversized individual record. A block’s encoded bytes include a count and checksum. <code>DataBlockReader::decode</code> validates every record and strict internal ordering. The reader validates footer bounds, adjacent section coverage, index ordering, data-block coverage, file identity and agreement with the manifest. A decoded block must end at its exact separator and sort after the previous separator.')+
    p('Builders write <code>.sst.tmp</code>, synchronize the file, close it, rename to <code>.sst</code>, then synchronize the directory. Installation occurs separately through the manifest. The initial format is uncompressed and has a 32 MiB bound per block/metadata section. SSTables are never modified after finalization. Empty SSTables are format-supported; engine compaction omits empty output files.')+
    refs(('src/storage/sstable.cpp','SSTableBuilder::finish'),('src/storage/sstable.cpp','SSTableReader'),('src/storage/sstable.cpp','DataBlockReader::decode')))

chapter('bloom','Bloom filters',
    equation('m = ceil(−n ln ε / (ln 2)²); h = round((m/n) ln 2); g<sub>i</sub>(k) = (H₁(k) + iH₂(k)) mod m')+
    p('The SSTable builder emits one hash pair per distinct user key by comparing adjacent sorted keys, regardless of how many versions occur. Policy 1 uses two seeded FNV-1a passes followed by a specified 64-bit avalanche; H₂ is made odd. Unsigned addition/multiplication intentionally wraps modulo 2⁶⁴ before the bit-vector modulus. It never relies on <code>std::hash</code> persistence.')+
    snippet('src/storage/bloom.cpp','BloomFilter::Hash BloomFilter::hash')+
    p('The filter has at least 64 bits for a nonempty input and between 1 and 30 probes. The default target is 1%. Encoding persists format, hash policy, bit count, probe count and all bits. An empty filter has zero bits/probes and returns false safely. Decode enforces shape and allocation bounds; the enclosing SSTable checksum protects the serialized bytes.')+
    p('A negative result skips an entire SSTable. A positive result still requires index selection and a block search. Tests check no false negatives at three configured rates and empirically bound observed positives without requiring an exact finite-sample rate. Bloom checks and negative decisions are counted separately.')+
    refs(('src/storage/bloom.cpp','BloomFilter::build'),('src/storage/bloom.cpp','BloomFilter::mayContain'),('tests/foundation_test.cpp','NoFalseNegativesAndMeasuredFalsePositives')))

chapter('index','Sparse block index',
    p('Every nonempty block has one entry: its final internal key, byte offset and encoded length. Separators are strictly ordered. For target <code>(key,snapshot_sequence)</code>, lower_bound selects the first block whose final key is at or beyond the target. This is a coverage index derived from block boundaries, not a full per-record or popularity index.')+
    snippet('src/storage/sstable.cpp','std::size_t SSTableReader::selectBlock')+
    p('A hot seek uses <code>string_view</code> in its binary-search target to avoid allocating a temporary owning key. The in-block lower_bound uses the identical canonical comparator. Different versions of one user key may occupy multiple data blocks: seeking by sequence chooses the correct boundary, and the next logical record is considered only if its user key matches.')+
    table(['Search stage','Purpose','Structure'],[('Level metadata','Identify candidate files','All overlapping L0 files; one binary-searched file in each deeper level'),('Sparse index','Identify a candidate data block','Complete in-memory separator vector'),('Data block','Find latest visible candidate','Decoded ordered record vector and lower_bound')])+
    p('All opened table indexes are resident in memory. That favors predictable seeks over partitioned-index complexity; index memory grows with block count and key lengths. The checked 32 MiB section limit makes this explicit. Partitioned indexes and shortest separators are future enhancements.')+
    refs(('src/storage/sstable.h','IndexEntry'),('src/storage/sstable.cpp','SSTableReader::get')))

chapter('reads','Point read path',
    flow('GET chooses the maximum visible sequence across all candidates', ['Capture committed sequence + memtables + installed Version under state lock', 'Mutable and immutable skip-list visible seeks', 'L0 overlap search; L1+ binary range selection', 'Table range → Bloom → sparse separator → cached/read data block', 'Choose highest visible sequence, including tombstones', 'VALUE returns bytes; DELETION/absence returns NOT_FOUND'])+
    p('The read view is captured as one state transition: a reader sees either the old immutable source plus old installed version, or the newly installed table. Rotation cannot create a gap. Subsequent writes may insert into a pinned mutable table, but their sequences exceed the captured frontier and remain invisible. Every candidate competes by sequence; source ordering alone never determines the winner.')+
    snippet('src/engine/db_impl.cpp','Result<std::string> DBImpl::get',32)+
    p('L0 may contain overlapping ranges, so all candidate tables are examined. Deeper levels are non-overlapping and sorted, allowing one range candidate per level. GET returns a copied owned value, avoiding unsafe views into mutable or evictable storage. A discovered corruption is reported rather than treated as a miss. Existing reads may continue after a write-side fail-stop error using the last published view.')+
    refs(('src/engine/db_impl.cpp','DBImpl::readView'),('src/engine/db_impl.cpp','DBImpl::get'),('src/storage/sstable.cpp','SSTableReader::get')))

chapter('scans','Range scans',
    p('Each memtable and overlapping L0 SSTable exposes a <code>RecordIterator</code>. Disjoint files in each deeper level become one <code>TableSequenceIterator</code>: a binary range seek selects the first table, and only the current table loads a block. This avoids pinning one data block for every deeper file. <code>MergingIterator</code> uses <code>std::priority_queue</code> ordered by internal key, with source identity breaking exact ties. Advancing a heap winner advances only that source.')+
    code('capture read view at sequence s\nseek all sources to max(start, after)\nwhile heap has a record in [start,end):\n    key = smallest user key\n    consume all versions of key in descending sequence order\n    winner = first record whose sequence <= s\n    skip if winner is missing, deleted, or key <= after\n    append winner if result and byte budgets permit\n    return exclusive continuation at the last emitted key when bounded','text')+
    p('The engine never materializes the complete database for a scan. Record-data memory is bounded by the admitted memtables, overlapping L0 sources, one current block per deeper level and the output page. Table references and resident indexes still grow with file count. Duplicate physical versions of a key produce one logical result. The merge consumes all versions of a key before returning its value, so versions spanning files or blocks cannot leak duplicate rows.')+
    p('Scans return ascending bytewise keys over a half-open range. Pagination uses an explicit exclusive user key; it has no hidden snapshot change. Embedded callers reuse a Snapshot; server clients reuse a generation-qualified snapshot ID. An absent snapshot gives each page a fresh frontier. A continuation may be conservative when remaining rows are deleted.')+
    refs(('src/engine/merge.h','MergingIterator'),('src/engine/merge.h','TableSequenceIterator'),('src/engine/db_impl.cpp','DBImpl::scan'),('include/aegisdb/db.h','ScanOptions')))

chapter('snapshots','Snapshots and MVCC',
    table(['Event','Current GET(x)','Snapshot GET(x)'],[('PUT x=A, sequence 10','A','—'),('Create S at frontier 10','A','A'),('PUT x=B, sequence 11','B','A'),('DELETE x, sequence 12','NOT_FOUND','A')])+
    p('<code>SnapshotState</code> owns an ordered map from sequence to active token count. Creating a snapshot occurs under the database state lock and then the snapshot mutex, registering the committed frontier. Copies share a <code>SnapshotToken</code>; the token destructor decrements the map only when its final owner releases it. A handle can outlive the database because its registry has separate shared ownership.')+
    equation('cutoff = oldest active snapshot, or captured committed frontier if none; retain all seq &gt; cutoff and the first seq ≤ cutoff for each selected key')+
    p('Compaction uses this conservative cutoff. New snapshots created during compaction cannot require a sequence earlier than the captured frontier, while older existing snapshots were registered before input selection. Read operations pin their pre-compaction Version and memtables; file reclamation respects those pins. Snapshot tokens do not need to pin obsolete physical files after their historical winners have been preserved in replacement tables.')+
    p('Handles belong to a particular database instance. Using one after reopen or with another database returns INVALID_ARGUMENT. Server handles have a bounded TTL, explicit deletion and periodic cleanup. Neither embedded nor server snapshots survive restart. Releasing snapshots permits further historical cleanup; explicit compaction can rewrite bottom-level files to reclaim retained versions and tombstones.')+
    refs(('src/engine/db_impl.h','SnapshotToken'),('src/engine/db_impl.cpp','DBImpl::snapshot'),('src/server/router.cpp','ApiRouter::expireSnapshots')))

chapter('flush','Memtable rotation and flushing',
    sequence('Durable flush installation with no read-visibility gap',['Writer','Maintenance','Filesystem','Manifest'],[(0,2,'create/sync successor WAL'),(0,3,'register successor WAL durably'),(0,1,'freeze and enqueue old memtable'),(1,2,'stream records → sync/rename SSTable'),(1,3,'AddFile + DeleteLog in one edit'),(1,0,'publish Version and remove immutable'),(1,2,'remove retired WAL; sync directory')])+
    p('Rotation is serialized with mutation admission. It synchronizes the old WAL, registers the successor segment through the manifest and swaps the active/immutable associations under the state lock. Capturing the replacement queue while holding the manifest-installation mutex prevents a concurrent completed flush from being reintroduced. An immutable source is never modified.')+
    p('<code>flushOne</code> selects the oldest immutable source and streams its iterator into a builder outside the global state lock. It opens/verifies the finished output, prepares a replacement Version, then commits AddFile plus DeleteLog and the new WAL floor as one logical edit. Publication replaces the Version and removes the immutable in one critical section. Only then can the old WAL be removed.')+
    p('The immutable queue is bounded. Admission waits on a condition variable and returns BUSY at the configured timeout. One bounded batch can overshoot the mutable target; it is frozen immediately when capacity permits, otherwise the next write waits. A failed flush retains the memtable and dependent WAL and puts the engine into fail-stop write mode. Recovery can reconstruct the source; automatic blind retries are deliberately absent.')+
    refs(('src/engine/db_impl.cpp','DBImpl::rotate'),('src/engine/db_impl.cpp','DBImpl::flushOne'),('src/engine/db_impl.cpp','DBImpl::waitForRoom')))

chapter('levels','Level organization',
    equation('capacity(Lℓ) = levelOneBytes × R<sup>ℓ−1</sup>, for ℓ ≥ 1; default R = 10')+
    p('Direct flush outputs enter L0 and may overlap. Levels 1 through the configured final level contain sorted, strictly disjoint inclusive user-key ranges: the previous file’s largest key is less than the next file’s smallest key. A user key is never split across adjacent compaction outputs, even if that group exceeds the output-size target.')+
    table(['Level','Read selection','Compaction pressure'],[('L0','All ranges containing the key','File count / l0CompactionFiles'),('L1+','Binary search by largest user key','Stored bytes / configured capacity'),('Final level','Same disjoint range search','No automatic next-level promotion; explicit rewrite can reclaim history')])+
    p('Every manifest file record carries identity, level, encoded file size, user-key bounds and sequence bounds. Manifest replay validates level ranges and duplicate identities. Increasing level count on reopen is supported; decreasing it below existing installed levels fails validation. File identities are reserved durably before use and never reused, including orphan identities found during recovery.')+
    p('L0 admission is bounded even with automatic compaction disabled. Maintenance can temporarily add at most the bounded queued flushes beyond the stop threshold. In manual mode, explicit compaction first merges existing L0 to free room before flushing active data. Larger ratios reduce the number of levels but change overlap and write/read amplification; no universal optimum is claimed.')+
    refs(('src/storage/sstable.h','TableMeta'),('src/storage/manifest.cpp','Manifest::apply'),('src/engine/compaction.cpp','CompactionPicker::pick')))

chapter('compaction','Leveled compaction',
    flow('Compaction preserves logical reads and atomically replaces physical inputs', ['Pick highest pressure: all L0, or one source file from a deeper level', 'Include every overlapping next-level file', 'Pin Version; stream ordered iterators through a min-heap', 'Retain snapshot winners; remove only proven-safe tombstones', 'Split outputs only between distinct user keys', 'Sync outputs; one manifest edit adds outputs and deletes inputs', 'Publish Version; reclaim inputs after their table-reader pins expire'])+
    p('The automatic picker scores L0 by count and deeper source levels by size. L0 uses every current L0 input to simplify overlap closure. Deeper automatic compactions select the first source file from the highest-scored level and all its next-level overlaps. Explicit compaction uses all files in each source level for a deterministic sweep. If only bottom-level files remain, it rewrites that level in place for historical cleanup. The executor groups disjoint inputs from each deeper level into a lazy table sequence; L0 inputs remain separate heap sources.')+
    snippet('src/engine/compaction.cpp','std::vector<TableMeta> CompactionExecutor::run',28)+
    p('Per key, records above the snapshot cutoff survive. The first at or below the cutoff survives unless it is a tombstone and every unprocessed file range excludes the key. All older selected versions are then redundant. A range false positive retains extra history. This base-level check scans all other installed levels, including shallower levels, and is safe because current memtables contain later sequences than already installed input data.')+
    p('No input file is deleted just because output exists. Addition/deletion sets are installed together. The executor handles zero surviving outputs by committing only input deletions. Old readers keep their tables through shared ownership; weak retirement entries are cleaned when expired or on a later restart. Reclamation can be delayed until the next maintenance pass, which favors safety over prompt deletion.')+
    refs(('src/engine/compaction.cpp','CompactionPlan::baseLevelFor'),('src/engine/compaction.cpp','CompactionExecutor::run'),('src/engine/db_impl.cpp','DBImpl::compactOne')))

chapter('manifest','Manifest and installed versions',
    p('<code>CURRENT</code> contains the exact name <code>MANIFEST-000001</code> plus newline. The pointer is installed using a synchronized temporary file, atomic rename and directory sync. A missing pointer in an already edited manifest is corruption. A partial initialization header can be recreated only when no reserved WAL/SSTable identity exists: no acknowledged mutation was possible at that boundary. The manifest uses magic <code>AEGMAN01</code> and the same checksummed frame envelope as the WAL.')+
    table(['Edit field','Encoding / role'],[('Type + flags','u8 type=1, u8 optional-field mask'),('SetLogNumber','Optional u64 minimum required WAL identity'),('SetLastSequence','Optional u64 committed/recovered frontier'),('SetNextFileNumber','Optional u64 monotonic allocator frontier'),('AddFile','u32 count + TableMeta records'),('DeleteFile / AddLog / DeleteLog','Each is u32 count + repeated u64 identities')])+
    p('Explicit AddLog/DeleteLog sets let recovery detect a missing segment between other file identities. The manifest is authoritative for live SSTables; directory scans are used only to reserve orphan identities and safely clean non-installed files. <code>Manifest::commit</code> prepares and validates a copied state, appends/synchronizes the edit, then replaces its durable-state mirror. <code>applyVersion</code> prepares the separately pinned runtime hierarchy.')+
    snippet('src/storage/manifest.cpp','void Manifest::commit')+
    p('Installed Versions are published only after commit. A persistence failure can leave a complete but unacknowledged durable edit; recovery replays it, while the running process stops accepting writes. Partial final edit frames are discarded and truncated. Complete corrupted frames are rejected. Checkpointing/manifest rotation is future work; history currently grows with allocations and installations.')+
    refs(('src/storage/manifest.cpp','Manifest::allocate'),('src/storage/manifest.cpp','applyVersion'),('src/engine/db_impl.cpp','DBImpl::reclaim')))

chapter('recovery','Crash recovery',
    code('lock database directory exclusively\nread CURRENT; replay checked manifest edits\nopen and validate every required SSTable\nfor required WAL identities in order:\n    replay only complete, checksummed atomic batches\n    enforce increasing sequences and rebuild ordered memtables\n    truncate/sync only an allowed incomplete newest tail\nreserve a new identity; create/sync a new writable WAL\nregister successor WAL and recovered frontier durably\nflush reconstructed old segments in FIFO order\nclean orphan files and synchronize directory\nstart maintenance worker; accept requests','text')+
    table(['Crash boundary','Recovery source','Why data remains recoverable'],[('During WAL append','Previous complete frames','Partial final batch is never replayed'),('After sync, before publication/response','Complete WAL frame','Unacknowledged batch may become visible on restart'),('During rotation','Explicit required WAL set','Old segment remains live; empty successor is harmless'),('During SSTable write/rename','Required WAL','Uninstalled table is an orphan'),('After table manifest installation','Installed SSTable','DeleteLog is atomic with AddFile'),('During compaction before edit','Old installed inputs','New outputs are not authoritative'),('After compaction edit, before publication/deletion','New installed outputs','Replay applies additions and deletions atomically'),('Before WAL retirement','Manifest-installed SSTable','Extra old WAL is safely ignored/cleaned')])+
    p('The committed frontier is the maximum of manifest state and all replayed complete batches. Required WALs are replayed even if their sequences are below the manifest frontier, because rotation may have recorded a frontier while those records still live only in WAL/memory. Recovery drains old segments before serving requests, keeping the runtime immutable queue within its configured bound.')+
    p('Missing required files, truncated live SSTables, unsupported formats and complete checksum errors fail open. A numbered unregistered WAL at or above the retirement floor must contain only an initialization header. If it has record bytes, recovery rejects possible lost manifest history instead of silently deleting acknowledged data. Orphan cleanup touches only reserved numeric WAL/SSTable/temp names and CURRENT.tmp, preserving unrelated files. There is no salvage mode.')+
    p('<code>verifyTablesOnOpen</code> defaults true and reads every data block; disabling it leaves data validation until a block is accessed. Checksummed metadata and file-size agreement remain mandatory. Crash tests use SIGKILL and injected abrupt process exits at persistence boundaries, not only graceful close.')+
    refs(('src/engine/db_impl.cpp','DBImpl::recover'),('src/storage/log.cpp','FramedLog::replay'),('tests/recovery_test.cpp','CrashDuringFlushAndCompactionInstallation')))

chapter('concurrency','Concurrency model',
    table(['Lock / boundary','Protects','Important rule'],[('writeMutex_ (timed)','Serialized mutations and rotation','Validation precedes admission; bounded timeout'),('stateMutex_','Read-view capture, committed frontier, publication, failure state','Never held across WAL sync or full table writes'),('Memtable shared_mutex','Skip-list links and ownership','Scan iterator holds shared access through its page'),('manifestMutex_','File allocation and durable edit installation','Queue capture and version install serialize here'),('maintenanceMutex_','Flush/compaction execution and retired-file list','No conflicting maintenance ranges'),('SnapshotState::mutex','Ordered active-sequence map','Acquired after state lock when both are needed'),('BlockCache mutex','LRU/map and cache charge','Pinned immutable blocks survive eviction')])+
    p('Mutations validate, assign sequences and preallocate outside the state lock, append/sync WAL, then take state and memtable locks for atomic publication. Read views pin a consistent collection before any I/O. Rotation and flush publication use the same state boundary. Manifest commits hold the installation mutex across synchronization but do not hold the read-view lock.')+
    p('The maintenance worker sleeps on a condition variable and owns its lifetime through <code>std::jthread</code>. It wakes for queued flushes or scored compaction. Backpressure similarly waits on a condition variable. There are no detached threads or polling loops in the engine. A storage error wakes blocked writers and disables further acknowledgments.')+
    p('Database destruction requires callers to have finished all API calls. It stops/joins the worker and synchronizes the active WAL; destruction does not require flushing it into a table because recovery can replay it. The server closes admission, cancels sessions, joins storage/job workers and drains cancellation completions. A snapshot token can outlive its engine without using destroyed locks.')+
    refs(('src/engine/db_impl.cpp','DBImpl::background'),('src/storage/memtable.cpp','Memtable::publish'),('src/server/server.cpp','DatabaseServer::run')))

chapter('oop','Dependency injection and ownership',
    p('The meaningful infrastructure boundary is <code>FileSystem</code>, with <code>File</code> and <code>DirectoryLock</code> RAII interfaces. The public factory chooses POSIX in its composition root unless <code>Options::filesystem</code> supplies another implementation. WAL, SSTable builder/reader, manifest and compaction receive that boundary explicitly; encoding is shared in one codec rather than duplicated across storage formats.')+
    snippet('include/aegisdb/filesystem.h','class FileSystem',24)+
    p('<code>CompactionPicker</code> holds immutable policy options and has no file-writing responsibilities. <code>CompactionExecutor</code> receives filesystem, path, options, metrics and an allocator callback, then produces metadata for installation by DBImpl. <code>ApiRouter</code> receives the actual Database and an injectable steady-clock function; it owns no TCP sockets. DatabaseServer owns sockets and request admission, then delegates routing.')+
    p('Tests wrap the real filesystem using <code>FaultFileSystem</code>: a frame can be torn, sync can fail, manifest installation can fail, or the child process can exit immediately after a table/manifest synchronization. A condition-variable gate freezes table output to test queue bounds without timing races. Router TTL tests advance an injected clock rather than waiting for wall time.')+
    p('Shared ownership is used only for pinning mutable/immutable memtables, immutable table readers/versions and snapshot state. File descriptors, directory locks, builders, WAL writers, engine implementation and iterator objects have unique ownership. Non-owning skip-list links and component references are safe because their owning roots outlive every use. SkipList cannot move its sentinel/owners, SnapshotToken cannot duplicate release responsibility, and MergingIterator cannot move away from its self-referencing heap comparator. Private storage headers are not part of the installed public API.')+
    refs(('src/filesystem/posix.cpp','PosixFileSystem'),('tests/fault_fs.h','FaultFileSystem'),('src/server/server.h','ApiRouter')))

chapter('server','Server architecture',
    sequence('HTTP request lifecycle',['Client','Session / I/O','Request pool','Database'],[(0,1,'HTTP/1.1 bytes; Beast body/header limits'),(1,2,'admitted request, one per connection'),(2,3,'ApiRouter → embedded API'),(3,2,'Status / owned result'),(2,1,'post sanitized JSON response to strand'),(1,0,'async response; close connection')])+
    p('Boost.Asio provides TCP, strands, signals, deadlines and owned thread pools. Boost.Beast parses HTTP; Boost.JSON parses JSON. Sessions use <code>beast::tcp_stream</code> deadlines and a bounded request parser. The listener creates each session on its own strand and limits live connections. Excess sockets are closed before request parsing, keeping request-worker admission bounded by the connection count.')+
    p('Database calls run on the configured request pool, so WAL sync and GET/SCAN disk work do not run on I/O threads. Admin requests enqueue bounded jobs on one separate worker and return 202 with a status ID. Snapshot TTL cleanup runs on a steady timer. Router exceptions become category-only JSON errors, while operator logs carry infrastructure context.')+
    p('Configuration controls host, port, database directory, worker/I/O counts, connection/request limits, timeouts, registry sizes and engine options. Defaults are localhost, port 8080, two I/O threads and four database workers. The initial protocol closes each connection after one response and has no optimized custom wire format, TLS or authentication. Socket deadlines do not cancel an admitted filesystem operation.')+
    note('The real server executable is compiled. The supplied TCP suite exercises requests, concurrency, oversized bodies, restart and shutdown, but cannot execute in this sandbox because bind() is prohibited. Router tests exercise actual handlers and storage without making a claim about socket behavior.')+
    refs(('src/server/server.cpp','DatabaseServer::Session'),('src/server/router.cpp','ApiRouter::route'),('src/server/config.cpp','parseOptions')))

chapter('put','Complete PUT walkthrough',
    mutation_states()+
    flow('End-to-end durable PUT', ['POST /v1/kv/put: strict Base64 fields, optional sync=true', 'ApiRouter → Database::put → one-operation WriteBatch', 'Validate sizes/types → timed writer admission', 'Assign consecutive sequence → encode batch → allocate skip-list nodes', 'Append checksummed WAL frame → synchronize if requested', 'Publish all nodes + committed sequence under state lock', 'Rotate/schedule when bounded capacity permits → acknowledge'])+
    snippet('src/engine/db.cpp','Status Database::put')+
    p('The write API validates the complete batch before modifying durable state. Preallocating skip-list nodes avoids acknowledging a persisted batch that cannot be linked due to a later ordinary allocation failure. The implementation intentionally synchronizes before publication; this is a stronger ordering than exposing data while durability synchronization is still pending. Snapshots and current reads see only the committed frontier.')+
    p('SYNC acknowledges only after WAL sync and successful publication. ASYNC skips the device sync at this point. Rotation still seals/syncs segments and durably records successor association. A post-publication backpressure timeout postpones rotation while the already committed mutation succeeds; before-publication BUSY does not commit. A persistence error fails stop and can still leave an unacknowledged complete batch for recovery.')+
    p('The guarantee assumes a local POSIX filesystem/device honoring file and directory fsync, same-directory atomic rename and advisory locking. Existing ancestor directory names must already be durable; newly created ancestors have their parent entries synchronized. macOS additionally requests F_FULLFSYNC where supported. No physical power-loss guarantee has been experimentally established in this session.')+
    refs(('src/server/router.cpp','ApiRouter::route'),('src/engine/db_impl.cpp','DBImpl::write'),('src/storage/memtable.cpp','SkipList::publish')))

chapter('get','Complete GET walkthrough',
    p('The HTTP router decodes the user key and resolves an optional snapshot handle. Unknown or expired handles produce 404 before reading storage. <code>Database::get</code> forwards to DBImpl, which captures the frontier and source lifetimes under the state lock. It does not duplicate storage logic in the router.')+
    p('The mutable and immutable skip lists seek the internal target and return at most one visible candidate each. L0 readers first reject non-overlapping file ranges, then consult Bloom filters. Each deeper level binary-searches its disjoint file ranges. Positive Bloom results enter the sparse-index and in-block binary searches. All candidates compete by sequence, so a newer tombstone masks every older value regardless of where it resides.')+
    snippet('src/storage/sstable.cpp','std::optional<Record> SSTableReader::get',28)+
    p('A cached block bypasses the data-file read. On a miss, the reader validates its checksum, record ordering, separator and range/sequence metadata before caching it. The returned value owns its bytes, so its lifetime is independent of cache eviction and file reclamation. Read-hit/miss metrics count logical outcome; block/Bloom metrics count physical decisions. The HTTP result encodes the value again as canonical Base64.')+
    refs(('src/engine/db.cpp','Database::get'),('src/storage/sstable.cpp','SSTableReader::readBlock'),('src/storage/cache.cpp','BlockCache::get')))

chapter('delete','DELETE and tombstones',
    p('DELETE creates a DELETION record with a new sequence and empty payload. It follows the identical write-batch/WAL/publication path as PUT. No in-place SSTable removal occurs. A current read returns NOT_FOUND if that tombstone is the greatest visible sequence, while an older snapshot can still read an earlier VALUE.')+
    equation('L0: (k,20,DELETION), L2: (k,5,VALUE,old) ⇒ current GET(k) = NOT_FOUND')+
    p('Removing the L0 tombstone without covering L2 would resurrect <code>old</code>. The executor therefore checks every unprocessed file range before dropping a retained base tombstone. If any range can contain that key, the tombstone survives. If complete coverage and the snapshot cutoff prove it redundant, dropping the tombstone also drops older selected values; the logical result remains absent.')+
    p('Retention is conservative: file-range overlap may keep a tombstone even when no actual older value exists. New active memtable mutations have later sequences and cannot introduce an older installed value after the check. Background maintenance is serialized, so concurrent table installation cannot invalidate the selected coverage. Explicit bottom-level rewriting after snapshot release can safely produce no output, which the manifest installs as only input deletions.')+
    refs(('src/engine/db.cpp','Database::remove'),('src/engine/compaction.cpp','CompactionPlan::baseLevelFor'),('tests/engine_test.cpp','SnapshotReleaseReclaimsBottomTombstonesAndEmptyOutputs')))

def benchmark_table():
    rows=[]
    for n in (10000,100000):
        path=ROOT/f'benchmarks/results/macos-arm64-{n}-async.jsonl'
        if not path.exists():
            continue
        for line in path.read_text().splitlines():
            row=json.loads(line)
            if row.get('workload') in ('fresh_random_insert','sequential_insert','random_update','random_read_warm_engine_cache','concurrent_read'):
                rows.append((f'{n:,}',row['workload'],f"{row['ops_per_second']:,.0f}",f"{row['p50_us']:.3f}",f"{row['p99_us']:.3f}"))
    return table(['Records','Measured workload','Operations/s','p50 µs','p99 µs'],rows) if rows else note('Benchmark results have not been generated yet.')

chapter('performance','Performance and resource accounting',
    p('Insertion is expected O(log n) in the skip list plus sequential WAL bytes and optional synchronization. A point lookup consults bounded memtables, all L0 candidates and one candidate per deeper level; each table adds Bloom probes, O(log blocks) sparse search and O(log records/block) search. A scan/compaction over N internal records and K sources is O(N log K). K counts overlapping L0 inputs and one ordered source per deeper level, plus memtables for scans. Record/block memory streams; table metadata and open-file handles remain resident.')+
    p('The LRU cache uses an unordered map and list under one mutex. Its key is monotonically unique file identity plus block offset. Only decoded immutable blocks are cached. Insertion charges vector/record/string capacities; map/list and allocator overhead are not charged. An evicted block pinned by a reader remains alive outside resident cache accounting. Frequently accessed metadata/indexes stay in table readers.')+
    benchmark_table()+
    p('The table above is generated from raw Release-build JSON Lines. Workloads use 16-byte keys, 128-byte values, seed 615, four read threads and ASYNC writes. Fresh random insertion uses a separate empty database. The main database measures sequential writes, random updates to the same set and batches of 100 operations. Final measurements run without concurrent build/test work. The retained before-growth run also predates framing hardening and is not an isolated optimization experiment. No external database comparison is made.')+
    equation('write amplification = (WAL + flush + compaction output bytes) / logical mutation bytes; measured read proxy = block-cache misses / GET; space amplification = physical live bytes / logical live bytes')+
    p('The benchmark emits average and p50/p95/p99 latency, throughput, scan-page bytes, flush/compaction duration and processed bytes, recovery time, cache misses per GET and peak process RSS. WAL+SSTable write amplification covers the main database and excludes the separate random-insert database and manifest/directory traffic. Compaction input bytes are logical file sizes, not measured physical reads. Space amplification is explained but not reported as a measured ratio. The initially empty engine-cache workload does not evict the OS cache. Warm L0 reads and post-compaction reads have different file organization, so their rates are not a cache-only comparison.')+
    refs(('apps/benchmark_main.cpp','Measurement'),('src/storage/cache.cpp','BlockCache::insert'),('benchmarks/README.md','Methodology')))

def verification_table():
    rows=[]
    for name in ('debug','asan','tsan'):
        path=ROOT/f'output/verification/{name}.xml'
        if path.exists():
            root=ET.parse(path).getroot(); cases=list(root.iter('testcase'))
            failures=sum(c.find('failure') is not None or c.find('error') is not None for c in cases)
            skipped=sum(c.find('skipped') is not None for c in cases)
            rows.append((name,len(cases)-failures-skipped,failures,skipped,len(cases)))
    return table(['Executed configuration','Passed','Failed','Skipped','Total'],rows)

chapter('testing','Testing and verification',
    verification_table()+
    p('GoogleTest cases compare skip-list output with a sorted reference, verify CRC32C and strict binary boundaries, truncate every byte position of a final WAL batch, check Bloom false negatives/rates, and seek SSTable versions across tiny block boundaries. Engine cases exercise restart, binary keys, updates/deletes, multiple snapshots, non-overlap, empty compaction output, manual L0 pressure and queue timeouts.')+
    p('The deterministic differential workload uses seed <code>0xA615</code> and 1,200 operations across PUT, DELETE, GET, SCAN, batch, snapshot, flush, compact and reopen. It compares current and historical views with standard-container models and includes the operation trace on failure. Concurrency tests use barriers and explicit gates: atomic pair writes cannot appear as mismatched scan values, snapshots race safely with multiple writers, and a blocked flush proves bounded admission.')+
    p('Recovery tests run child processes with an acknowledgment pipe, send SIGKILL after a durable batch, and inject exits after table synchronization or manifest installation during flush/compaction. Other faults include sync failure, partial WAL append, failed output/manifest writes, missing files, corrupted frames, reserved orphan cleanup, every initialization-header truncation and manifest rollback detection. Complete failed-sync batches may reappear after restart, while torn batches never partially replay. A lazy-iterator test verifies that seeking within a disjoint level reads only the current table.')+
    note('The real TCP suite is registered as <code>Server.RealTCP</code>. It exited with the designated skip code after a direct bind permission probe. Its HTTP/concurrency/restart/shutdown claims are unexecuted here. Router tests use the real engine and an injected clock, but do not replace TCP validation. Linux execution and physical power-loss tests remain unexecuted.')+
    p('ASan/UBSan and selected TSan tests are run using separate presets. Clang-format has been applied. Clang-tidy was executed; its remaining advice is tracked as review information, not reported as a clean zero-warning audit. Every final source change requires an appropriate rerun before its result can be claimed.')+
    refs(('tests/engine_test.cpp','DeterministicDifferentialWithHistory'),('tests/recovery_test.cpp','SIGKILLAfterAcknowledgedBatch'),('tests/server_test.py','ServerTest')))

chapter('usage','Build and usage',
    p('Use CMake 3.24+, a C++20 compiler, Ninja, Boost 1.75+ with JSON, GoogleTest and Python 3. The core library needs only standard C++ and POSIX. Linux package instructions and macOS SDK/dependency guidance are in README. The macOS development environment used a matching Xcode SDK because its default CommandLineTools SDK was incompatible with the linker.')+
    code('cmake --preset debug\ncmake --build --preset debug -j4\nctest --preset debug\ncmake --preset release\ncmake --build --preset release -j4\nbuild/release/aegisdb-server --config config/example.json\nbuild/debug/aegisdb-example ./example-data\nbuild/release/aegisdb-benchmark --count 100000\ncmake --install build/release --prefix /your/install/path','text')+
    code("curl -s -H 'Content-Type: application/json' http://127.0.0.1:8080/v1/kv/put \\\n  -d '{\"key_base64\":\"dXNlcjox\",\"value_base64\":\"SGVsbG8=\",\"sync\":true}'\ncurl -s -H 'Content-Type: application/json' http://127.0.0.1:8080/v1/kv/get \\\n  -d '{\"key_base64\":\"dXNlcjox\"}'",'text')+
    code(r'''curl -s -H 'Content-Type: application/json' http://127.0.0.1:8080/v1/kv/delete \
  -d '{"key_base64":"dXNlcjox","sync":true}'
curl -s -H 'Content-Type: application/json' -d '{}' http://127.0.0.1:8080/v1/snapshots
# Copy the returned ID; reuse it for every scan page.
SNAPSHOT_ID='returned-generation-and-counter'
curl -s -H 'Content-Type: application/json' http://127.0.0.1:8080/v1/kv/scan \
  -d "{\"start_key_base64\":\"YQ==\",\"end_key_base64\":\"eg==\",\"limit\":100,\"snapshot_id\":\"$SNAPSHOT_ID\"}"
curl -s -X DELETE "http://127.0.0.1:8080/v1/snapshots/$SNAPSHOT_ID"
''','text')+
    p('Reopen the same database directory to recover persisted state; the directory lock prevents two writable opens. Do not delete CURRENT, live WALs or manifest-listed SSTables. The engine checks limits before mutation. Shutdown with SIGTERM/SIGINT, and handle uncertain outcomes for canceled requests. The API chapter linked below includes batches, continuation fields and maintenance polling.')+
    refs(('README.md','Build'),('docs/http-api.md','Scans'),('config/example.json','storage'),('CMakeLists.txt','aegisdb')))

chapter('tradeoffs','Design tradeoffs',
    table(['Decision','Benefit','Cost / alternative'],[('Skip list','Expected logarithmic seek/insert, ordered iteration','Per-node allocations; tree/arena designs could improve locality'),('Segment per memtable','Simple WAL retirement proof and bounded replay units','Rotation metadata synchronization; separate large WAL grouping is possible'),('Immutable SSTables','Stable readers, cache keys and atomic replacement','Updates rewrite data during compaction'),('Sparse indexes + Bloom','Small resident metadata and inexpensive negative filtering','Extra block reads on positives; full indexes would consume more memory'),('Leveled hierarchy','Disjoint deeper ranges reduce point-read candidates','Overlap rewriting creates write amplification'),('Conservative MVCC/tombstone rules','Preserves historical reads and prevents resurrection','Retains more history than snapshot-specific optimization'),('Serialized writer','Simple sequence and batch-publication proof','Limits write concurrency; group commit is future work'),('One maintenance worker','No conflicting compactions/installations','Flush and compaction can delay each other'),('HTTP + Base64','Interoperable, mature parsing, binary-safe clients','JSON/Base64 and connection overhead; no optimized DB wire protocol'),('Single-node scope','Concentrates on local storage correctness','No replicated availability or distributed routing')])+
    p('Constructor injection is applied at replaceable infrastructure/policy boundaries, while small codecs and data types remain ordinary values. Expected public errors use Status/Result. Internal infrastructure uses typed Error exceptions to unwind safely through multiple RAII layers, translated at public/server boundaries. There is no interface for every class. Synchronization favors understandable invariants over lock-free complexity.')+
    refs(('include/aegisdb/status.h','Status'),('src/core/codec.cpp','exceptionStatus'),('src/engine/db_impl.h','DBImpl')))

chapter('future','Future improvements',
    p('The next correctness-validation steps are to execute the supplied TCP suite outside the restricted sandbox, run Linux builds/sanitizers, and test actual filesystem/device failure assumptions. These are validation gaps, not fictional implemented capabilities. Keep the implementation checkpoint current before continuing work in another session.')+
    table(['Future work','Purpose','Compatibility / correctness consideration'],[('Manifest checkpoint rotation','Bound startup/history growth','Durable CURRENT transition and old-manifest retirement'),('Prefix compression / restart points','Reduce repeated key bytes','Versioned block format and snapshot-aware seeks'),('Compression via established library','Reduce file bytes','Checksums, decoder limits and dependency policy'),('Partitioned filters / two-level indexes','Bound resident metadata','No false negatives; safe partition selection'),('Parallel compaction','Increase maintenance throughput','Range conflict tracking and atomic installs'),('Group commit / write combining','Amortize synchronization','Preserve batch ordering and per-request outcome'),('Advanced caching / mmap','Reduce I/O/copy overhead','Immutable lifetime and safe reclamation'),('Column families / binary protocol','Broaden API/workloads','Shared durability model and bounded resource accounting'),('Adaptive scheduling','Control stalls/amplification','Measured policy changes, not guessed performance')])+
    p('None of these enhancements is claimed as implemented. Clustering, consensus, replication, SQL, joins, secondary indexes and authentication remain outside the initial project. The present engine has deliberately small, cohesive boundaries that can be extended without duplicating its storage logic.')+
    refs(('docs/implementation-status.md','Pending'),('include/aegisdb/options.h','Options')))

CSS = '''
:root{--ink:#162333;--muted:#596a7e;--line:#dbe3ec;--accent:#047e8b;--nav:#101c2b;--paper:#fff;--bg:#f3f6fa}*{box-sizing:border-box}html{scroll-behavior:smooth;scroll-padding-top:32px}body{margin:0;background:var(--bg);color:var(--ink);font:16px/1.7 -apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif}a{color:var(--accent)}aside.sidebar{position:fixed;inset:0 auto 0 0;width:270px;background:var(--nav);color:#c7d5e5;display:flex;flex-direction:column;padding:28px 20px;z-index:4}.brand{font-size:24px;letter-spacing:.04em;color:white;font-weight:750}.brand small{display:block;font-size:10px;letter-spacing:.18em;color:#77ccd2;font-weight:500;margin:4px 0 20px}#search{width:100%;background:#223247;border:1px solid #344a60;color:white;padding:10px 12px;border-radius:7px;font:inherit;font-size:13px}nav{overflow:auto;margin-top:18px;flex:1}nav a{display:flex;gap:10px;align-items:baseline;color:#b8c9dc;text-decoration:none;font-size:12px;line-height:1.45;padding:8px 9px;border-left:2px solid transparent;border-radius:3px}nav a span{font-size:10px;opacity:.6;min-width:18px}nav a:hover,nav a.active{color:white;background:#1e3347;border-left-color:#56d5d1}.sidebar footer{font-size:11px;opacity:.65;border-top:1px solid #304257;padding-top:15px;margin-top:15px}main{margin-left:270px;padding:44px 52px;max-width:1500px}.hero{background:linear-gradient(120deg,#172d43,#174a53);color:white;padding:46px 44px;border-radius:16px;margin-bottom:38px}.eyebrow,.label{font-size:11px;font-weight:700;letter-spacing:.14em}.eyebrow{color:#95dedc}.hero h1{font-size:42px;line-height:1.15;margin:16px 0;font-weight:730;letter-spacing:-.035em}.hero p{color:#d0e1eb;max-width:720px;font-size:17px}.chips{display:flex;flex-wrap:wrap;gap:9px;margin-top:26px}.chip{border:1px solid #478186;border-radius:30px;font-size:11px;padding:5px 12px;color:#bbefea}section{background:var(--paper);padding:36px 40px;margin-bottom:24px;border:1px solid var(--line);border-radius:12px;scroll-margin-top:30px}h2{font-size:26px;line-height:1.3;margin:8px 0 24px;letter-spacing:-.02em}.chapter-no{font-size:11px;font-weight:700;letter-spacing:.15em;color:var(--accent)}p{margin:17px 0;max-width:88ch}small{color:var(--muted);font-size:11px}code{font-family:"SFMono-Regular",Consolas,monospace;font-size:.87em;background:#eef3f7;padding:2px 5px;border-radius:4px}pre{background:#142437;border-radius:9px;padding:22px;overflow:auto;color:#cddbeb;font-size:13px;line-height:1.6;margin:23px 0 8px}pre code{background:none;padding:0;color:inherit;font-size:inherit}.keyword{color:#a5c7ff}.string{color:#90e0b7}.number{color:#f4be83}.comment{color:#8298ae}.note{border-left:4px solid #1ca6a9;background:#ecf9f8;padding:17px 20px;border-radius:0 7px 7px 0;margin:23px 0;font-size:14px}.equation{font-family:Georgia,serif;background:#f0f5fa;padding:20px;border-radius:8px;text-align:center;font-size:18px;margin:22px 0;overflow:auto}.table-wrap{overflow:auto;margin:24px 0}table{border-collapse:collapse;width:100%;font-size:13px;line-height:1.5}th{text-align:left;background:#ecf2f8;font-size:11px;text-transform:uppercase;letter-spacing:.06em}th,td{padding:13px 14px;border-bottom:1px solid var(--line);vertical-align:top}td:first-child{font-weight:600}figure{margin:24px 0;background:#f5f8fb;border:1px solid var(--line);border-radius:10px;padding:12px}svg{display:block;width:100%;max-height:680px}svg rect{fill:#e0f0f1;stroke:#64a6aa;stroke-width:1}svg text{fill:#203b50;font-size:17px;font-family:inherit}svg path{fill:none;stroke:#548b98;stroke-width:1.8}svg marker path{fill:#548b98;stroke:none}.lifeline{stroke-dasharray:5 5;opacity:.4}.annotation{font-size:13px}figcaption{font-size:11px;color:var(--muted);text-align:center;padding:8px}.sources{border-top:1px solid var(--line);margin-top:30px;padding-top:17px;display:flex;flex-wrap:wrap;gap:10px}.sources .label{color:var(--muted);width:100%;font-size:9px}.sources a{background:#f3f7fa;border:1px solid #e0e7ee;border-radius:7px;padding:9px 12px;text-decoration:none;max-width:100%;font-size:12px}.sources a span{display:block;color:var(--muted);font:10px/1.8 monospace;overflow-wrap:anywhere}.page-footer{text-align:center;font-size:12px;color:var(--muted);padding:20px}#menu{display:none}a:focus-visible,button:focus-visible,input:focus-visible{outline:3px solid #eea928;outline-offset:3px}@media(max-width:1050px){main{padding:30px 24px}section{padding:28px 25px}.hero{padding:34px 30px}.hero h1{font-size:34px}}@media(max-width:760px){aside.sidebar{transform:translateX(-100%);transition:transform .18s;width:270px}aside.sidebar.open{transform:translateX(0)}main{margin:0;padding:65px 14px 20px}#menu{display:block;position:fixed;top:12px;right:15px;z-index:6;padding:9px 15px;border:0;border-radius:7px;background:#12303e;color:white;box-shadow:0 3px 10px #0002}.hero h1{font-size:30px}section{padding:24px 20px}h2{font-size:23px}pre{padding:17px;font-size:11px}svg text{font-size:19px}}@media print{aside.sidebar,#menu{display:none}main{margin:0;padding:0}.hero{color:black;background:none;padding:15px}.hero p,.eyebrow,.chip{color:black}section{border:0;break-before:auto;padding:20px 0}figure,pre,table{break-inside:avoid}pre{white-space:pre-wrap;background:#eef2f5;color:black}.sources{font-size:10px}a{color:black}}
'''
JS = '''
const navLinks=[...document.querySelectorAll('nav a')];
document.querySelector('#search').addEventListener('input',e=>{const q=e.target.value.toLowerCase();navLinks.forEach(a=>a.hidden=!a.textContent.toLowerCase().includes(q));});
const sidebar=document.querySelector('.sidebar');const menu=document.querySelector('#menu');
menu.addEventListener('click',()=>{const open=sidebar.classList.toggle('open');menu.setAttribute('aria-expanded',String(open));});
navLinks.forEach(a=>a.addEventListener('click',()=>{sidebar.classList.remove('open');menu.setAttribute('aria-expanded','false');}));
document.addEventListener('keydown',e=>{if(e.key==='Escape'){sidebar.classList.remove('open');menu.setAttribute('aria-expanded','false');}});
const observer=new IntersectionObserver(entries=>{entries.filter(e=>e.isIntersecting).forEach(e=>{navLinks.forEach(a=>a.classList.toggle('active',a.hash==='#'+e.target.id));});},{rootMargin:'-10% 0px -70% 0px'});
document.querySelectorAll('section').forEach(s=>observer.observe(s));
'''
CSS+='nav a[hidden]{display:none}'
navigation=''.join(f'<a href="#{slug}"><span>{i:02d}</span>{escape(title)}</a>' for i,(slug,title,_) in enumerate(chapters,1))
content=''.join(f'<section id="{slug}"><div class="chapter-no">CHAPTER {i:02d}</div><h2>{escape(title)}</h2>{body}</section>' for i,(slug,title,body) in enumerate(chapters,1))
html='<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>AegisDB — Engineering Handbook</title><style>'+CSS+'</style></head><body><button id="menu" aria-controls="navigation" aria-expanded="false">Contents</button><aside class="sidebar" id="navigation"><div class="brand">AEGIS<span style="color:#72d5d3">DB</span><small>ENGINEERING HANDBOOK</small></div><label for="search" class="label">FIND A CHAPTER</label><input id="search" placeholder="Search contents…" type="search"><nav aria-label="Table of contents">'+navigation+'</nav><footer>C++20 · Single node · Format v1<br>Offline documentation · 2026-10-11</footer></aside><main><header class="hero"><div class="eyebrow">STORAGE SYSTEMS / IMPLEMENTATION REFERENCE</div><h1>A durable LSM tree,<br>from bytes to behavior.</h1><p>The actual AegisDB architecture, algorithms, file formats and correctness boundaries — connected to the C++ implementation and its tests.</p><div class="chips"><span class="chip">27 technical chapters</span><span class="chip">Embedded + HTTP</span><span class="chip">Atomic batches + MVCC</span><span class="chip">Source-linked, offline</span></div></header>'+content+'<footer class="page-footer">Generated by scripts/build_handbook.py · Source excerpts and relative links verified at generation time.<br>See implementation-status.md for continuation and validation limits.</footer></main><script>'+JS+'</script></body></html>'
parts=[]
last=0
for number,match in enumerate(re.finditer(r'<svg\b.*?</svg>',html,re.S)):
    parts.append(html[last:match.start()])
    svg=match.group()
    for name in ('arrow','seqarrow','skiparrow','statearrow'):
        svg=svg.replace('id="'+name+'"','id="'+name+'-'+str(number)+'"').replace('url(#'+name+')','url(#'+name+'-'+str(number)+')')
    parts.append(svg)
    last=match.end()
parts.append(html[last:])
html=''.join(parts)
(ROOT/'docs/architecture.html').write_text(html)
print(f'Generated {len(chapters)} chapters, {len(html):,} bytes')
