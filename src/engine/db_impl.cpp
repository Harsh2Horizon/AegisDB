#include "engine/db_impl.h"
#include "engine/merge.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <set>

namespace aegisdb::detail {
DBImpl::DBImpl(std::filesystem::path path, Options options)
    : path_(std::move(path)), options_(std::move(options)),
      fs_(options_.filesystem ? options_.filesystem : makePosixFileSystem()),
      cache_(options_.blockCacheBytes, metrics_) {
    validateOptions();
    fs_->createDirectories(path_);
    directoryLock_ = fs_->lock(path_);
    manifest_ = std::make_unique<Manifest>(*fs_, path_, options_.levelCount);
    recover();
    worker_ = std::jthread([this](std::stop_token stop) { background(stop); });
}
void DBImpl::validateOptions() const {
    require(options_.memtableBytes > 0 && options_.walSegmentBytes > 16 &&
                options_.walSegmentBytes <= maxFrameBytes && options_.maxImmutableMemtables > 0 &&
                options_.maxImmutableMemtables <= 64,
            Code::invalid_argument, "invalid write buffer options");
    require(options_.blockBytes > 0 && options_.blockBytes <= maxFrameBytes / 2 &&
                options_.tableBytes > 0 && options_.tableBytes <= 256 * 1024 * 1024,
            Code::invalid_argument, "invalid table options");
    require(options_.maxKeyBytes <= 1024 * 1024 && options_.maxValueBytes <= 16 * 1024 * 1024 &&
                options_.maxBatchBytes > 13 && options_.maxBatchBytes <= maxFrameBytes &&
                options_.maxBatchOperations > 0 && options_.maxBatchOperations <= 1000000,
            Code::invalid_argument, "invalid encoding limits");
    require(options_.levelCount >= 2 && options_.levelCount <= 32 && options_.levelOneBytes > 0 &&
                std::isfinite(options_.levelRatio) && options_.levelRatio > 1 &&
                options_.l0CompactionFiles > 0 &&
                options_.l0StopFiles >= options_.l0CompactionFiles,
            Code::invalid_argument, "invalid level options");
    require(std::isfinite(options_.bloomFalsePositiveRate) && options_.bloomFalsePositiveRate > 0 &&
                options_.bloomFalsePositiveRate < 1 && options_.skipListHeight > 0 &&
                options_.skipListHeight <= SkipList::heightLimit &&
                options_.skipListPromotion > 0 && options_.skipListPromotion < 1,
            Code::invalid_argument, "invalid probabilistic structure options");
    require(options_.maxScanResults > 0 && options_.maxScanBytes > 0 && options_.maxSnapshots > 0 &&
                options_.writeTimeout.count() > 0 &&
                options_.writeTimeout <= std::chrono::minutes(5),
            Code::invalid_argument, "invalid resource limits");
}
std::shared_ptr<SSTableReader> DBImpl::openTable(const TableMeta &meta) {
    const auto file = path_ / fileName(meta.number, ".sst");
    require(fs_->exists(file), Code::corruption, "required SSTable missing");
    auto table = std::make_shared<SSTableReader>(*fs_, file, meta, cache_, metrics_);
    if (options_.verifyTablesOnOpen)
        table->verify();
    return table;
}
void DBImpl::recover() {
    const auto &installedLogs = manifest_->state().logs;
    std::set<std::string> registeredNames;
    for (auto id : installedLogs)
        registeredNames.insert(fileName(id, ".wal"));
    const auto floorName = fileName(manifest_->state().logNumber, ".wal");
    for (const auto &entry : fs_->list(path_)) {
        const auto name = entry.filename().string();
        if (!numberedFileName(name, ".wal") || name < floorName)
            continue;
        const bool registered = registeredNames.contains(name);
        if (!registered) {
            auto file = fs_->open(entry, OpenMode::read_only);
            // No record can enter a successor WAL before its durable registration. Extra data
            // above the retirement floor therefore signals lost/corrupt manifest history.
            require(file->size() <= 16, Code::corruption, "unregistered WAL contains records");
        }
    }
    auto version = std::make_shared<Version>();
    version->levels.resize(options_.levelCount);
    committed_ = manifest_->state().lastSequence;
    for (std::size_t l = 0; l < options_.levelCount; ++l)
        for (const auto &meta : manifest_->state().levels[l])
            version->levels[l].push_back(openTable(meta));
    version_ = std::move(version);
    Sequence replayed = 0;
    const auto logs = manifest_->state().logs;
    for (auto number : logs) {
        const auto filePath = path_ / fileName(number, ".wal");
        require(fs_->exists(filePath), Code::corruption, "required WAL missing");
        auto file = fs_->open(filePath, OpenMode::append_existing);
        require(file->size() <= 256 * 1024 * 1024, Code::resource_exhausted,
                "recovery WAL exceeds safety bound");
        auto mem =
            std::make_shared<Memtable>(number, options_.skipListHeight, options_.skipListPromotion);
        const auto valid = FramedLog::replay(
            *file, walMagic, number == *logs.rbegin(), [&](std::string_view payload) {
                auto batch = decodeBatch(payload);
                require(batch.front().sequence > replayed, Code::corruption,
                        "WAL sequence regression");
                replayed = batch.back().sequence;
                committed_ = std::max(committed_, replayed);
                mem->publish(mem->prepare(std::move(batch)));
            });
        if (valid != file->size()) {
            file->truncate(valid);
            file->sync();
            ++metrics_.walSyncs;
        }
        immutable_.push_back(std::move(mem));
    }
    const auto number = manifest_->allocate();
    auto next = FramedLog::create(*fs_, path_ / fileName(number, ".wal"), walMagic);
    metrics_.walBytes += 16;
    ++metrics_.walSyncs;
    auto mem =
        std::make_shared<Memtable>(number, options_.skipListHeight, options_.skipListPromotion);
    VersionEdit edit;
    edit.addLogs = {number};
    edit.logNumber = logs.empty() ? number : *logs.begin();
    edit.lastSequence = committed_;
    manifest_->commit(edit);
    active_ = std::move(mem);
    wal_ = std::move(next);
    // Recovery drains old segments before serving requests, bounding the runtime immutable queue.
    while (!immutable_.empty())
        flushOne();
    std::set<std::string> live;
    for (const auto &level : manifest_->state().levels)
        for (const auto &m : level)
            live.insert(fileName(m.number, ".sst"));
    for (auto n : manifest_->state().logs)
        live.insert(fileName(n, ".wal"));
    for (const auto &p : fs_->list(path_)) {
        const auto name = p.filename().string();
        if ((numberedFileName(name, ".sst") || numberedFileName(name, ".wal") ||
             numberedFileName(name, ".sst.tmp") || name == "CURRENT.tmp") &&
            !live.contains(name))
            fs_->remove(p);
    }
    fs_->syncDirectory(path_);
}
DBImpl::~DBImpl() {
    {
        std::lock_guard lock(stateMutex_);
        stopping_ = true;
    }
    worker_.request_stop();
    stateCV_.notify_all();
    if (worker_.joinable())
        worker_.join();
    try {
        if (wal_) {
            wal_->sync();
            ++metrics_.walSyncs;
        }
    } catch (...) {
        std::cerr << "AegisDB: WAL synchronization failed during close\n";
    }
}
Status DBImpl::health() const {
    std::lock_guard lock(stateMutex_);
    if (!error_)
        return error_;
    if (stopping_)
        return {Code::shutting_down, "database shutting down"};
    return {};
}
void DBImpl::fail(Status error) const {
    std::lock_guard lock(stateMutex_);
    if (error_.ok())
        error_ = std::move(error);
    stateCV_.notify_all();
}
std::uint64_t DBImpl::allocate() {
    std::lock_guard lock(manifestMutex_);
    return manifest_->allocate();
}
void DBImpl::waitForRoom(std::unique_lock<std::mutex> &lock) {
    const auto deadline = std::chrono::steady_clock::now() + options_.writeTimeout;
    const auto room = [this] {
        return stopping_ || !error_.ok() ||
               (immutable_.size() < options_.maxImmutableMemtables &&
                version_->levels[0].size() < options_.l0StopFiles);
    };
    if (!stateCV_.wait_until(lock, deadline, room))
        throw Error(Code::busy, "storage backpressure timeout");
    require(!stopping_, Code::shutting_down, "database shutting down");
    if (!error_)
        throw Error(error_.code, error_.message);
}
void DBImpl::rotate() {
    std::shared_ptr<Memtable> old;
    std::deque<std::shared_ptr<Memtable>> queue;
    {
        std::unique_lock lock(stateMutex_);
        if (active_->empty())
            return;
        waitForRoom(lock);
    }
    // Serialize queue capture with flush installation; otherwise a completed flush could be
    // restored.
    std::lock_guard manifestLock(manifestMutex_);
    {
        std::lock_guard lock(stateMutex_);
        old = active_;
        queue = immutable_;
        queue.push_back(old);
    }
    const auto number = manifest_->allocate();
    auto nextMem =
        std::make_shared<Memtable>(number, options_.skipListHeight, options_.skipListPromotion);
    auto nextLog = FramedLog::create(*fs_, path_ / fileName(number, ".wal"), walMagic);
    metrics_.walBytes += 16;
    ++metrics_.walSyncs;
    wal_->sync();
    ++metrics_.walSyncs;
    VersionEdit edit;
    edit.addLogs = {number};
    edit.logNumber = *manifest_->state().logs.begin();
    {
        std::lock_guard lock(stateMutex_);
        edit.lastSequence = committed_;
    }
    manifest_->commit(edit);
    {
        std::lock_guard lock(stateMutex_);
        active_ = std::move(nextMem);
        immutable_ = std::move(queue);
        wal_ = std::move(nextLog);
        workRequested_ = true;
    }
    stateCV_.notify_all();
}
Status DBImpl::write(const WriteBatch &batch, WriteOptions options) {
    bool durableAttempt = false;
    bool published = false;
    try {
        const auto &operations = batch.operations();
        require(!operations.empty() && operations.size() <= options_.maxBatchOperations,
                Code::invalid_argument, "invalid batch size");
        std::size_t bytes = 13;
        for (const auto &op : operations) {
            require(op.key.size() <= options_.maxKeyBytes &&
                        op.value.size() <= options_.maxValueBytes,
                    Code::invalid_argument, "key or value exceeds configured size");
            require(op.type == Mutation::Type::put || op.type == Mutation::Type::remove,
                    Code::invalid_argument, "invalid mutation type");
            require(op.type != Mutation::Type::remove || op.value.empty(), Code::invalid_argument,
                    "delete has a value");
            const auto n = 17 + op.key.size() + op.value.size();
            require(n <= options_.maxBatchBytes && bytes <= options_.maxBatchBytes - n,
                    Code::invalid_argument, "batch exceeds byte limit");
            bytes += n;
        }
        std::unique_lock writer(writeMutex_, std::defer_lock);
        require(writer.try_lock_for(options_.writeTimeout), Code::busy, "writer admission timeout");
        {
            std::unique_lock lock(stateMutex_);
            waitForRoom(lock);
        }
        if (active_->memoryUsage() >= options_.memtableBytes ||
            wal_->size() >= options_.walSegmentBytes)
            rotate();
        Sequence first;
        {
            std::lock_guard lock(stateMutex_);
            require(operations.size() <= maxSequence - committed_, Code::resource_exhausted,
                    "sequence exhausted");
            first = committed_ + 1;
        }
        std::vector<Record> records;
        records.reserve(operations.size());
        for (std::size_t i = 0; i < operations.size(); ++i) {
            const auto &op = operations[i];
            records.push_back({op.key, first + i,
                               op.type == Mutation::Type::put ? Kind::value : Kind::deletion,
                               op.value});
        }
        const auto encoded = encodeBatch(records);
        auto prepared = active_->prepare(std::move(records));
        durableAttempt = true;
        wal_->append(encoded, false);
        metrics_.walBytes += encoded.size() + 12;
        if (options.sync) {
            wal_->sync();
            ++metrics_.walSyncs;
        }
        {
            std::lock_guard lock(stateMutex_);
            if (!error_)
                throw Error(error_.code, error_.message);
            active_->publish(std::move(prepared));
            committed_ = first + operations.size() - 1;
            published = true;
        }
        for (const auto &op : operations)
            if (op.type == Mutation::Type::put)
                ++metrics_.puts;
            else
                ++metrics_.deletes;
        // If the queue is full, retain at most one bounded oversize active table until the next
        // write.
        bool room;
        {
            std::lock_guard lock(stateMutex_);
            room = immutable_.size() < options_.maxImmutableMemtables &&
                   version_->levels[0].size() < options_.l0StopFiles;
        }
        if (room && (active_->memoryUsage() >= options_.memtableBytes ||
                     wal_->size() >= options_.walSegmentBytes))
            rotate();
        return {};
    } catch (...) {
        auto error = exceptionStatus();
        // Backpressure after publication only postpones rotation; the committed batch succeeded.
        if (published && error.code == Code::busy)
            return {};
        if (durableAttempt || (error.code == Code::io_error || error.code == Code::corruption))
            fail(error);
        return error;
    }
}
DBImpl::ReadView DBImpl::readView(const Snapshot &snapshot) const {
    std::lock_guard lock(stateMutex_);
    require(!stopping_, Code::shutting_down, "database shutting down");
    Sequence sequence = committed_;
    if (snapshot.token_) {
        require(snapshot.token_->owner == snapshots_, Code::invalid_argument,
                "snapshot belongs to another database instance");
        sequence = snapshot.token_->sequence;
    }
    return {sequence, active_, immutable_, version_};
}
Result<std::string> DBImpl::get(std::string_view key, ReadOptions options) const {
    ++metrics_.gets;
    try {
        require(key.size() <= options_.maxKeyBytes, Code::invalid_argument, "key too large");
        const auto view = readView(options.snapshot);
        std::optional<Record> winner;
        const auto consider = [&](std::optional<Record> record) {
            if (record && (!winner || record->sequence > winner->sequence))
                winner = std::move(record);
        };
        consider(view.active->get(key, view.sequence));
        for (const auto &mem : view.immutable)
            consider(mem->get(key, view.sequence));
        for (const auto &table : view.version->levels[0])
            consider(table->get(key, view.sequence));
        for (std::size_t l = 1; l < view.version->levels.size(); ++l) {
            const auto &level = view.version->levels[l];
            const auto it = std::lower_bound(level.begin(), level.end(), key,
                                             [](const auto &table, std::string_view target) {
                                                 return table->metadata().largest < target;
                                             });
            if (it != level.end())
                consider((*it)->get(key, view.sequence));
        }
        if (!winner || winner->kind == Kind::deletion) {
            ++metrics_.misses;
            return Status{Code::not_found, "key not found"};
        }
        ++metrics_.hits;
        return std::move(winner->value);
    } catch (...) {
        auto error = exceptionStatus();
        if (error.code == Code::io_error || error.code == Code::corruption)
            fail(error);
        return error;
    }
}
Result<ScanPage> DBImpl::scan(const ScanOptions &options) const {
    ++metrics_.scans;
    try {
        require(options.limit > 0 && options.limit <= options_.maxScanResults &&
                    options.start.size() <= options_.maxKeyBytes &&
                    (!options.end || options.end->size() <= options_.maxKeyBytes) &&
                    (!options.after || options.after->size() <= options_.maxKeyBytes),
                Code::invalid_argument, "invalid scan bounds");
        require(!options.end || options.start <= *options.end, Code::invalid_argument,
                "reversed scan range");
        const auto view = readView(options.snapshot);
        const auto start = options.after ? std::max(options.start, *options.after) : options.start;
        std::vector<std::unique_ptr<RecordIterator>> sources;
        sources.push_back(view.active->iterator(start));
        for (const auto &mem : view.immutable)
            sources.push_back(mem->iterator(start));
        for (std::size_t l = 0; l < view.version->levels.size(); ++l) {
            const auto &level = view.version->levels[l];
            if (l == 0) {
                for (const auto &table : level) {
                    const auto &m = table->metadata();
                    if (m.largest >= start && (!options.end || m.smallest < *options.end))
                        sources.push_back(table->iterator(start));
                }
            } else {
                const auto first = std::lower_bound(level.begin(), level.end(), start,
                                                    [](const auto &table, std::string_view key) {
                                                        return table->metadata().largest < key;
                                                    });
                const auto last =
                    options.end ? std::lower_bound(first, level.end(), *options.end,
                                                   [](const auto &table, std::string_view key) {
                                                       return table->metadata().smallest < key;
                                                   })
                                : level.end();
                if (first != last)
                    sources.push_back(std::make_unique<TableSequenceIterator>(
                        std::vector<std::shared_ptr<SSTableReader>>(first, last), start));
            }
        }
        MergingIterator merge(std::move(sources));
        ScanPage page;
        std::size_t bytes = 0;
        while (merge.valid()) {
            const auto key = merge.record().key;
            if (options.end && key >= *options.end)
                break;
            std::optional<Record> winner;
            while (merge.valid() && merge.record().key == key) {
                const auto &r = merge.record();
                if (!winner && r.sequence <= view.sequence)
                    winner = r;
                merge.next();
            }
            if ((options.after && key <= *options.after) || !winner ||
                winner->kind == Kind::deletion)
                continue;
            const auto charge = key.size() + winner->value.size();
            if (charge > options_.maxScanBytes - bytes) {
                require(!page.entries.empty(), Code::resource_exhausted,
                        "scan record exceeds byte limit");
                page.nextAfter = page.entries.back().key;
                return page;
            }
            bytes += charge;
            page.entries.push_back({key, std::move(winner->value)});
            if (page.entries.size() == options.limit) {
                if (merge.valid() && (!options.end || merge.record().key < *options.end))
                    page.nextAfter = key;
                return page;
            }
        }
        return page;
    } catch (...) {
        auto error = exceptionStatus();
        if (error.code == Code::io_error || error.code == Code::corruption)
            fail(error);
        return error;
    }
}
Result<Snapshot> DBImpl::snapshot() {
    try {
        std::lock_guard stateLock(stateMutex_);
        require(!stopping_, Code::shutting_down, "database shutting down");
        std::lock_guard snapshotLock(snapshots_->mutex);
        require(snapshots_->handles < options_.maxSnapshots, Code::resource_exhausted,
                "snapshot limit reached");
        auto [it, inserted] = snapshots_->active.try_emplace(committed_, 0);
        (void)inserted;
        ++it->second;
        ++snapshots_->handles;
        try {
            return Snapshot(std::make_shared<SnapshotToken>(snapshots_, committed_));
        } catch (...) {
            if (--it->second == 0)
                snapshots_->active.erase(it);
            --snapshots_->handles;
            throw;
        }
    } catch (...) {
        return exceptionStatus();
    }
}
void DBImpl::flushOne() {
    std::shared_ptr<Memtable> source;
    {
        std::lock_guard lock(stateMutex_);
        if (immutable_.empty())
            return;
        source = immutable_.front();
    }
    std::vector<std::shared_ptr<SSTableReader>> additions;
    VersionEdit edit;
    if (!source->empty()) {
        SSTableBuilder builder(*fs_, path_, allocate(), options_.blockBytes,
                               options_.bloomFalsePositiveRate);
        auto it = source->iterator();
        while (it->valid()) {
            builder.add(it->record());
            it->next();
        }
        auto meta = builder.finish(0);
        metrics_.flushBytes += meta.size;
        additions.push_back(openTable(meta));
        edit.additions.push_back(std::move(meta));
    }
    std::lock_guard manifestLock(manifestMutex_);
    edit.deleteLogs = {source->walNumber};
    const auto &logs = manifest_->state().logs;
    const auto nextLog = std::next(logs.find(source->walNumber));
    require(nextLog != logs.end(), Code::internal_error, "flush has no successor WAL");
    edit.logNumber = *nextLog;
    std::shared_ptr<Version> next;
    {
        std::lock_guard lock(stateMutex_);
        edit.lastSequence = committed_;
        next = applyVersion(*version_, edit, additions);
    }
    manifest_->commit(edit);
    {
        std::lock_guard lock(stateMutex_);
        version_ = std::move(next);
        require(!immutable_.empty() && immutable_.front() == source, Code::internal_error,
                "immutable queue changed");
        immutable_.pop_front();
        workRequested_ = true;
    }
    ++metrics_.flushes;
    stateCV_.notify_all();
    fs_->remove(path_ / fileName(source->walNumber, ".wal"));
    fs_->syncDirectory(path_);
}
bool DBImpl::compactOne(std::optional<std::size_t> forced) {
    std::optional<CompactionPlan> plan;
    {
        std::lock_guard lock(stateMutex_);
        plan = CompactionPicker(options_).pick(version_, snapshots_->oldest(committed_), forced);
    }
    if (!plan)
        return false;
    CompactionExecutor executor(*fs_, path_, options_, metrics_);
    const auto metas = executor.run(*plan, [this] { return allocate(); });
    VersionEdit edit;
    edit.additions = metas;
    std::vector<std::shared_ptr<SSTableReader>> additions;
    additions.reserve(metas.size());
    for (const auto &meta : metas)
        additions.push_back(openTable(meta));
    for (const auto &input : plan->inputs)
        edit.deletions.push_back(input->metadata().number);
    obsolete_.reserve(obsolete_.size() + plan->inputs.size());
    std::vector<std::filesystem::path> paths;
    paths.reserve(plan->inputs.size());
    for (const auto &input : plan->inputs)
        paths.push_back(path_ / fileName(input->metadata().number, ".sst"));
    {
        std::lock_guard manifestLock(manifestMutex_);
        std::shared_ptr<Version> next;
        {
            std::lock_guard lock(stateMutex_);
            edit.lastSequence = committed_;
            next = applyVersion(*version_, edit, additions);
        }
        manifest_->commit(edit);
        {
            std::lock_guard lock(stateMutex_);
            version_ = std::move(next);
            workRequested_ = true;
        }
    }
    for (std::size_t i = 0; i < plan->inputs.size(); ++i)
        obsolete_.push_back({plan->inputs[i], std::move(paths[i])});
    plan.reset();
    ++metrics_.compactions;
    stateCV_.notify_all();
    reclaim();
    return true;
}
void DBImpl::reclaim() {
    bool removed = false;
    std::erase_if(obsolete_, [&](const auto &entry) {
        if (!entry.first.expired())
            return false;
        fs_->remove(entry.second);
        removed = true;
        return true;
    });
    if (removed)
        fs_->syncDirectory(path_);
}
void DBImpl::background(std::stop_token stop) {
    try {
        while (!stop.stop_requested()) {
            {
                std::unique_lock lock(stateMutex_);
                stateCV_.wait(lock, [&] {
                    return stopping_ || !error_.ok() || !immutable_.empty() || workRequested_;
                });
                if (stopping_ || !error_.ok())
                    return;
                workRequested_ = false;
            }
            std::lock_guard maintenanceLock(maintenanceMutex_);
            bool flush;
            {
                std::lock_guard lock(stateMutex_);
                flush = !immutable_.empty();
            }
            if (flush)
                flushOne();
            else if (options_.automaticCompaction)
                (void)compactOne();
            reclaim();
        }
    } catch (...) {
        ++metrics_.backgroundErrors;
        fail(exceptionStatus());
        std::cerr << "AegisDB: background maintenance failed; writes disabled\n";
    }
}
Status DBImpl::flush() {
    bool installation = false;
    try {
        std::unique_lock writer(writeMutex_, std::defer_lock);
        require(writer.try_lock_for(options_.writeTimeout), Code::busy, "writer admission timeout");
        const auto status = health();
        if (!status)
            throw Error(status.code, status.message);
        installation = true;
        rotate();
        std::uint64_t target = 0;
        {
            std::lock_guard lock(stateMutex_);
            if (!immutable_.empty())
                target = immutable_.back()->walNumber;
        }
        writer.unlock();
        stateCV_.notify_all();
        std::unique_lock lock(stateMutex_);
        stateCV_.wait(lock, [&] {
            return stopping_ || !error_.ok() || immutable_.empty() ||
                   immutable_.front()->walNumber > target;
        });
        if (!error_)
            throw Error(error_.code, error_.message);
        require(!stopping_, Code::shutting_down, "database shutting down");
        return {};
    } catch (...) {
        auto error = exceptionStatus();
        if (installation && error.code != Code::busy)
            fail(error);
        return error;
    }
}
Status DBImpl::compact() {
    // Manual mode still obeys the L0 bound. Free existing L0 capacity before rotating active data.
    if (!options_.automaticCompaction) {
        try {
            std::lock_guard lock(maintenanceMutex_);
            const auto status = health();
            if (!status)
                return status;
            (void)compactOne(0);
        } catch (...) {
            auto error = exceptionStatus();
            fail(error);
            return error;
        }
    }
    auto status = flush();
    if (!status)
        return status;
    try {
        std::lock_guard maintenanceLock(maintenanceMutex_);
        status = health();
        if (!status)
            return status;
        bool moved = false;
        for (std::size_t level = 0; level + 1 < options_.levelCount; ++level)
            moved = compactOne(level) || moved;
        if (!moved)
            (void)compactOne(options_.levelCount - 1);
        return {};
    } catch (...) {
        auto error = exceptionStatus();
        fail(error);
        return error;
    }
}
Statistics DBImpl::statistics() const {
    Statistics s;
    s.puts = metrics_.puts.load();
    s.gets = metrics_.gets.load();
    s.deletes = metrics_.deletes.load();
    s.scans = metrics_.scans.load();
    s.readHits = metrics_.hits.load();
    s.readMisses = metrics_.misses.load();
    s.walBytes = metrics_.walBytes.load();
    s.walSyncs = metrics_.walSyncs.load();
    s.bloomChecks = metrics_.bloomChecks.load();
    s.bloomNegatives = metrics_.bloomNegatives.load();
    s.cacheHits = metrics_.cacheHits.load();
    s.cacheMisses = metrics_.cacheMisses.load();
    s.cacheBytes = cache_.usage();
    s.flushes = metrics_.flushes.load();
    s.flushBytesWritten = metrics_.flushBytes.load();
    s.compactions = metrics_.compactions.load();
    s.compactionBytesRead = metrics_.compactionRead.load();
    s.compactionBytesWritten = metrics_.compactionWritten.load();
    s.backgroundErrors = metrics_.backgroundErrors.load();
    std::lock_guard lock(stateMutex_);
    s.lastSequence = committed_;
    s.memtableBytes = active_->memoryUsage();
    s.immutableMemtables = immutable_.size();
    for (const auto &m : immutable_)
        s.memtableBytes += m->memoryUsage();
    for (const auto &level : version_->levels) {
        LevelStats ls;
        ls.tables = level.size();
        for (const auto &table : level)
            ls.bytes += table->metadata().size;
        s.levels.push_back(ls);
    }
    return s;
}
} // namespace aegisdb::detail
