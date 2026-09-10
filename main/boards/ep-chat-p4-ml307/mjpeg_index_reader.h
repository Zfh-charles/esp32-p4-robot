#ifndef EP_CHAT_P4_MJPEG_INDEX_READER_H_
#define EP_CHAT_P4_MJPEG_INDEX_READER_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    MJPEG_INDEX_READER_MAX_FRAMES = 300,
    MJPEG_INDEX_READER_MIN_FRAME_BYTES = 100,
    MJPEG_INDEX_READER_MAX_FRAME_BYTES = 2 * 1024 * 1024,
    MJPEG_INDEX_READER_YIELD_INTERVAL = 8,
};

typedef struct {
    size_t offset;
    size_t size;
} mjpeg_index_entry_t;

typedef void (*mjpeg_index_reader_yield_fn)(void *context);

typedef enum {
    MJPEG_INDEX_READER_OK = 0,
    MJPEG_INDEX_READER_INVALID_ARGUMENT = -1,
} mjpeg_index_reader_result_t;

/**
 * Build an in-memory MJPEG frame index without allocating or taking locks.
 *
 * The reader deliberately preserves the legacy scanner's semantics: the first
 * SOI is paired with the first following EOI, rejected complete candidates are
 * consumed, a truncated tail keeps earlier entries, and the result may contain
 * zero frames. Capacity is clamped to MJPEG_INDEX_READER_MAX_FRAMES.
 */
mjpeg_index_reader_result_t mjpeg_index_reader_build(
    const uint8_t *buffer,
    size_t buffer_size,
    mjpeg_index_entry_t *entries,
    size_t entry_capacity,
    size_t *out_count,
    mjpeg_index_reader_yield_fn yield_fn,
    void *yield_context);

/** Compile-time R0 implementation retained until the G5 deletion gate. */
mjpeg_index_reader_result_t mjpeg_index_reader_build_legacy(
    const uint8_t *buffer,
    size_t buffer_size,
    mjpeg_index_entry_t *entries,
    size_t entry_capacity,
    size_t *out_count,
    mjpeg_index_reader_yield_fn yield_fn,
    void *yield_context);

#ifdef __cplusplus
}
#endif

#endif  // EP_CHAT_P4_MJPEG_INDEX_READER_H_
