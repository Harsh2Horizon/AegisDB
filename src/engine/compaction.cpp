#include "engine/compaction.h"
#include "engine/merge.h"
#include <algorithm>
#include <cmath>
#include <map>

namespace aegisdb::detail {
bool CompactionPlan::baseLevelFor(std::string_view key) const {
    // Check every unprocessed range, including shallower levels; range false positives retain data.
    for (const auto &level : version->levels)
        for (const auto &t : level) {
            const auto &m = t->metadata();
            if (key >= m.smallest && key <= m.largest &&
                std::find(inputs.begin(), inputs.end(), t) == inputs.end())
                return false;
        }
    return true;
}
std::optional<CompactionPlan> CompactionPicker::pick(std::shared_ptr<const Version> version,
                                                     Sequence oldest,
                                                     std::optional<std::size_t> forced) const {
    std::size_t selected = 0;
    double best = 0;
    if (forced)
        selected = *forced;
    else {
        double capacity = static_cast<double>(options_.levelOneBytes);
        for (std::size_t l = 0; l + 1 < version->levels.size(); ++l) {
            double score = 0;
            if (l == 0)
                score = static_cast<double>(version->levels[0].size()) /
                        static_cast<double>(options_.l0CompactionFiles);
            else {
                std::uint64_t bytes = 0;
                for (const auto &t : version->levels[l])
                    bytes += t->metadata().size;
                score = static_cast<double>(bytes) / capacity;
                capacity *= options_.levelRatio;
            }
            if (score >= 1 && score > best) {
                best = score;
                selected = l;
            }
        }
        if (best == 0)
            return {};
    }
    if (selected >= version->levels.size() || version->levels[selected].empty())
        return {};
    const auto outputLevel = std::min(selected + 1, version->levels.size() - 1);
    CompactionPlan plan{std::move(version), {}, outputLevel, oldest};
    const auto &source = plan.version->levels[selected];
    if (selected == 0 || forced)
        plan.inputs = source;
    else
        plan.inputs.push_back(source.front());
    std::string smallest = plan.inputs.front()->metadata().smallest,
                largest = plan.inputs.front()->metadata().largest;
    for (const auto &t : plan.inputs) {
        smallest = std::min(smallest, t->metadata().smallest);
        largest = std::max(largest, t->metadata().largest);
    }
    if (outputLevel != selected)
        for (const auto &t : plan.version->levels[outputLevel])
            if (t->metadata().largest >= smallest && t->metadata().smallest <= largest)
                plan.inputs.push_back(t);
    return plan;
}
std::vector<TableMeta> CompactionExecutor::run(const CompactionPlan &plan,
                                               const std::function<std::uint64_t()> &allocate) {
    std::vector<std::unique_ptr<RecordIterator>> sources;
    std::map<std::uint32_t, std::vector<std::shared_ptr<SSTableReader>>> disjoint;
    for (const auto &t : plan.inputs) {
        if (t->metadata().level == 0)
            sources.push_back(t->iterator());
        else
            disjoint[t->metadata().level].push_back(t);
        metrics_.compactionRead += t->metadata().size;
    }
    for (auto &[level, tables] : disjoint) {
        (void)level;
        sources.push_back(std::make_unique<TableSequenceIterator>(std::move(tables)));
    }
    MergingIterator merge(std::move(sources));
    std::vector<TableMeta> output;
    std::unique_ptr<SSTableBuilder> builder;
    std::string lastOutput;
    auto finish = [&] {
        if (builder) {
            auto m = builder->finish(static_cast<std::uint32_t>(plan.outputLevel));
            metrics_.compactionWritten += m.size;
            output.push_back(std::move(m));
            builder.reset();
        }
    };
    while (merge.valid()) {
        const std::string key = merge.record().key;
        bool retainedBase = false;
        std::optional<Sequence> previous;
        while (merge.valid() && merge.record().key == key) {
            const auto &r = merge.record();
            const bool duplicate = previous && r.sequence == *previous;
            previous = r.sequence;
            bool keep = !duplicate && (r.sequence > plan.oldestSnapshot || !retainedBase);
            if (!duplicate && r.sequence <= plan.oldestSnapshot) {
                retainedBase = true;
                if (r.kind == Kind::deletion && plan.baseLevelFor(key))
                    keep = false;
            }
            if (keep) {
                if (builder && lastOutput != key && builder->estimatedSize() >= options_.tableBytes)
                    finish();
                if (!builder)
                    builder = std::make_unique<SSTableBuilder>(fs_, path_, allocate(),
                                                               options_.blockBytes,
                                                               options_.bloomFalsePositiveRate);
                builder->add(r);
                lastOutput = key;
            }
            merge.next();
        }
    }
    finish();
    return output;
}
} // namespace aegisdb::detail
