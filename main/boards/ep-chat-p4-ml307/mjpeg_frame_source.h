/* G4-2 pure frame-claim component; caller retains cache and lock ownership. */
#ifndef EP_CHAT_P4_MJPEG_FRAME_SOURCE_H_
#define EP_CHAT_P4_MJPEG_FRAME_SOURCE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mjpeg_index_reader.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const uint8_t *buffer;
    size_t buffer_size;
    const mjpeg_index_entry_t *entries;
    size_t frame_count;
    bool ready;
    bool index_built;
} mjpeg_frame_source_view_t;

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t offset;
    size_t frame_index;
} mjpeg_frame_claim_t;

typedef enum {
    MJPEG_FRAME_SOURCE_OK = 0,
    MJPEG_FRAME_SOURCE_INVALID_ARGUMENT = -1,
    MJPEG_FRAME_SOURCE_INVALID_STATE = -2,
    MJPEG_FRAME_SOURCE_NOT_FOUND = -3,
    MJPEG_FRAME_SOURCE_INVALID_SIZE = -4,
} mjpeg_frame_source_result_t;

/**
 * Claim the current indexed frame and advance the cursor exactly once.
 *
 * The caller retains cache lifetime and lock ownership. A valid claim advances
 * before decode; EOF and invalid entries leave the cursor unchanged.
 */
mjpeg_frame_source_result_t mjpeg_frame_source_claim_next(
    const mjpeg_frame_source_view_t *source,
    size_t *cursor,
    mjpeg_frame_claim_t *out_claim);

void mjpeg_frame_source_reset(size_t *cursor);

mjpeg_frame_source_result_t mjpeg_frame_source_seek_and_claim(
    const mjpeg_frame_source_view_t *source,
    size_t requested_index,
    size_t *cursor,
    mjpeg_frame_claim_t *out_claim);

/** Independent R0 oracle retained until the G5 deletion gate. */
mjpeg_frame_source_result_t mjpeg_frame_source_claim_next_legacy(
    const mjpeg_frame_source_view_t *source,
    size_t *cursor,
    mjpeg_frame_claim_t *out_claim);

#ifdef __cplusplus
}
#endif

#endif  // EP_CHAT_P4_MJPEG_FRAME_SOURCE_H_
