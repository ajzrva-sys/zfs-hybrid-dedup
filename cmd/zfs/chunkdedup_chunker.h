/* Copyright (c) Microsoft Corporation. Licensed under the MIT License.
 * Portable adaptation of pinned BuildXL RegressionChunking/DeterministicChunker.
 */
#ifndef CHUNKDEDUP_CHUNKER_H
#define CHUNKDEDUP_CHUNKER_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define CHUNKDEDUP_CHUNK_MIN 32768u
#define CHUNKDEDUP_CHUNK_AVG 65536u
#define CHUNKDEDUP_CHUNK_MAX 131072u
#define CHUNKDEDUP_PUSH_SIZE 1048576u
struct chunkdedup_chunk {
    uint64_t offset;
    uint32_t length;
    uint8_t hash[32]; /* first 32 bytes of SHA-512, never SHA-512/256 */
    uint32_t cut_type; /* BuildXL DedupChunkCutType numeric value */
};
/* Callback bytes remain valid only until the callback returns. Return an errno
 * value to abort; a failed session cannot subsequently be resumed. */
typedef int (*chunkdedup_chunk_callback)(void *, const struct chunkdedup_chunk *,
    const uint8_t *bytes);
struct chunkdedup_chunker;
int chunkdedup_chunker_create(struct chunkdedup_chunker **,
    chunkdedup_chunk_callback, void *);
int chunkdedup_chunker_push(struct chunkdedup_chunker *, const void *, size_t);
int chunkdedup_chunker_finish(struct chunkdedup_chunker *);
void chunkdedup_chunker_destroy(struct chunkdedup_chunker *);
/* A single <=1MiB EOF buffer, equivalent to RegressionChunking + Complete.
 * Streaming consumers must use the deterministic session API above. */
int chunkdedup_regression_chunks(const uint8_t *, size_t,
    struct chunkdedup_chunk *, size_t capacity, size_t *count);
#ifdef __cplusplus
}
#endif
#endif
