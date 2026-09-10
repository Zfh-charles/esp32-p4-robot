#include "mjpeg_index_reader.h"

static size_t find_marker(const uint8_t *buffer, size_t buffer_size,
                          size_t search_offset, uint8_t first, uint8_t second)
{
    if (search_offset >= buffer_size) {
        return buffer_size;
    }

    for (size_t i = search_offset; i + 1 < buffer_size; ++i) {
        if (buffer[i] == first && buffer[i + 1] == second) {
            return i;
        }
    }
    return buffer_size;
}

mjpeg_index_reader_result_t mjpeg_index_reader_build(
    const uint8_t *buffer,
    size_t buffer_size,
    mjpeg_index_entry_t *entries,
    size_t entry_capacity,
    size_t *out_count,
    mjpeg_index_reader_yield_fn yield_fn,
    void *yield_context)
{
    if (buffer == NULL || entries == NULL || out_count == NULL || entry_capacity == 0) {
        return MJPEG_INDEX_READER_INVALID_ARGUMENT;
    }

    const size_t limit = entry_capacity < MJPEG_INDEX_READER_MAX_FRAMES
                             ? entry_capacity
                             : (size_t)MJPEG_INDEX_READER_MAX_FRAMES;
    size_t search_offset = 0;
    size_t accepted_count = 0;
    size_t complete_candidate_count = 0;

    while (search_offset < buffer_size && accepted_count < limit) {
        const size_t frame_start = find_marker(buffer, buffer_size, search_offset, 0xFF, 0xD8);
        if (frame_start == buffer_size) {
            break;
        }

        const size_t frame_end_marker = find_marker(
            buffer, buffer_size, frame_start + 2, 0xFF, 0xD9);
        if (frame_end_marker == buffer_size) {
            break;
        }

        const size_t frame_end = frame_end_marker + 2;
        const size_t frame_size = frame_end - frame_start;
        if (frame_size >= MJPEG_INDEX_READER_MIN_FRAME_BYTES &&
            frame_size <= MJPEG_INDEX_READER_MAX_FRAME_BYTES) {
            entries[accepted_count].offset = frame_start;
            entries[accepted_count].size = frame_size;
            ++accepted_count;
        }

        search_offset = frame_end;
        ++complete_candidate_count;
        if (yield_fn != NULL &&
            complete_candidate_count % MJPEG_INDEX_READER_YIELD_INTERVAL == 0) {
            yield_fn(yield_context);
        }
    }

    *out_count = accepted_count;
    return MJPEG_INDEX_READER_OK;
}

mjpeg_index_reader_result_t mjpeg_index_reader_build_legacy(
    const uint8_t *buffer,
    size_t buffer_size,
    mjpeg_index_entry_t *entries,
    size_t entry_capacity,
    size_t *out_count,
    mjpeg_index_reader_yield_fn yield_fn,
    void *yield_context)
{
    if (buffer == NULL || entries == NULL || out_count == NULL || entry_capacity == 0) {
        return MJPEG_INDEX_READER_INVALID_ARGUMENT;
    }

    const size_t limit = entry_capacity < MJPEG_INDEX_READER_MAX_FRAMES
                             ? entry_capacity
                             : (size_t)MJPEG_INDEX_READER_MAX_FRAMES;
    size_t search_pos = 0;
    size_t frame_count = 0;
    size_t yield_budget = 0;

    while (search_pos < buffer_size && frame_count < limit) {
        const size_t frame_start = find_marker(buffer, buffer_size, search_pos, 0xFF, 0xD8);
        if (frame_start == buffer_size) {
            break;
        }

        const size_t frame_end_marker = find_marker(
            buffer, buffer_size, frame_start + 2, 0xFF, 0xD9);
        if (frame_end_marker == buffer_size) {
            break;
        }

        const size_t frame_end = frame_end_marker + 2;
        const size_t frame_size = frame_end - frame_start;
        if (frame_size >= MJPEG_INDEX_READER_MIN_FRAME_BYTES &&
            frame_size <= MJPEG_INDEX_READER_MAX_FRAME_BYTES) {
            entries[frame_count].offset = frame_start;
            entries[frame_count].size = frame_size;
            ++frame_count;
        }

        search_pos = frame_end;
        if (++yield_budget >= MJPEG_INDEX_READER_YIELD_INTERVAL) {
            yield_budget = 0;
            if (yield_fn != NULL) {
                yield_fn(yield_context);
            }
        }
    }

    *out_count = frame_count;
    return MJPEG_INDEX_READER_OK;
}
