#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

#include "mjpeg_index_reader.h"

namespace {

std::vector<uint8_t> MakeCandidate(size_t total_size)
{
    assert(total_size >= 4);
    std::vector<uint8_t> frame(total_size, 0x00);
    frame[0] = 0xFF;
    frame[1] = 0xD8;
    frame[total_size - 2] = 0xFF;
    frame[total_size - 1] = 0xD9;
    return frame;
}

void Append(std::vector<uint8_t> *target, const std::vector<uint8_t> &value)
{
    target->insert(target->end(), value.begin(), value.end());
}

void CountYield(void *context)
{
    ++(*static_cast<size_t *>(context));
}

void TestRejectedCandidateIsConsumed()
{
    std::vector<uint8_t> stream = {0x10, 0x20};
    Append(&stream, MakeCandidate(99));
    stream.push_back(0x30);
    const size_t valid_offset = stream.size();
    Append(&stream, MakeCandidate(100));

    mjpeg_index_entry_t entries[4] = {};
    size_t count = 99;
    const auto result = mjpeg_index_reader_build(
        stream.data(), stream.size(), entries, 4, &count, nullptr, nullptr);

    assert(result == MJPEG_INDEX_READER_OK);
    assert(count == 1);
    assert(entries[0].offset == valid_offset);
    assert(entries[0].size == 100);
}

void TestOversizedCandidateIsConsumed()
{
    std::vector<uint8_t> stream;
    Append(&stream, MakeCandidate(MJPEG_INDEX_READER_MAX_FRAME_BYTES + 1));
    const size_t valid_offset = stream.size();
    Append(&stream, MakeCandidate(100));

    mjpeg_index_entry_t entries[2] = {};
    size_t count = 0;
    assert(mjpeg_index_reader_build(
               stream.data(), stream.size(), entries, 2, &count, nullptr, nullptr) ==
           MJPEG_INDEX_READER_OK);
    assert(count == 1);
    assert(entries[0].offset == valid_offset);
}

void TestTruncatedTailKeepsEarlierEntries()
{
    std::vector<uint8_t> stream = MakeCandidate(100);
    stream.insert(stream.end(), {0xFF, 0xD8, 0x01, 0x02, 0x03});

    mjpeg_index_entry_t entries[2] = {};
    size_t count = 0;
    assert(mjpeg_index_reader_build(
               stream.data(), stream.size(), entries, 2, &count, nullptr, nullptr) ==
           MJPEG_INDEX_READER_OK);
    assert(count == 1);
    assert(entries[0].offset == 0);
    assert(entries[0].size == 100);
}

void TestYieldCountsCompleteCandidates()
{
    std::vector<uint8_t> stream;
    for (size_t i = 0; i < 8; ++i) {
        Append(&stream, MakeCandidate(4));
    }

    mjpeg_index_entry_t entries[1] = {};
    size_t count = 0;
    size_t yields = 0;
    assert(mjpeg_index_reader_build(
               stream.data(), stream.size(), entries, 1, &count, CountYield, &yields) ==
           MJPEG_INDEX_READER_OK);
    assert(count == 0);
    assert(yields == 1);
}

void TestHardFrameCap()
{
    std::vector<uint8_t> stream;
    for (size_t i = 0; i < MJPEG_INDEX_READER_MAX_FRAMES + 1; ++i) {
        Append(&stream, MakeCandidate(100));
    }

    mjpeg_index_entry_t entries[MJPEG_INDEX_READER_MAX_FRAMES + 8] = {};
    size_t count = 0;
    assert(mjpeg_index_reader_build(
               stream.data(), stream.size(), entries,
               MJPEG_INDEX_READER_MAX_FRAMES + 8, &count, nullptr, nullptr) ==
           MJPEG_INDEX_READER_OK);
    assert(count == MJPEG_INDEX_READER_MAX_FRAMES);
}

void TestLegacyRollbackMatchesNewReader()
{
    std::vector<uint8_t> stream = {0x01, 0x02};
    Append(&stream, MakeCandidate(4));
    Append(&stream, MakeCandidate(100));
    Append(&stream, MakeCandidate(MJPEG_INDEX_READER_MAX_FRAME_BYTES + 1));
    Append(&stream, MakeCandidate(140));
    stream.insert(stream.end(), {0xFF, 0xD8, 0x03});

    mjpeg_index_entry_t current[8] = {};
    mjpeg_index_entry_t legacy[8] = {};
    size_t current_count = 0;
    size_t legacy_count = 0;
    size_t current_yields = 0;
    size_t legacy_yields = 0;

    assert(mjpeg_index_reader_build(
               stream.data(), stream.size(), current, 8, &current_count,
               CountYield, &current_yields) == MJPEG_INDEX_READER_OK);
    assert(mjpeg_index_reader_build_legacy(
               stream.data(), stream.size(), legacy, 8, &legacy_count,
               CountYield, &legacy_yields) == MJPEG_INDEX_READER_OK);
    assert(current_count == legacy_count);
    assert(current_yields == legacy_yields);
    for (size_t i = 0; i < current_count; ++i) {
        assert(current[i].offset == legacy[i].offset);
        assert(current[i].size == legacy[i].size);
    }
}

void TestInvalidArguments()
{
    const uint8_t buffer[] = {0x00};
    mjpeg_index_entry_t entries[1] = {};
    size_t count = 0;

    assert(mjpeg_index_reader_build(
               nullptr, 0, entries, 1, &count, nullptr, nullptr) ==
           MJPEG_INDEX_READER_INVALID_ARGUMENT);
    assert(mjpeg_index_reader_build(
               buffer, sizeof(buffer), nullptr, 1, &count, nullptr, nullptr) ==
           MJPEG_INDEX_READER_INVALID_ARGUMENT);
    assert(mjpeg_index_reader_build(
               buffer, sizeof(buffer), entries, 0, &count, nullptr, nullptr) ==
           MJPEG_INDEX_READER_INVALID_ARGUMENT);
    assert(mjpeg_index_reader_build(
               buffer, sizeof(buffer), entries, 1, nullptr, nullptr, nullptr) ==
           MJPEG_INDEX_READER_INVALID_ARGUMENT);
}

}  // namespace

int main()
{
    TestRejectedCandidateIsConsumed();
    TestOversizedCandidateIsConsumed();
    TestTruncatedTailKeepsEarlierEntries();
    TestYieldCountsCompleteCandidates();
    TestHardFrameCap();
    TestLegacyRollbackMatchesNewReader();
    TestInvalidArguments();
    std::cout << "MJPEG_INDEX_READER_TEST PASS\n";
    return 0;
}
