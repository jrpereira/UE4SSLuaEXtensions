// The journal: appending, wrapping, reclaiming, sessions and crash recovery.
#include <Journal.hpp>
#include <TestSupport.hpp>

#include <string>
#include <vector>

using namespace UE4SSLuaLogBridge::Core;

namespace
{
JournalEntry entry(uint64_t seq, std::string_view message, std::string_view category = "main")
{
    return JournalEntry{seq, 20261118140322123, 3, 1, "Mod", "C:/Mods/Mod", category, message};
}

void crc()
{
    const std::string text = "123456789";
    CHECK(crc32(reinterpret_cast<const uint8_t*>(text.data()), text.size()) == 0xCBF43926u);
}

void append_and_read()
{
    std::vector<uint8_t> region(4096);
    Journal journal(region.data(), region.size());
    CHECK(journal.open() == 0 && journal.session() == 1);
    CHECK(journal.capacity() == 4096 - Journal::header_bytes);
    const auto id = journal.append(entry(1, "hello"));
    CHECK(id == 1 && !journal.empty());
    const auto record = journal.read(id);
    CHECK(record.seq == 1 && record.rank == 3 && record.target == 1 && record.state == RecordState::written);
    CHECK(record.module == "Mod" && record.folder == "C:/Mods/Mod" && record.category == "main" && record.message == "hello");
    CHECK(record.time == 20261118140322123);
    journal.mark(id, RecordState::delivered);
    journal.reclaim();
    CHECK(journal.empty() && journal.used() == 0);
}

void full_and_wrap()
{
    std::vector<uint8_t> region(Journal::header_bytes + 1024);
    Journal journal(region.data(), region.size());
    journal.open();
    const std::string message(100, 'x'); // 56 + 3 + 11 + 4 + 100 = 174 -> 176 bytes
    CHECK(Journal::record_size(entry(1, message)) == 176);
    std::vector<uint64_t> ids;
    for (uint64_t seq = 1; seq <= 5; ++seq) ids.push_back(journal.append(entry(seq, message)));
    CHECK(ids.back() == 5 && journal.used() == 880);
    CHECK(journal.append(entry(6, message)) == 0); // 144 bytes left at the end, none at the start

    // Freeing the oldest two lets the next record wrap to offset 0 behind a padding marker.
    journal.mark(ids[0], RecordState::delivered);
    journal.mark(ids[1], RecordState::delivered);
    journal.reclaim();
    const auto wrapped = journal.append(entry(6, message));
    CHECK(wrapped != 0 && journal.read(wrapped).seq == 6);
    CHECK(journal.append(entry(7, message)) != 0); // the second freed record's room
    CHECK(journal.append(entry(8, message)) == 0);  // head has reached the tail
    // Out-of-order delivery: the tail stays until the oldest is delivered.
    journal.mark(ids[3], RecordState::delivered);
    journal.reclaim();
    CHECK(journal.append(entry(8, message)) == 0);
    journal.mark(ids[2], RecordState::delivered);
    journal.reclaim();
    CHECK(journal.append(entry(8, message)) != 0);

    CHECK(journal.append(entry(9, std::string(2000, 'y'))) == 0); // never fits
}

void recovery()
{
    std::vector<uint8_t> region(Journal::header_bytes + 2048);
    {
        Journal journal(region.data(), region.size());
        CHECK(journal.open() == 0);
        for (uint64_t seq = 1; seq <= 30; ++seq)
        {
            const auto id = journal.append(entry(seq, "line " + std::to_string(seq), seq % 2 ? "main" : "combat"));
            if (id == 0) break; // full: the rest were never logged
            if (seq <= 10)
            {
                journal.mark(id, RecordState::delivered);
                journal.reclaim();
            }
        }
        // The process "crashes" here: nothing else is written to the region.
    }
    Journal next(region.data(), region.size());
    const auto previous = next.open();
    CHECK(previous == 1 && next.session() == 2);
    const auto records = next.recover(previous);
    CHECK(!records.empty());
    bool ordered = true, states = true;
    for (std::size_t i = 1; i < records.size(); ++i) ordered = ordered && records[i - 1].seq < records[i].seq;
    for (const auto& record : records)
    {
        states = states && (record.seq <= 10 ? record.state == RecordState::delivered : record.state == RecordState::written);
        states = states && record.message == "line " + std::to_string(record.seq);
    }
    CHECK(ordered && states);
    CHECK(records.back().seq >= 20); // the most recent lines survive
    CHECK(next.recover(2).empty() && next.recover(0).empty());
}

void torn_and_corrupt()
{
    std::vector<uint8_t> region(Journal::header_bytes + 1024);
    Journal journal(region.data(), region.size());
    journal.open();
    journal.append(entry(1, "kept"));
    const auto torn = journal.append(entry(2, "torn"));
    const auto corrupt = journal.append(entry(3, "corrupt"));
    (void)torn;
    (void)corrupt;
    // Record 2 never got its state word (the process died while copying it).
    const std::size_t second = Journal::header_bytes + Journal::record_size(entry(1, "kept"));
    region[second + 12] = 0;
    // Record 3's message was overwritten after its CRC.
    const std::size_t third = second + Journal::record_size(entry(2, "torn"));
    region[third + Journal::record_header_bytes + 3 + 11 + 4] ^= 0xFF;

    Journal next(region.data(), region.size());
    const auto records = next.recover(next.open());
    CHECK(records.size() == 1 && records[0].message == "kept");
}

void foreign_header()
{
    std::vector<uint8_t> region(Journal::header_bytes + 512, 0xAB);
    Journal journal(region.data(), region.size());
    CHECK(journal.open() == 0 && journal.session() == 1);
    // A different capacity starts over.
    Journal again(region.data(), region.size() - 64);
    CHECK(again.open() == 0);
}
} // namespace

int main()
{
    crc();
    append_and_read();
    full_and_wrap();
    recovery();
    torn_and_corrupt();
    foreign_header();
    return LexTests::finish("JournalTests");
}
