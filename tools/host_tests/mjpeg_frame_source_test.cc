#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "mjpeg_frame_source.h"

namespace {

int failures = 0;

#define CHECK(condition)                                                         \
    do {                                                                         \
        if (!(condition)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,         \
                         #condition);                                              \
            ++failures;                                                           \
        }                                                                         \
    } while (0)

mjpeg_frame_source_view_t MakeSource(
    const std::vector<uint8_t>& data,
    const mjpeg_index_entry_t* entries,
    size_t frame_count)
{
    return {data.data(), data.size(), entries, frame_count, true, true};
}

void TestClaimAdvanceEofAndReset()
{
    const std::vector<uint8_t> data(240, 0x5a);
    const mjpeg_index_entry_t entries[] = {{10, 100}, {120, 100}};
    const auto source = MakeSource(data, entries, 2);
    size_t cursor = 0;
    mjpeg_frame_claim_t claim{};

    CHECK(mjpeg_frame_source_claim_next(&source, &cursor, &claim) ==
          MJPEG_FRAME_SOURCE_OK);
    CHECK(claim.frame_index == 0);
    CHECK(claim.data == data.data() + 10);
    CHECK(claim.size == 100);
    CHECK(cursor == 1);

    CHECK(mjpeg_frame_source_claim_next(&source, &cursor, &claim) ==
          MJPEG_FRAME_SOURCE_OK);
    CHECK(claim.frame_index == 1);
    CHECK(cursor == 2);
    CHECK(mjpeg_frame_source_claim_next(&source, &cursor, &claim) ==
          MJPEG_FRAME_SOURCE_NOT_FOUND);
    CHECK(cursor == 2);

    mjpeg_frame_source_reset(&cursor);
    CHECK(cursor == 0);
}

void TestBadEntryRepeatsWithoutAdvance()
{
    const std::vector<uint8_t> data(100, 0x3c);
    const mjpeg_index_entry_t entries[] = {{98, 10}};
    const auto source = MakeSource(data, entries, 1);
    size_t cursor = 0;
    mjpeg_frame_claim_t claim{};

    CHECK(mjpeg_frame_source_claim_next(&source, &cursor, &claim) ==
          MJPEG_FRAME_SOURCE_INVALID_SIZE);
    CHECK(cursor == 0);
    CHECK(mjpeg_frame_source_claim_next(&source, &cursor, &claim) ==
          MJPEG_FRAME_SOURCE_INVALID_SIZE);
    CHECK(cursor == 0);
}

void TestSeekWrapsAndOverwritesCursor()
{
    const std::vector<uint8_t> data(300, 0x7e);
    const mjpeg_index_entry_t entries[] = {{0, 100}, {100, 100}, {200, 100}};
    const auto source = MakeSource(data, entries, 3);
    size_t cursor = 0;
    mjpeg_frame_claim_t claim{};

    CHECK(mjpeg_frame_source_seek_and_claim(&source, 5, &cursor, &claim) ==
          MJPEG_FRAME_SOURCE_OK);
    CHECK(claim.frame_index == 2);
    CHECK(cursor == 3);
    CHECK(mjpeg_frame_source_seek_and_claim(&source, 3, &cursor, &claim) ==
          MJPEG_FRAME_SOURCE_OK);
    CHECK(claim.frame_index == 0);
    CHECK(cursor == 1);
}

void TestInvalidStateAndArgumentsFailClosed()
{
    const std::vector<uint8_t> data(100, 0x42);
    const mjpeg_index_entry_t entries[] = {{0, 100}};
    auto source = MakeSource(data, entries, 1);
    size_t cursor = 0;
    mjpeg_frame_claim_t claim{};

    CHECK(mjpeg_frame_source_claim_next(nullptr, &cursor, &claim) ==
          MJPEG_FRAME_SOURCE_INVALID_ARGUMENT);
    CHECK(mjpeg_frame_source_claim_next(&source, nullptr, &claim) ==
          MJPEG_FRAME_SOURCE_INVALID_ARGUMENT);
    source.ready = false;
    CHECK(mjpeg_frame_source_claim_next(&source, &cursor, &claim) ==
          MJPEG_FRAME_SOURCE_INVALID_STATE);
    source.ready = true;
    source.index_built = false;
    CHECK(mjpeg_frame_source_claim_next(&source, &cursor, &claim) ==
          MJPEG_FRAME_SOURCE_INVALID_STATE);
    source.index_built = true;
    source.frame_count = 0;
    CHECK(mjpeg_frame_source_seek_and_claim(&source, 0, &cursor, &claim) ==
          MJPEG_FRAME_SOURCE_INVALID_STATE);
}

void TestNewAndLegacyAreDifferentiallyEquivalent()
{
    const std::vector<uint8_t> data(300, 0x18);
    const mjpeg_index_entry_t entries[] = {{5, 100}, {290, 20}};
    const auto source = MakeSource(data, entries, 2);
    size_t new_cursor = 0;
    size_t old_cursor = 0;

    for (int i = 0; i < 3; ++i) {
        mjpeg_frame_claim_t candidate{};
        mjpeg_frame_claim_t legacy{};
        const auto candidate_result =
            mjpeg_frame_source_claim_next(&source, &new_cursor, &candidate);
        const auto legacy_result =
            mjpeg_frame_source_claim_next_legacy(&source, &old_cursor, &legacy);
        CHECK(candidate_result == legacy_result);
        CHECK(new_cursor == old_cursor);
        if (candidate_result == MJPEG_FRAME_SOURCE_OK) {
            CHECK(candidate.data == legacy.data);
            CHECK(candidate.size == legacy.size);
            CHECK(candidate.offset == legacy.offset);
            CHECK(candidate.frame_index == legacy.frame_index);
        }
    }
}

}  // namespace

int main()
{
    TestClaimAdvanceEofAndReset();
    TestBadEntryRepeatsWithoutAdvance();
    TestSeekWrapsAndOverwritesCursor();
    TestInvalidStateAndArgumentsFailClosed();
    TestNewAndLegacyAreDifferentiallyEquivalent();
    if (failures != 0) {
        std::fprintf(stderr, "mjpeg_frame_source_test: %d failure(s)\n", failures);
        return 1;
    }
    std::puts("mjpeg_frame_source_test: PASS");
    return 0;
}
