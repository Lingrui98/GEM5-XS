#include "cpu/o3/entangling_prefetcher.hh"

#include <algorithm>
#include <array>
#include <cassert>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace gem5
{

namespace o3
{

namespace
{

constexpr std::array<std::uint32_t, 7> EntangledFormats{
    58, 28, 18, 13, 10, 8, 6};
constexpr std::uint32_t NumFormats = 6;
constexpr std::uint32_t EntangledSets = 256;
constexpr std::uint32_t EntangledWays = 16;
constexpr std::uint32_t EntangledIndexBits = 8;
constexpr std::uint32_t EntangledTagBits = 10;
constexpr std::uint64_t EntangledTagMask =
    (std::uint64_t{1} << EntangledTagBits) - 1;
constexpr std::uint32_t MaxEntangledPerLine = NumFormats;
constexpr std::uint32_t ConfidenceThreshold = 1;
constexpr std::uint32_t ConfidenceMaximum = 3;
constexpr std::uint32_t MaxHistoryEntries = 32;
constexpr std::uint32_t HistoryTagBits = 58;
constexpr std::uint64_t HistoryTagMask =
    (std::uint64_t{1} << HistoryTagBits) - 1;
constexpr std::uint32_t BasicBlockSizeBits = 6;
constexpr std::uint32_t BasicBlockSizeMaximum =
    (1U << BasicBlockSizeBits) - 1;
constexpr std::uint32_t TimeDiffBits = 20;
constexpr std::uint64_t TimeDiffOverflow = std::uint64_t{1} << TimeDiffBits;
constexpr std::uint64_t TimeDiffMask = TimeDiffOverflow - 1;
constexpr std::uint32_t TimeBits = 12;
constexpr std::uint64_t TimeOverflow = std::uint64_t{1} << TimeBits;
constexpr std::uint64_t TimeMask = TimeOverflow - 1;

std::uint64_t
hashLine(std::uint64_t line)
{
    return line ^ (line >> 2) ^ (line >> 5);
}

std::uint32_t
formatEntangled(std::uint64_t source, std::uint64_t destination)
{
    for (std::uint32_t i = NumFormats; i != 0; --i) {
        const auto bits = EntangledFormats[i - 1];
        if ((source >> bits) == (destination >> bits)) {
            return i;
        }
    }
    assert(false && "entangled addresses exceed the official 58-bit range");
    return 0;
}

std::uint64_t
compressEntangled(std::uint64_t destination, std::uint32_t format)
{
    assert(format > 0 && format <= NumFormats);
    const auto bits = EntangledFormats[format - 1];
    return destination & ((std::uint64_t{1} << bits) - 1);
}

std::uint64_t
extendEntangled(
    std::uint64_t source, std::uint64_t destination,
    std::uint32_t format)
{
    assert(format > 0 && format <= NumFormats);
    const auto bits = EntangledFormats[format - 1];
    return ((source >> bits) << bits) |
           (destination & ((std::uint64_t{1} << bits) - 1));
}

std::uint64_t
latency(std::uint64_t cycle, std::uint64_t previous)
{
    const auto masked_cycle = cycle & TimeMask;
    const auto masked_previous = previous & TimeMask;
    if (masked_previous > masked_cycle) {
        return masked_cycle + TimeOverflow - masked_previous;
    }
    return masked_cycle - masked_previous;
}

class StateDigest
{
  private:
    static constexpr std::uint64_t Offset = 1469598103934665603ULL;
    static constexpr std::uint64_t Prime = 1099511628211ULL;
    std::uint64_t value = Offset;

  public:
    void
    add(std::uint64_t item)
    {
        for (unsigned i = 0; i < sizeof(item); ++i) {
            value ^= static_cast<std::uint8_t>(item >> (i * 8));
            value *= Prime;
        }
    }

    std::uint64_t get() const { return value; }
};

} // anonymous namespace

struct EntanglingPrefetcher::Impl
{
    struct HistoryEntry
    {
        std::uint64_t tag = 0;
        std::uint64_t timeDiff = 0;
        std::uint32_t basicBlockSize = 0;
        std::uint64_t instructionId = 0;
        std::uint64_t entangledSource = 0;
        std::uint32_t format = 0;
        std::uint32_t numDestinations = 0;
        bool wrongPath = false;
    };

    struct EntangledEntry
    {
        std::uint64_t tag = 0;
        std::uint32_t format = 0;
        std::array<std::uint64_t, MaxEntangledPerLine> destinations{};
        std::array<std::uint32_t, MaxEntangledPerLine> confidence{};
        std::uint32_t basicBlockSize = 0;
    };

    struct TimingRequest
    {
        std::uint32_t sourceSet = 0;
        std::uint32_t sourceWay = NoSourceWay;
        std::uint64_t timestamp = 0;
        bool accessed = false;
        std::uint32_t historyPosition = MaxHistoryEntries;
        bool acceptedPrefetch = false;
    };

    struct Residency
    {
        std::uint64_t virtualLine = 0;
        std::uint32_t sourceSet = 0;
        std::uint32_t sourceWay = NoSourceWay;
        bool accessed = false;
    };

    AlgorithmVersion algorithmVersion;
    std::uint64_t currentCycle;
    std::uint64_t lastBasicBlock = 0;
    std::uint32_t consecutiveCount = 0;
    std::uint32_t basicBlockMergeDiff = 0;
    bool containsWrongPath = false;

    std::array<HistoryEntry, MaxHistoryEntries> history{};
    std::uint32_t historyHead = 0;
    std::uint64_t historyHeadTime;

    std::array<EntangledEntry, EntangledSets * EntangledWays> entangled{};
    std::array<std::uint32_t, EntangledSets> fifo{};

    std::unordered_map<std::uint64_t, TimingRequest> timingRequests;
    std::unordered_map<std::uint64_t, Residency> residentByPhysical;
    std::unordered_map<
        std::uint64_t, std::unordered_set<std::uint64_t>>
        residentPhysicalByVirtual;

    Stats statistics;

    Impl(AlgorithmVersion version, std::uint64_t initial_cycle)
        : algorithmVersion(version), currentCycle(initial_cycle),
          historyHeadTime(initial_cycle)
    {
        const auto initial_format = isTc2024() ? NumFormats : 1;
        for (auto &entry : entangled) {
            entry.format = initial_format;
        }
    }

    bool isTc2024() const
    {
        return algorithmVersion == AlgorithmVersion::Tc2024;
    }

    std::uint32_t historyEntries() const
    {
        return isTc2024() ? 32 : 16;
    }

    std::uint32_t historyMask() const
    {
        return historyEntries() - 1;
    }

    std::uint32_t historySearches() const
    {
        return isTc2024() ? 24 : 16;
    }

    std::uint32_t mergeEntries() const
    {
        return isTc2024() ? 3 : 6;
    }

    void setCycle(std::uint64_t cycle)
    {
        assert(cycle >= currentCycle);
        currentCycle = cycle;
    }

    EntangledEntry &entry(std::uint32_t set, std::uint32_t way)
    {
        return entangled[set * EntangledWays + way];
    }

    const EntangledEntry &entry(
        std::uint32_t set, std::uint32_t way) const
    {
        return entangled[set * EntangledWays + way];
    }

    std::uint32_t findEntangledWay(std::uint64_t line) const
    {
        const auto hashed = hashLine(line);
        const auto tag = (hashed >> EntangledIndexBits) & EntangledTagMask;
        const auto set = static_cast<std::uint32_t>(hashed % EntangledSets);
        for (std::uint32_t way = 0; way < EntangledWays; ++way) {
            if (entry(set, way).tag == tag) {
                return way;
            }
        }
        return EntangledWays;
    }

    void relocateVictim(std::uint32_t set)
    {
        const auto victim_way = fifo[set];
        const auto &victim = entry(set, victim_way);
        bool victim_destinations_free = true;
        for (const auto confidence : victim.confidence) {
            if (confidence >= ConfidenceThreshold) {
                victim_destinations_free = false;
                break;
            }
        }
        if (victim_destinations_free && victim.basicBlockSize == 0) {
            return;
        }

        auto free_way = victim_way;
        bool free_with_size = false;
        for (auto way = (victim_way + 1) % EntangledWays;
             way != victim_way; way = (way + 1) % EntangledWays) {
            bool destinations_free = true;
            for (const auto confidence : entry(set, way).confidence) {
                if (confidence >= ConfidenceThreshold) {
                    destinations_free = false;
                    break;
                }
            }
            if (!destinations_free) {
                continue;
            }
            if (free_way == victim_way) {
                free_way = way;
                free_with_size = entry(set, way).basicBlockSize != 0;
            } else if (free_with_size &&
                       entry(set, way).basicBlockSize == 0) {
                free_way = way;
                free_with_size = false;
                break;
            }
        }

        if (free_way != victim_way &&
            (!free_with_size ||
             (free_with_size && !victim_destinations_free))) {
            entry(set, free_way) = victim;
        }
    }

    std::pair<std::uint32_t, std::uint32_t>
    allocateEntangledEntry(std::uint64_t line)
    {
        const auto hashed = hashLine(line);
        const auto set = static_cast<std::uint32_t>(hashed % EntangledSets);
        auto way = findEntangledWay(line);
        if (way < EntangledWays) {
            return {set, way};
        }

        relocateVictim(set);
        way = fifo[set];
        auto &allocated = entry(set, way);
        allocated = EntangledEntry{};
        allocated.tag =
            (hashed >> EntangledIndexBits) & EntangledTagMask;
        allocated.format = isTc2024() ? NumFormats : 1;
        fifo[set] = (fifo[set] + 1) % EntangledWays;
        return {set, way};
    }

    void addEntangled(std::uint64_t source, std::uint64_t destination)
    {
        auto [set, way] = allocateEntangledEntry(source);
        auto &source_entry = entry(set, way);

        for (std::uint32_t slot = 0;
             slot < MaxEntangledPerLine; ++slot) {
            if (source_entry.confidence[slot] >= ConfidenceThreshold &&
                extendEntangled(
                    source, source_entry.destinations[slot],
                    source_entry.format) == destination) {
                source_entry.confidence[slot] = ConfidenceMaximum;
                return;
            }
        }

        const auto new_format = formatEntangled(source, destination);
        while (true) {
            auto minimum_format = new_format;
            std::uint32_t valid_destinations = 1;
            auto minimum_value = ConfidenceMaximum + 1;
            std::uint32_t minimum_position = 0;
            for (std::uint32_t slot = 0;
                 slot < MaxEntangledPerLine; ++slot) {
                if (source_entry.confidence[slot] < ConfidenceThreshold) {
                    continue;
                }
                ++valid_destinations;
                const auto slot_format = formatEntangled(
                    source,
                    extendEntangled(
                        source, source_entry.destinations[slot],
                        source_entry.format));
                minimum_format = std::min(minimum_format, slot_format);
                if (source_entry.confidence[slot] < minimum_value) {
                    minimum_value = source_entry.confidence[slot];
                    minimum_position = slot;
                }
            }

            if (valid_destinations > minimum_format) {
                source_entry.confidence[minimum_position] = 0;
                continue;
            }

            for (std::uint32_t slot = 0;
                 slot < MaxEntangledPerLine; ++slot) {
                if (source_entry.confidence[slot] >= ConfidenceThreshold) {
                    source_entry.destinations[slot] = compressEntangled(
                        extendEntangled(
                            source, source_entry.destinations[slot],
                            source_entry.format),
                        minimum_format);
                }
            }
            source_entry.format = minimum_format;
            break;
        }

        for (std::uint32_t slot = 0;
             slot < MaxEntangledPerLine; ++slot) {
            if (source_entry.confidence[slot] < ConfidenceThreshold) {
                source_entry.destinations[slot] =
                    compressEntangled(destination, source_entry.format);
                source_entry.confidence[slot] = ConfidenceMaximum;
                return;
            }
        }
        assert(false && "entangled insertion must make a slot available");
    }

    bool entangledAvailable(
        std::uint64_t source, std::uint64_t destination,
        bool insert_not_present) const
    {
        const auto set =
            static_cast<std::uint32_t>(hashLine(source) % EntangledSets);
        const auto way = findEntangledWay(source);
        if (way == EntangledWays) {
            return insert_not_present;
        }
        const auto &source_entry = entry(set, way);
        for (std::uint32_t slot = 0;
             slot < MaxEntangledPerLine; ++slot) {
            if (source_entry.confidence[slot] >= ConfidenceThreshold &&
                extendEntangled(
                    source, source_entry.destinations[slot],
                    source_entry.format) == destination) {
                return true;
            }
        }

        auto minimum_format = formatEntangled(source, destination);
        std::uint32_t valid_destinations = 1;
        for (std::uint32_t slot = 0;
             slot < MaxEntangledPerLine; ++slot) {
            if (source_entry.confidence[slot] < ConfidenceThreshold) {
                continue;
            }
            ++valid_destinations;
            minimum_format = std::min(
                minimum_format,
                formatEntangled(
                    source,
                    extendEntangled(
                        source, source_entry.destinations[slot],
                        source_entry.format)));
        }
        return valid_destinations <= minimum_format;
    }

    void addBasicBlockSize(std::uint64_t line, std::uint32_t size)
    {
        if (isTc2024()) {
            assert(size <= BasicBlockSizeMaximum);
        }
        auto [set, way] = allocateEntangledEntry(line);
        auto &line_entry = entry(set, way);
        if (isTc2024()) {
            line_entry.basicBlockSize =
                std::max(line_entry.basicBlockSize, size);
        } else if (size > line_entry.basicBlockSize) {
            line_entry.basicBlockSize = size & BasicBlockSizeMaximum;
        }
    }

    std::uint32_t basicBlockSize(std::uint64_t line) const
    {
        const auto set =
            static_cast<std::uint32_t>(hashLine(line) % EntangledSets);
        const auto way = findEntangledWay(line);
        if (way < EntangledWays) {
            return entry(set, way).basicBlockSize;
        }
        return 0;
    }

    std::uint64_t entangledDestination(
        std::uint64_t source, std::uint32_t slot,
        std::uint32_t &set, std::uint32_t &way) const
    {
        set = static_cast<std::uint32_t>(hashLine(source) % EntangledSets);
        way = findEntangledWay(source);
        if (way < EntangledWays &&
            entry(set, way).confidence[slot] >= ConfidenceThreshold) {
            return extendEntangled(
                source, entry(set, way).destinations[slot],
                entry(set, way).format);
        }
        return 0;
    }

    void updateConfidence(
        std::uint32_t set, std::uint32_t way,
        std::uint64_t destination, bool accessed)
    {
        if (way >= EntangledWays) {
            return;
        }
        auto &source_entry = entry(set, way);
        for (std::uint32_t slot = 0;
             slot < MaxEntangledPerLine; ++slot) {
            if (source_entry.confidence[slot] < ConfidenceThreshold ||
                compressEntangled(
                    source_entry.destinations[slot],
                    source_entry.format) !=
                    compressEntangled(destination, source_entry.format)) {
                continue;
            }
            if (accessed &&
                source_entry.confidence[slot] < ConfidenceMaximum) {
                ++source_entry.confidence[slot];
            }
            if (!accessed && source_entry.confidence[slot] > 0) {
                --source_entry.confidence[slot];
            }
        }
    }

    std::uint32_t confidence(
        std::uint64_t source, std::uint64_t destination) const
    {
        const auto set =
            static_cast<std::uint32_t>(hashLine(source) % EntangledSets);
        const auto way = findEntangledWay(source);
        if (way == EntangledWays) {
            return 0;
        }
        const auto &source_entry = entry(set, way);
        for (std::uint32_t slot = 0;
             slot < MaxEntangledPerLine; ++slot) {
            if (source_entry.confidence[slot] >= ConfidenceThreshold &&
                extendEntangled(
                    source, source_entry.destinations[slot],
                    source_entry.format) == destination) {
                return source_entry.confidence[slot];
            }
        }
        return 0;
    }

    std::uint32_t findHistory(std::uint64_t line) const
    {
        const auto tag = line & HistoryTagMask;
        auto position = (historyHead + historyMask()) % historyEntries();
        for (std::uint32_t count = 0; count < historyEntries(); ++count) {
            if (history[position].tag == tag) {
                return position;
            }
            position = (position + historyMask()) % historyEntries();
        }
        return historyEntries();
    }

    void moveHistoryHeadToEntangled()
    {
        if (!isTc2024()) {
            return;
        }
        const auto &oldest = history[historyHead];
        if (oldest.basicBlockSize != 0) {
            addBasicBlockSize(
                oldest.tag,
                std::max(
                    basicBlockSize(oldest.tag), oldest.basicBlockSize));
        }
        if (oldest.entangledSource != 0) {
            addEntangled(oldest.entangledSource, oldest.tag);
        }
    }

    std::uint32_t addHistory(
        std::uint64_t line, std::uint64_t instruction_id,
        std::uint32_t format, std::uint32_t num_destinations,
        bool wrong_path)
    {
        while (currentCycle - historyHeadTime >= TimeDiffOverflow) {
            moveHistoryHeadToEntangled();
            auto &empty = history[historyHead];
            empty.tag = 0;
            empty.timeDiff = TimeDiffMask;
            empty.basicBlockSize = 0;
            empty.instructionId = 0;
            empty.entangledSource = 0;
            empty.format = 0;
            empty.numDestinations = 0;
            empty.wrongPath = false;
            historyHead = (historyHead + 1) % historyEntries();
            // Preserve the exact official update, including use of MASK.
            historyHeadTime += TimeDiffMask;
        }

        moveHistoryHeadToEntangled();
        auto &allocated = history[historyHead];
        allocated.tag = line & HistoryTagMask;
        allocated.timeDiff =
            (currentCycle - historyHeadTime) & TimeDiffMask;
        allocated.basicBlockSize = 0;
        allocated.instructionId = isTc2024() ? instruction_id : 0;
        allocated.entangledSource = 0;
        allocated.format = isTc2024() ? format : 0;
        allocated.numDestinations =
            isTc2024() ? num_destinations : 0;
        allocated.wrongPath = isTc2024() ? wrong_path : false;
        const auto position = historyHead;
        historyHead = (historyHead + 1) % historyEntries();
        historyHeadTime = currentCycle;
        return position;
    }

    void addHistoryBasicBlockSize(
        std::uint64_t line, std::uint32_t size, bool wrong_path)
    {
        const auto position = findHistory(line);
        assert(position < historyEntries());
        auto &history_entry = history[position];
        if (isTc2024() && wrong_path &&
            ((size & BasicBlockSizeMaximum) >
             history_entry.basicBlockSize)) {
            history_entry.wrongPath = true;
        }
        history_entry.basicBlockSize = size & BasicBlockSizeMaximum;
    }

    std::uint32_t findBasicBlockMerge(std::uint64_t line) const
    {
        const auto tag = line & HistoryTagMask;
        auto position = (historyHead + historyMask()) % historyEntries();
        for (std::uint32_t count = 0; count < historyEntries(); ++count) {
            if (count >= mergeEntries()) {
                return 0;
            }
            const auto &candidate = history[position];
            if (tag > candidate.tag &&
                tag - candidate.tag <= candidate.basicBlockSize) {
                return tag - candidate.tag;
            }
            position = (position + historyMask()) % historyEntries();
        }
        assert(false && "merge search must terminate at its fixed limit");
        return 0;
    }

    std::uint64_t selectTcSource(
        std::uint64_t line, std::uint32_t history_position,
        std::uint64_t request_latency, bool &wrong_path)
    {
        assert(history_position < historyEntries());
        const auto tag = line & HistoryTagMask;
        assert(tag != 0);
        if (history[history_position].tag != tag) {
            return 0;
        }

        auto position =
            (history_position + historyMask()) % historyEntries();
        const auto first =
            (historyHead + historyMask()) % historyEntries();
        auto accumulated_time = history[history_position].timeDiff;
        std::uint32_t searched = 0;
        std::uint64_t best_source = 0;
        std::uint32_t best_position = 0;
        std::uint32_t best_format = 0;
        std::uint8_t found = 0;

        while (position != first) {
            auto &candidate = history[position];
            if (candidate.tag == tag) {
                return 0;
            }
            if (candidate.tag != 0 &&
                accumulated_time >= request_latency) {
                ++searched;
                const auto candidate_format =
                    formatEntangled(candidate.tag, tag);
                const auto new_format =
                    candidate.numDestinations == 0 ||
                            candidate_format < candidate.format ?
                        candidate_format : candidate.format;
                if (searched == 1) {
                    best_source = candidate.tag;
                    best_position = position;
                    best_format = new_format;
                }
                if (candidate.format != 0) {
                    if (candidate.numDestinations < new_format) {
                        if (candidate_format == candidate.format) {
                            best_source = candidate.tag;
                            best_position = position;
                            best_format = new_format;
                            break;
                        } else if (found < 2) {
                            best_source = candidate.tag;
                            best_position = position;
                            best_format = new_format;
                            found = 2;
                        }
                    }
                } else if (found == 0) {
                    best_source = candidate.tag;
                    best_position = position;
                    best_format = new_format;
                    found = 1;
                }
                if (searched == historySearches()) {
                    break;
                }
            }
            accumulated_time += candidate.timeDiff;
            position = (position + historyMask()) % historyEntries();
        }

        if (best_source != 0) {
            auto &best = history[best_position];
            best.format = best_format;
            ++best.numDestinations;
            if (best.wrongPath || history[history_position].wrongPath) {
                wrong_path = true;
            }
        }
        return best_source;
    }

    std::uint64_t selectIscaSource(
        std::uint64_t line, std::uint32_t history_position,
        std::uint64_t request_latency, std::uint32_t skip) const
    {
        assert(history_position < historyEntries());
        const auto tag = line & HistoryTagMask;
        assert(tag != 0);
        if (history[history_position].tag != tag) {
            return 0;
        }

        auto position =
            (history_position + historyMask()) % historyEntries();
        const auto first =
            (historyHead + historyMask()) % historyEntries();
        auto accumulated_time = history[history_position].timeDiff;
        std::uint32_t skipped = 0;
        while (position != first) {
            const auto &candidate = history[position];
            if (candidate.tag == tag) {
                return 0;
            }
            if (candidate.tag != 0 &&
                accumulated_time >= request_latency) {
                if (skip == skipped) {
                    return candidate.tag;
                }
                ++skipped;
            }
            accumulated_time += candidate.timeDiff;
            position = (position + historyMask()) % historyEntries();
        }
        return 0;
    }

    void squashHistory(std::uint64_t instruction_id, std::uint64_t line)
    {
        auto position =
            (historyHead + historyMask()) % historyEntries();
        std::uint64_t time_to_add = 0;
        bool found_smaller = false;
        auto new_head = position;
        for (std::uint32_t count = 0; count < historyEntries(); ++count) {
            auto &candidate = history[position];
            if (candidate.instructionId > instruction_id) {
                candidate.tag = 0;
                if (!found_smaller) {
                    time_to_add += candidate.timeDiff;
                    candidate.timeDiff = 0;
                }
                candidate.basicBlockSize = 0;
                candidate.instructionId = 0;
                candidate.entangledSource = 0;
                candidate.wrongPath = false;
            } else {
                if (candidate.tag <= line &&
                    candidate.tag + candidate.basicBlockSize >= line) {
                    candidate.basicBlockSize = line - candidate.tag;
                    candidate.wrongPath = false;
                }
                if (!found_smaller) {
                    found_smaller = true;
                    candidate.timeDiff += time_to_add;
                    new_head = position;
                }
            }
            position = (position + historyMask()) % historyEntries();
        }
        historyHead = (new_head + 1) % historyEntries();
    }

    void squash(const SquashEvent &event)
    {
        if (!isTc2024()) {
            return;
        }
        setCycle(event.cycle);
        squashHistory(event.instructionId, event.virtualLine);
        const auto last =
            (historyHead + historyMask()) % historyEntries();
        auto &last_entry = history[last];
        if ((last_entry.tag <= event.virtualLine &&
             last_entry.tag + basicBlockMergeDiff + consecutiveCount >=
                 event.virtualLine) ||
            (last_entry.tag <= event.virtualLine &&
             last_entry.tag + last_entry.basicBlockSize >=
                 event.virtualLine)) {
            last_entry.basicBlockSize = 0;
            last_entry.wrongPath = false;
            lastBasicBlock = last_entry.tag;
            consecutiveCount = event.virtualLine - lastBasicBlock;
        } else {
            lastBasicBlock = 0;
            consecutiveCount = 0;
        }
        basicBlockMergeDiff = 0;
        containsWrongPath = false;
    }

    bool addTimingRequest(
        std::uint64_t line, std::uint32_t source_set,
        std::uint32_t source_way, bool accepted_prefetch)
    {
        if (timingRequests.find(line) != timingRequests.end() ||
            hasResidentVirtualLine(line)) {
            return false;
        }
        timingRequests.emplace(
            line,
            TimingRequest{
                source_set, source_way, currentCycle & TimeMask, false,
                historyEntries(), accepted_prefetch});
        return true;
    }

    bool ongoingRequest(std::uint64_t line) const
    {
        return timingRequests.find(line) != timingRequests.end();
    }

    bool hasResidentVirtualLine(std::uint64_t line) const
    {
        const auto resident = residentPhysicalByVirtual.find(line);
        return resident != residentPhysicalByVirtual.end() &&
               !resident->second.empty();
    }

    bool ongoingAccessedRequest(std::uint64_t line) const
    {
        const auto request = timingRequests.find(line);
        return request != timingRequests.end() && request->second.accessed;
    }

    bool timingAccessed(std::uint64_t line) const
    {
        const auto request = timingRequests.find(line);
        if (request != timingRequests.end()) {
            return request->second.accessed;
        }
        const auto virtual_residencies =
            residentPhysicalByVirtual.find(line);
        if (virtual_residencies == residentPhysicalByVirtual.end()) {
            return false;
        }
        for (const auto physical_line : virtual_residencies->second) {
            const auto resident = residentByPhysical.find(physical_line);
            if (resident != residentByPhysical.end() &&
                resident->second.accessed) {
                return true;
            }
        }
        return false;
    }

    bool differentTimingSource(
        std::uint64_t line, std::uint32_t source_set,
        std::uint32_t source_way) const
    {
        const auto request = timingRequests.find(line);
        if (request != timingRequests.end() &&
            (request->second.sourceSet != source_set ||
             request->second.sourceWay != source_way)) {
            return true;
        }

        const auto virtual_residencies =
            residentPhysicalByVirtual.find(line);
        if (virtual_residencies == residentPhysicalByVirtual.end()) {
            return false;
        }
        for (const auto physical_line : virtual_residencies->second) {
            const auto resident = residentByPhysical.find(physical_line);
            if (resident != residentByPhysical.end() &&
                (resident->second.sourceSet != source_set ||
                 resident->second.sourceWay != source_way)) {
                return true;
            }
        }
        return false;
    }

    void accessTiming(
        std::uint64_t virtual_line, std::uint64_t physical_line,
        std::uint32_t history_position)
    {
        const auto request = timingRequests.find(virtual_line);
        if (request != timingRequests.end()) {
            auto &timing = request->second;
            if (!timing.accessed) {
                timing.accessed = true;
                timing.historyPosition = history_position;
                if (!isTc2024() && timing.sourceWay < EntangledWays) {
                    const auto source_set = timing.sourceSet;
                    const auto source_way = timing.sourceWay;
                    timing.sourceSet = 0;
                    timing.sourceWay = NoSourceWay;
                    updateConfidence(
                        source_set, source_way, virtual_line, false);
                }
            }
            return;
        }

        const auto resident = residentByPhysical.find(physical_line);
        if (resident != residentByPhysical.end()) {
            resident->second.accessed = true;
        }
    }

    std::uint64_t requestLatency(
        std::uint64_t line, std::uint32_t &history_position) const
    {
        const auto request = timingRequests.find(line);
        if (request == timingRequests.end() || !request->second.accessed) {
            return 0;
        }
        history_position = request->second.historyPosition;
        return latency(currentCycle, request->second.timestamp);
    }

    void removeResidencyIndex(
        std::uint64_t virtual_line, std::uint64_t physical_line)
    {
        const auto virtual_residencies =
            residentPhysicalByVirtual.find(virtual_line);
        if (virtual_residencies == residentPhysicalByVirtual.end()) {
            return;
        }
        virtual_residencies->second.erase(physical_line);
        if (virtual_residencies->second.empty()) {
            residentPhysicalByVirtual.erase(virtual_residencies);
        }
    }

    void installResidency(
        std::uint64_t virtual_line, std::uint64_t physical_line)
    {
        const auto existing = residentByPhysical.find(physical_line);
        if (existing != residentByPhysical.end()) {
            ++statistics.overwrittenResidencies;
            const auto state = existing->second;
            if (!state.accessed) {
                ++statistics.wrongPrefetches;
            }
            if (state.sourceWay < EntangledWays) {
                updateConfidence(
                    state.sourceSet, state.sourceWay,
                    state.virtualLine, state.accessed);
            }
            removeResidencyIndex(
                state.virtualLine, physical_line);
            residentByPhysical.erase(existing);
        }

        Residency residency;
        residency.virtualLine = virtual_line;
        const auto request = timingRequests.find(virtual_line);
        if (request == timingRequests.end()) {
            residency.sourceWay = NoSourceWay;
            residency.accessed = true;
        } else {
            residency.sourceSet = request->second.sourceSet;
            residency.sourceWay = request->second.sourceWay;
            residency.accessed = request->second.accessed;
            timingRequests.erase(request);
        }
        residentByPhysical.emplace(physical_line, residency);
        residentPhysicalByVirtual[virtual_line].insert(physical_line);
    }

    bool issueCandidate(
        const Candidate &candidate, std::uint32_t timing_source_set,
        std::uint32_t timing_source_way, const AcceptCallback &accept,
        DemandResult &result)
    {
        ++result.candidatesOffered;
        ++statistics.candidatesOffered;
        if (hasResidentVirtualLine(candidate.virtualLine)) {
            ++statistics.candidatesRejected;
            return false;
        }
        const bool accepted = accept && accept(candidate);
        if (!accepted) {
            ++statistics.candidatesRejected;
            return false;
        }

        const bool inserted = addTimingRequest(
            candidate.virtualLine, timing_source_set,
            timing_source_way, true);
        if (!inserted) {
            ++statistics.candidatesRejected;
            return false;
        }
        ++result.candidatesAccepted;
        ++statistics.candidatesAccepted;
        return true;
    }

    DemandResult demand(
        const DemandAccess &access, const AcceptCallback &accept)
    {
        setCycle(access.cycle);
        assert(!access.prefetchHit || access.cacheHit);

        DemandResult result;
        ++statistics.demandAccesses;
        if (!access.cacheHit) {
            ++statistics.demandMisses;
            if (ongoingRequest(access.virtualLine) &&
                !timingAccessed(access.virtualLine)) {
                ++statistics.latePrefetches;
            }
        } else if (residentByPhysical.find(access.physicalLine) ==
                   residentByPhysical.end()) {
            ++statistics.untrackedDemandHits;
        }
        if (access.prefetchHit) {
            ++statistics.prefetchHits;
        }

        bool consecutive = false;
        if (lastBasicBlock + consecutiveCount == access.virtualLine) {
            result.repeatedCurrentLine = true;
            return result;
        } else if (
            lastBasicBlock + consecutiveCount + 1 == access.virtualLine) {
            ++consecutiveCount;
            if (isTc2024() && access.wrongPath) {
                containsWrongPath = true;
            }
            consecutive = true;
        }

        auto basic_block_size = basicBlockSize(access.virtualLine);
        for (std::uint32_t offset = 1;
             offset <= basic_block_size; ++offset) {
            const auto candidate_line = access.virtualLine + offset;
            if (ongoingRequest(candidate_line)) {
                continue;
            }
            const Candidate candidate{
                candidate_line,
                access.virtualLine,
                access.virtualLine,
                0,
                NoSourceWay,
                CandidateKind::BasicBlock};
            issueCandidate(
                candidate, 0, NoSourceWay, accept, result);
        }

        auto source_format = NumFormats;
        std::uint32_t num_entangled = 0;
        for (std::uint32_t slot = 0;
             slot < MaxEntangledPerLine; ++slot) {
            std::uint32_t source_set = 0;
            std::uint32_t source_way = NoSourceWay;
            const auto entangled_line = entangledDestination(
                access.virtualLine, slot, source_set, source_way);
            if (entangled_line == 0 ||
                entangled_line == access.virtualLine) {
                continue;
            }

            if (isTc2024()) {
                source_format = std::min(
                    source_format,
                    formatEntangled(access.virtualLine, entangled_line));
            }
            ++num_entangled;
            basic_block_size = basicBlockSize(entangled_line);
            for (std::uint32_t offset = 0;
                 offset <= basic_block_size; ++offset) {
                const auto candidate_line = entangled_line + offset;
                if (isTc2024() && differentTimingSource(
                        candidate_line, source_set, source_way)) {
                    updateConfidence(
                        source_set, source_way, candidate_line, false);
                }
                if (ongoingRequest(candidate_line)) {
                    continue;
                }

                const auto kind = offset == 0 ?
                    CandidateKind::Entangled :
                    CandidateKind::EntangledBasicBlock;
                const Candidate candidate{
                    candidate_line,
                    access.virtualLine,
                    entangled_line,
                    source_set,
                    source_way,
                    kind};
                issueCandidate(
                    candidate, source_set,
                    offset == 0 ? source_way : NoSourceWay,
                    accept, result);
            }
        }

        if (isTc2024()) {
            assert(source_format >= num_entangled);
            if (num_entangled == 0 && basic_block_size == 0) {
                source_format = 0;
            }
        }

        if (!consecutive && consecutiveCount != 0) {
            const auto maximum_size = basicBlockSize(lastBasicBlock);
            if (basicBlockMergeDiff > 0) {
                const auto block_start =
                    lastBasicBlock - basicBlockMergeDiff;
                const auto block_size =
                    consecutiveCount + basicBlockMergeDiff;
                if (!isTc2024()) {
                    addBasicBlockSize(block_start, block_size);
                }
                addHistoryBasicBlockSize(
                    block_start, block_size,
                    isTc2024() && containsWrongPath);
            } else {
                const auto block_size =
                    std::max(maximum_size, consecutiveCount);
                if (!isTc2024()) {
                    addBasicBlockSize(lastBasicBlock, block_size);
                }
                addHistoryBasicBlockSize(
                    lastBasicBlock, block_size,
                    isTc2024() && containsWrongPath);
            }
        }

        if (!consecutive) {
            consecutiveCount = 0;
            lastBasicBlock = access.virtualLine;
            if (isTc2024()) {
                containsWrongPath = access.wrongPath;
            }
            basicBlockMergeDiff =
                findBasicBlockMerge(lastBasicBlock);
        }

        auto history_position = historyEntries();
        if (!consecutive && basicBlockMergeDiff == 0) {
            if (findHistory(access.virtualLine) == historyEntries() ||
                (!access.cacheHit &&
                 !ongoingAccessedRequest(access.virtualLine))) {
                history_position = addHistory(
                    access.virtualLine, access.instructionId,
                    isTc2024() ? source_format : 0,
                    isTc2024() ? num_entangled : 0,
                    isTc2024() && access.wrongPath);
            }
        }

        if (!access.cacheHit && !ongoingRequest(access.virtualLine)) {
            const bool inserted = addTimingRequest(
                access.virtualLine, 0, NoSourceWay, false);
            assert(inserted);
        }
        accessTiming(
            access.virtualLine, access.physicalLine, history_position);
        return result;
    }

    void fill(const FillEvent &event)
    {
        setCycle(event.cycle);
        auto history_position = historyEntries();
        const auto fill_latency =
            requestLatency(event.virtualLine, history_position);
        installResidency(event.virtualLine, event.physicalLine);

        if (fill_latency == 0 ||
            history_position >= historyEntries()) {
            return;
        }
        if (isTc2024()) {
            bool wrong_path = false;
            const auto source = selectTcSource(
                event.virtualLine, history_position,
                fill_latency, wrong_path);
            if (source != 0) {
                assert(source != event.virtualLine);
                history[history_position].entangledSource = source;
                if (wrong_path) {
                    history[history_position].wrongPath = true;
                }
            }
            return;
        }

        bool inserted = false;
        for (std::uint32_t attempt = 0; attempt < 2; ++attempt) {
            const auto source = selectIscaSource(
                event.virtualLine, history_position,
                fill_latency, attempt);
            if (source != 0 && source != event.virtualLine &&
                entangledAvailable(
                    source, event.virtualLine, false)) {
                addEntangled(source, event.virtualLine);
                inserted = true;
                break;
            }
        }
        if (!inserted) {
            const auto source = selectIscaSource(
                event.virtualLine, history_position,
                fill_latency, 0);
            if (source != 0 && source != event.virtualLine) {
                addEntangled(source, event.virtualLine);
            }
        }
    }

    bool evict(const EvictEvent &event)
    {
        setCycle(event.cycle);
        const auto resident = residentByPhysical.find(event.physicalLine);
        if (resident == residentByPhysical.end()) {
            ++statistics.untrackedEvictions;
            return false;
        }

        const auto state = resident->second;
        if (!state.accessed) {
            ++statistics.wrongPrefetches;
        }
        if (state.sourceWay < EntangledWays) {
            updateConfidence(
                state.sourceSet, state.sourceWay,
                state.virtualLine, state.accessed);
        }
        removeResidencyIndex(state.virtualLine, event.physicalLine);
        residentByPhysical.erase(resident);
        return true;
    }

    bool cancel(std::uint64_t virtual_line)
    {
        const auto request = timingRequests.find(virtual_line);
        if (request == timingRequests.end() ||
            !request->second.acceptedPrefetch) {
            return false;
        }
        timingRequests.erase(request);
        ++statistics.acceptedRequestsCanceled;
        return true;
    }

    std::uint64_t algorithmDigest() const
    {
        StateDigest digest;
        digest.add(static_cast<std::uint8_t>(algorithmVersion));
        digest.add(lastBasicBlock);
        digest.add(consecutiveCount);
        digest.add(basicBlockMergeDiff);
        digest.add(containsWrongPath);
        digest.add(historyHead);
        digest.add(historyHeadTime);
        digest.add(historyEntries());
        for (std::uint32_t i = 0; i < historyEntries(); ++i) {
            const auto &item = history[i];
            digest.add(item.tag);
            digest.add(item.timeDiff);
            digest.add(item.basicBlockSize);
            digest.add(item.instructionId);
            digest.add(item.entangledSource);
            digest.add(item.format);
            digest.add(item.numDestinations);
            digest.add(item.wrongPath);
        }
        for (const auto &item : entangled) {
            digest.add(item.tag);
            digest.add(item.format);
            for (std::uint32_t slot = 0;
                 slot < MaxEntangledPerLine; ++slot) {
                digest.add(item.destinations[slot]);
                digest.add(item.confidence[slot]);
            }
            digest.add(item.basicBlockSize);
        }
        for (const auto item : fifo) {
            digest.add(item);
        }

        std::vector<std::uint64_t> timing_lines;
        timing_lines.reserve(timingRequests.size());
        for (const auto &[line, request] : timingRequests) {
            timing_lines.push_back(line);
        }
        std::sort(timing_lines.begin(), timing_lines.end());
        digest.add(timing_lines.size());
        for (const auto line : timing_lines) {
            const auto &request = timingRequests.at(line);
            digest.add(line);
            digest.add(
                request.sourceWay < EntangledWays ?
                request.sourceSet : 0);
            digest.add(request.sourceWay);
            digest.add(request.timestamp);
            digest.add(request.accessed);
            digest.add(request.accessed ? request.historyPosition : 0);
        }

        std::vector<std::array<std::uint64_t, 4>> residencies;
        residencies.reserve(residentByPhysical.size());
        for (const auto &[physical_line, resident] : residentByPhysical) {
            residencies.push_back({
                resident.virtualLine,
                resident.sourceWay < EntangledWays ?
                    resident.sourceSet : 0,
                resident.sourceWay, resident.accessed});
        }
        std::sort(residencies.begin(), residencies.end());
        digest.add(residencies.size());
        for (const auto &resident : residencies) {
            for (const auto field : resident) {
                digest.add(field);
            }
        }

        return digest.get();
    }

    std::uint64_t digest() const
    {
        StateDigest digest;
        digest.add(algorithmDigest());
        digest.add(currentCycle);

        std::vector<std::uint64_t> timing_lines;
        timing_lines.reserve(timingRequests.size());
        for (const auto &[line, request] : timingRequests) {
            timing_lines.push_back(line);
        }
        std::sort(timing_lines.begin(), timing_lines.end());
        digest.add(timing_lines.size());
        for (const auto line : timing_lines) {
            const auto &request = timingRequests.at(line);
            digest.add(line);
            digest.add(request.sourceSet);
            digest.add(request.sourceWay);
            digest.add(request.timestamp);
            digest.add(request.accessed);
            digest.add(request.historyPosition);
            digest.add(request.acceptedPrefetch);
        }

        std::vector<std::uint64_t> physical_lines;
        physical_lines.reserve(residentByPhysical.size());
        for (const auto &[line, resident] : residentByPhysical) {
            physical_lines.push_back(line);
        }
        std::sort(physical_lines.begin(), physical_lines.end());
        digest.add(physical_lines.size());
        for (const auto line : physical_lines) {
            const auto &resident = residentByPhysical.at(line);
            digest.add(line);
            digest.add(resident.virtualLine);
            digest.add(resident.sourceSet);
            digest.add(resident.sourceWay);
            digest.add(resident.accessed);
        }
        return digest.get();
    }
};

EntanglingPrefetcher::EntanglingPrefetcher(
    AlgorithmVersion version, std::uint64_t initial_cycle)
    : impl(std::make_unique<Impl>(version, initial_cycle))
{
}

EntanglingPrefetcher::~EntanglingPrefetcher() = default;
EntanglingPrefetcher::EntanglingPrefetcher(
    EntanglingPrefetcher &&) noexcept = default;
EntanglingPrefetcher &
EntanglingPrefetcher::operator=(EntanglingPrefetcher &&) noexcept = default;

EntanglingPrefetcher::DemandResult
EntanglingPrefetcher::observeDemand(
    const DemandAccess &access, const AcceptCallback &accept)
{
    return impl->demand(access, accept);
}

void
EntanglingPrefetcher::observeFill(const FillEvent &event)
{
    impl->fill(event);
}

bool
EntanglingPrefetcher::observeEvict(const EvictEvent &event)
{
    return impl->evict(event);
}

void
EntanglingPrefetcher::observeSquash(const SquashEvent &event)
{
    impl->squash(event);
}

bool
EntanglingPrefetcher::cancelAcceptedRequest(std::uint64_t virtual_line)
{
    return impl->cancel(virtual_line);
}

EntanglingPrefetcher::AlgorithmVersion
EntanglingPrefetcher::version() const
{
    return impl->algorithmVersion;
}

EntanglingPrefetcher::BundleInfo
EntanglingPrefetcher::bundleInfo() const
{
    return BundleInfo{
        impl->historyEntries(),
        impl->historySearches(),
        impl->mergeEntries(),
        impl->isTc2024(),
        impl->isTc2024(),
        impl->isTc2024()};
}

const EntanglingPrefetcher::Stats &
EntanglingPrefetcher::stats() const
{
    return impl->statistics;
}

std::size_t
EntanglingPrefetcher::inflightRequests() const
{
    return impl->timingRequests.size();
}

std::size_t
EntanglingPrefetcher::residentLines() const
{
    return impl->residentByPhysical.size();
}

bool
EntanglingPrefetcher::hasResidentVirtualLine(
    std::uint64_t virtualLine) const
{
    return impl->hasResidentVirtualLine(virtualLine);
}

std::uint64_t
EntanglingPrefetcher::stateDigest() const
{
    return impl->digest();
}

std::uint64_t
EntanglingPrefetcher::algorithmStateDigest() const
{
    return impl->algorithmDigest();
}

std::uint32_t
EntanglingPrefetcher::debugFormat(
    std::uint64_t source, std::uint64_t destination)
{
    return formatEntangled(source, destination);
}

std::uint64_t
EntanglingPrefetcher::debugCompress(
    std::uint64_t destination, std::uint32_t format)
{
    return compressEntangled(destination, format);
}

std::uint64_t
EntanglingPrefetcher::debugExtend(
    std::uint64_t source, std::uint64_t destination,
    std::uint32_t format)
{
    return extendEntangled(source, destination, format);
}

std::uint64_t
EntanglingPrefetcher::debugHash(std::uint64_t line)
{
    return hashLine(line);
}

void
EntanglingPrefetcher::debugAddEntangled(
    std::uint64_t source, std::uint64_t destination)
{
    impl->addEntangled(source, destination);
}

void
EntanglingPrefetcher::debugAddBasicBlockSize(
    std::uint64_t line, std::uint32_t size)
{
    impl->addBasicBlockSize(line, size);
}

std::uint32_t
EntanglingPrefetcher::debugConfidence(
    std::uint64_t source, std::uint64_t destination) const
{
    return impl->confidence(source, destination);
}

std::uint32_t
EntanglingPrefetcher::debugBasicBlockSize(std::uint64_t line) const
{
    return impl->basicBlockSize(line);
}

std::uint32_t
EntanglingPrefetcher::debugFindHistory(std::uint64_t line) const
{
    return impl->findHistory(line);
}

std::uint32_t
EntanglingPrefetcher::debugHistoryHead() const
{
    return impl->historyHead;
}

std::uint64_t
EntanglingPrefetcher::debugHistoryTag(std::uint32_t index) const
{
    assert(index < impl->historyEntries());
    return impl->history[index].tag;
}

std::uint64_t
EntanglingPrefetcher::debugHistoryTimeDiff(std::uint32_t index) const
{
    assert(index < impl->historyEntries());
    return impl->history[index].timeDiff;
}

std::uint64_t
EntanglingPrefetcher::debugHistoryInstructionId(
    std::uint32_t index) const
{
    assert(index < impl->historyEntries());
    return impl->history[index].instructionId;
}

std::uint64_t
EntanglingPrefetcher::debugHistoryEntangledSource(
    std::uint32_t index) const
{
    assert(index < impl->historyEntries());
    return impl->history[index].entangledSource;
}

std::uint32_t
EntanglingPrefetcher::debugHistoryBasicBlockSize(
    std::uint32_t index) const
{
    assert(index < impl->historyEntries());
    return impl->history[index].basicBlockSize;
}

bool
EntanglingPrefetcher::debugTimingAccessed(std::uint64_t line) const
{
    return impl->timingAccessed(line);
}

std::uint32_t
EntanglingPrefetcher::debugTimingSourceWay(std::uint64_t line) const
{
    const auto request = impl->timingRequests.find(line);
    if (request != impl->timingRequests.end()) {
        return request->second.sourceWay;
    }
    return NoSourceWay;
}

} // namespace o3
} // namespace gem5
