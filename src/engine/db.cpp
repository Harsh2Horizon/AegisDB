#include "engine/db_impl.h"

namespace aegisdb {
std::uint64_t Snapshot::sequence() const { return token_ ? token_->sequence : 0; }
Database::Database(std::unique_ptr<detail::DBImpl> impl) : impl_(std::move(impl)) {}
Database::~Database() = default;
Result<std::unique_ptr<Database>> Database::open(const std::filesystem::path &path,
                                                 Options options) {
    try {
        return std::unique_ptr<Database>(
            new Database(std::make_unique<detail::DBImpl>(path, std::move(options))));
    } catch (...) {
        return detail::exceptionStatus();
    }
}
Status Database::put(std::string_view key, std::string_view value, WriteOptions options) {
    try {
        WriteBatch batch;
        batch.put(std::string(key), std::string(value));
        return impl_->write(batch, options);
    } catch (...) {
        return detail::exceptionStatus();
    }
}
Status Database::remove(std::string_view key, WriteOptions options) {
    try {
        WriteBatch batch;
        batch.remove(std::string(key));
        return impl_->write(batch, options);
    } catch (...) {
        return detail::exceptionStatus();
    }
}
Status Database::write(const WriteBatch &batch, WriteOptions options) {
    return impl_->write(batch, options);
}
Result<std::string> Database::get(std::string_view key, ReadOptions options) const {
    return impl_->get(key, std::move(options));
}
Result<ScanPage> Database::scan(const ScanOptions &options) const { return impl_->scan(options); }
Result<Snapshot> Database::createSnapshot() { return impl_->snapshot(); }
Status Database::flush() { return impl_->flush(); }
Status Database::compact() { return impl_->compact(); }
Statistics Database::statistics() const { return impl_->statistics(); }
Status Database::health() const { return impl_->health(); }
} // namespace aegisdb
