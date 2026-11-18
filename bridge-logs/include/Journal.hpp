#pragma once

// The journal: a ring of log records in one byte region, which the Windows
// layer maps from logs/journal.bin. A record is complete once its state word
// is stored (release order, after every other byte), so a crash at any moment
// leaves either the whole record or one that recovery ignores. Windows keeps
// the dirty pages of a mapped file when the process dies, so a record written
// here survives a crash without any syscall on the logging path.
//
// Recovery never trusts a stored head or tail: it scans the region for valid
// records (magic, sane sizes, CRC32, the session asked for) and orders them
// by sequence. Records of older sessions and overwritten ones fail the checks.
//
// Layout: a 64-byte header, then the ring (capacity bytes, a multiple of 8).
// Record: a 56-byte header, then module, folder, category and message bytes,
// padded to 8. A record that would cross the end of the ring is preceded by a
// padding marker covering the rest, and goes to offset 0. Portable.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace UE4SSLuaLogBridge::Core
{
inline uint32_t crc32(const uint8_t* data, std::size_t size, uint32_t crc = 0)
{
    static const auto table = [] {
        std::array<uint32_t, 256> values{};
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            values[i] = c;
        }
        return values;
    }();
    crc = ~crc;
    for (std::size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

enum class RecordState : uint32_t
{
    none = 0,
    written = 1,
    delivering = 2,
    delivered = 3,
};

struct JournalEntry
{
    uint64_t seq{};
    int64_t time{};
    int rank{};
    uint8_t target{};
    std::string_view module;
    std::string_view folder;
    std::string_view category;
    std::string_view message;
};

struct RecoveredRecord
{
    uint64_t seq{};
    int64_t time{};
    int rank{};
    uint8_t target{};
    RecordState state{};
    std::string module;
    std::string folder;
    std::string category;
    std::string message;
};

class Journal
{
public:
    static constexpr uint32_t file_magic = 0x4A424C4C;    // 'LLBJ'
    static constexpr uint32_t record_magic = 0x52424C4C;  // 'LLBR'
    static constexpr uint32_t padding_magic = 0x50424C4C; // 'LLBP'
    static constexpr uint32_t format_version = 1;
    static constexpr std::size_t header_bytes = 64;
    static constexpr std::size_t record_header_bytes = 56;

    // `bytes` covers the header and the ring; the ring is the rest, rounded down to 8.
    Journal(uint8_t* base, std::size_t bytes)
        : base_(base), ring_(base + header_bytes), capacity_(bytes > header_bytes ? (bytes - header_bytes) & ~std::size_t{7} : 0)
    {
    }

    // Validates the header (re-initializing a foreign or resized region),
    // starts a new session and returns the previous one (0 when none).
    uint64_t open()
    {
        uint32_t magic = load32(base_), version = load32(base_ + 4);
        uint64_t capacity = load64(base_ + 8), session = load64(base_ + 16);
        uint64_t previous = 0;
        if (magic == file_magic && version == format_version && capacity == capacity_) previous = session;
        else std::memset(base_, 0, header_bytes);
        store32(base_, file_magic);
        store32(base_ + 4, format_version);
        store64(base_ + 8, capacity_);
        session_ = previous + 1;
        store64(base_ + 16, session_);
        return previous;
    }

    [[nodiscard]] uint64_t session() const { return session_; }
    [[nodiscard]] std::size_t capacity() const { return capacity_; }
    [[nodiscard]] std::size_t used() const { return used_; }
    [[nodiscard]] bool empty() const { return slots_.empty(); }

    static std::size_t record_size(const JournalEntry& entry)
    {
        const auto raw = record_header_bytes + entry.module.size() + entry.folder.size() + entry.category.size() +
            entry.message.size();
        return (raw + 7) & ~std::size_t{7};
    }

    // Appends a record in state `written`; returns its slot id, or 0 when the
    // ring has no room (deliver and reclaim, then try again) or the record
    // could never fit.
    uint64_t append(const JournalEntry& entry)
    {
        const auto need = record_size(entry);
        if (need > capacity_ || entry.folder.size() > 0xFFFF || entry.module.size() > 0xFFFF ||
            entry.category.size() > 0xFFFF || entry.message.size() > 0xFFFFFFF)
        {
            return 0;
        }
        std::size_t at = 0;
        if (!place(need, at)) return 0;
        write_record(at, need, entry);
        slots_.push_back(Slot{at, need, false});
        used_ += need;
        return first_id_ + slots_.size() - 1;
    }

    // The bytes of a live slot's record, for delivery.
    [[nodiscard]] RecoveredRecord read(uint64_t id) const
    {
        const auto& slot = slots_[static_cast<std::size_t>(id - first_id_)];
        RecoveredRecord record;
        parse(slot.offset, record);
        return record;
    }

    void mark(uint64_t id, RecordState state)
    {
        auto& slot = slots_[static_cast<std::size_t>(id - first_id_)];
        if (state == RecordState::delivered) slot.delivered = true;
        state_ref(slot.offset).store(static_cast<uint32_t>(state), std::memory_order_release);
    }

    // Frees delivered records at the oldest end.
    void reclaim()
    {
        while (!slots_.empty() && slots_.front().delivered)
        {
            used_ -= slots_.front().size;
            slots_.pop_front();
            ++first_id_;
        }
    }

    // Every valid record of `session` in the region, by sequence.
    [[nodiscard]] std::vector<RecoveredRecord> recover(uint64_t session) const
    {
        std::vector<RecoveredRecord> found;
        if (session == 0) return found;
        std::size_t offset = 0;
        while (offset + record_header_bytes <= capacity_)
        {
            RecoveredRecord record;
            std::size_t size = 0;
            if (valid(offset, session, size) && parse(offset, record))
            {
                found.push_back(std::move(record));
                offset += size;
            }
            else
            {
                offset += 8;
            }
        }
        std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.seq < b.seq; });
        found.erase(std::unique(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.seq == b.seq; }),
            found.end());
        return found;
    }

private:
    struct Slot
    {
        std::size_t offset;
        std::size_t size;
        bool delivered;
    };

    uint8_t* base_;
    uint8_t* ring_;
    std::size_t capacity_;
    uint64_t session_{};
    std::size_t head_{};
    std::size_t used_{};
    std::deque<Slot> slots_;
    uint64_t first_id_{1};

    static uint32_t load32(const uint8_t* at)
    {
        uint32_t value;
        std::memcpy(&value, at, 4);
        return value;
    }
    static uint64_t load64(const uint8_t* at)
    {
        uint64_t value;
        std::memcpy(&value, at, 8);
        return value;
    }
    static void store32(uint8_t* at, uint32_t value) { std::memcpy(at, &value, 4); }
    static void store64(uint8_t* at, uint64_t value) { std::memcpy(at, &value, 8); }

    std::atomic_ref<uint32_t> state_ref(std::size_t offset) const
    {
        return std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(ring_ + offset + 12));
    }

    void pad(std::size_t at, std::size_t size)
    {
        if (size == 0) return;
        store32(ring_ + at, padding_magic);
        store32(ring_ + at + 4, static_cast<uint32_t>(size));
        slots_.push_back(Slot{at, size, true});
        used_ += size;
    }

    bool place(std::size_t need, std::size_t& at)
    {
        if (slots_.empty())
        {
            if (need > capacity_ - head_)
            {
                pad(head_, capacity_ - head_);
                head_ = 0;
            }
        }
        else
        {
            const auto tail = slots_.front().offset;
            if (head_ > tail)
            {
                if (need > capacity_ - head_)
                {
                    if (need > tail) return false;
                    pad(head_, capacity_ - head_);
                    head_ = 0;
                }
            }
            else if (need > tail - head_) // head_ == tail with live records: full
            {
                return false;
            }
        }
        at = head_;
        head_ += need;
        if (head_ == capacity_) head_ = 0;
        return true;
    }

    void write_record(std::size_t at, std::size_t size, const JournalEntry& entry)
    {
        uint8_t* record = ring_ + at;
        state_ref(at).store(0, std::memory_order_relaxed);
        store32(record, record_magic);
        store32(record + 4, static_cast<uint32_t>(size));
        store64(record + 16, session_);
        store64(record + 24, entry.seq);
        store64(record + 32, static_cast<uint64_t>(entry.time));
        record[40] = static_cast<uint8_t>(entry.rank);
        record[41] = entry.target;
        record[42] = 0;
        record[43] = 0;
        const uint16_t lengths[4] = {static_cast<uint16_t>(entry.module.size()), static_cast<uint16_t>(entry.folder.size()),
            static_cast<uint16_t>(entry.category.size()), 0};
        std::memcpy(record + 44, lengths, 8);
        store32(record + 52, static_cast<uint32_t>(entry.message.size()));
        uint8_t* cursor = record + record_header_bytes;
        for (auto part : {entry.module, entry.folder, entry.category, entry.message})
        {
            if (!part.empty()) std::memcpy(cursor, part.data(), part.size());
            cursor += part.size();
        }
        std::memset(cursor, 0, static_cast<std::size_t>(record + size - cursor));
        const auto payload = static_cast<std::size_t>(cursor - record);
        store32(record + 8, crc32(record + 16, payload - 16));
        state_ref(at).store(static_cast<uint32_t>(RecordState::written), std::memory_order_release);
    }

    bool valid(std::size_t offset, uint64_t session, std::size_t& size) const
    {
        const uint8_t* record = ring_ + offset;
        if (load32(record) != record_magic) return false;
        size = load32(record + 4);
        if (size < record_header_bytes || size % 8 != 0 || size > capacity_ - offset) return false;
        if (load64(record + 16) != session) return false;
        const auto state = load32(record + 12);
        if (state < 1 || state > 3) return false;
        uint16_t lengths[4];
        std::memcpy(lengths, record + 44, 8);
        const std::size_t payload = record_header_bytes + lengths[0] + lengths[1] + lengths[2] + load32(record + 52);
        if (payload > size) return false;
        return crc32(record + 16, payload - 16) == load32(record + 8);
    }

    bool parse(std::size_t offset, RecoveredRecord& out) const
    {
        const uint8_t* record = ring_ + offset;
        out.seq = load64(record + 24);
        out.time = static_cast<int64_t>(load64(record + 32));
        out.rank = record[40];
        out.target = record[41];
        out.state = static_cast<RecordState>(load32(record + 12));
        uint16_t lengths[4];
        std::memcpy(lengths, record + 44, 8);
        const auto message_length = load32(record + 52);
        const char* cursor = reinterpret_cast<const char*>(record + record_header_bytes);
        out.module.assign(cursor, lengths[0]);
        cursor += lengths[0];
        out.folder.assign(cursor, lengths[1]);
        cursor += lengths[1];
        out.category.assign(cursor, lengths[2]);
        cursor += lengths[2];
        out.message.assign(cursor, message_length);
        return true;
    }
};
} // namespace UE4SSLuaLogBridge::Core
