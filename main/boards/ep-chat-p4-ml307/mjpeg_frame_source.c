/* G4-2 pure frame-claim component; no heap, locks, RTOS, decoder or present. */
#include "mjpeg_frame_source.h"

static mjpeg_frame_source_result_t validate_source(
    const mjpeg_frame_source_view_t *source,
    const size_t *cursor,
    const mjpeg_frame_claim_t *out_claim)
{
    if (source == NULL || cursor == NULL || out_claim == NULL) {
        return MJPEG_FRAME_SOURCE_INVALID_ARGUMENT;
    }
    if (!source->ready || !source->index_built || source->buffer == NULL ||
        source->buffer_size == 0 ||
        (source->frame_count > 0 && source->entries == NULL)) {
        return MJPEG_FRAME_SOURCE_INVALID_STATE;
    }
    return MJPEG_FRAME_SOURCE_OK;
}

mjpeg_frame_source_result_t mjpeg_frame_source_claim_next(
    const mjpeg_frame_source_view_t *source,
    size_t *cursor,
    mjpeg_frame_claim_t *out_claim)
{
    const mjpeg_frame_source_result_t validation =
        validate_source(source, cursor, out_claim);
    if (validation != MJPEG_FRAME_SOURCE_OK) {
        return validation;
    }
    if (*cursor >= source->frame_count) {
        return MJPEG_FRAME_SOURCE_NOT_FOUND;
    }

    const size_t frame_index = *cursor;
    const mjpeg_index_entry_t *entry = &source->entries[frame_index];
    if (entry->offset >= source->buffer_size ||
        entry->offset + entry->size > source->buffer_size) {
        return MJPEG_FRAME_SOURCE_INVALID_SIZE;
    }

    out_claim->data = source->buffer + entry->offset;
    out_claim->size = entry->size;
    out_claim->offset = entry->offset;
    out_claim->frame_index = frame_index;
    ++(*cursor);
    return MJPEG_FRAME_SOURCE_OK;
}

void mjpeg_frame_source_reset(size_t *cursor)
{
    if (cursor != NULL) {
        *cursor = 0;
    }
}

mjpeg_frame_source_result_t mjpeg_frame_source_seek_and_claim(
    const mjpeg_frame_source_view_t *source,
    size_t requested_index,
    size_t *cursor,
    mjpeg_frame_claim_t *out_claim)
{
    const mjpeg_frame_source_result_t validation =
        validate_source(source, cursor, out_claim);
    if (validation != MJPEG_FRAME_SOURCE_OK) {
        return validation;
    }
    if (source->frame_count == 0) {
        return MJPEG_FRAME_SOURCE_INVALID_STATE;
    }

    *cursor = requested_index % source->frame_count;
    return mjpeg_frame_source_claim_next(source, cursor, out_claim);
}

mjpeg_frame_source_result_t mjpeg_frame_source_claim_next_legacy(
    const mjpeg_frame_source_view_t *source,
    size_t *cursor,
    mjpeg_frame_claim_t *out_claim)
{
    if (source == NULL || cursor == NULL || out_claim == NULL) {
        return MJPEG_FRAME_SOURCE_INVALID_ARGUMENT;
    }
    if (!source->ready || !source->index_built || source->buffer == NULL ||
        source->buffer_size == 0 ||
        (source->frame_count > 0 && source->entries == NULL)) {
        return MJPEG_FRAME_SOURCE_INVALID_STATE;
    }
    if (*cursor >= source->frame_count) {
        return MJPEG_FRAME_SOURCE_NOT_FOUND;
    }

    const size_t frame_index = *cursor;
    const mjpeg_index_entry_t *entry = &source->entries[frame_index];
    const size_t frame_start_pos = entry->offset;
    const size_t frame_size = entry->size;
    if (frame_start_pos >= source->buffer_size ||
        frame_start_pos + frame_size > source->buffer_size) {
        return MJPEG_FRAME_SOURCE_INVALID_SIZE;
    }

    out_claim->data = source->buffer + frame_start_pos;
    out_claim->size = frame_size;
    out_claim->offset = frame_start_pos;
    out_claim->frame_index = frame_index;
    ++(*cursor);
    return MJPEG_FRAME_SOURCE_OK;
}
