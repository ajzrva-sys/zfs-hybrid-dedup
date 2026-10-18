/* Copyright (c) Microsoft Corporation. Licensed under the MIT License.
 * Portable adaptation of BuildXL commit
 * 852a00e5b45ef8a8d06a2eb9040495c36243078f.
 * RegressionChunking is used by ManagedChunker only for one contiguous EOF
 * buffer. This port retains that contract and exposes streaming exclusively
 * through the exact 1MiB DeterministicChunker discard-last/rechunk protocol.
 * Indexes replace C# unsafe pointers, avoiding pointers before an allocation.
 */
#include "chunkdedup_chunker.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/evp.h>
#endif
#include "rabin64_table.h"

struct chunkdedup_chunker {
    uint8_t *buffer;
    size_t used;
    uint64_t baseline;
    chunkdedup_chunk_callback callback;
    void *argument;
    int error;
    int finished;
};

static int
chunk_hash(const uint8_t *bytes, size_t length, uint8_t hash[32])
{
    uint8_t digest[64];
#ifdef __APPLE__
    if (length > UINT_MAX || CC_SHA512(bytes, (CC_LONG)length, digest) == NULL)
        return EIO;
#else
    unsigned int size = 0;
    if (!EVP_Digest(bytes, length, digest, &size, EVP_sha512(), NULL) || size != 64)
        return EIO;
#endif
    memcpy(hash, digest, 32);
    return 0;
}

static int
add_chunk(const uint8_t *data, struct chunkdedup_chunk *chunks, size_t capacity,
    size_t *count, uint64_t offset, uint64_t length, uint32_t type)
{
    if (length == 0)
        return 0;
    if (*count == capacity)
        return EOVERFLOW;
    struct chunkdedup_chunk *chunk = &chunks[(*count)++];
    chunk->offset = offset;
    chunk->length = (uint32_t)length;
    chunk->cut_type = type;
    return chunk_hash(data + offset, (size_t)length, chunk->hash);
}

static uint64_t
append_rabin(uint64_t hash, uint8_t byte)
{
    return (hash << 8) ^ byte ^ g_arrPolynomialsTD[hash >> 56];
}

int
chunkdedup_regression_chunks(const uint8_t *data, size_t size,
    struct chunkdedup_chunk *chunks, size_t capacity, size_t *count)
{
    const int64_t minimum = CHUNKDEDUP_CHUNK_MIN;
    const int64_t maximum = CHUNKDEDUP_CHUNK_MAX;
    const uint32_t masks[4] = {65535,32767,16383,8191};
    const uint32_t matches[4] = {0x5555,0x5555,0x1555,0x1555};
    const uint32_t smallest_mask = 4095, smallest_match = 0x555;
    int64_t regress[4] = {-1,-1,-1,-1};
    int64_t start = 0, previous = 0, zero_run = 0;
    uint64_t offset = 0, hash = 0;
    int error;
    if (count == NULL || chunks == NULL || (data == NULL && size != 0) ||
        size > CHUNKDEDUP_PUSH_SIZE)
        return EINVAL;
    *count = 0;
    if (size < 16)
        return add_chunk(data, chunks, capacity, count, 0, size, 5);

    while (start < (int64_t)size) {
        int64_t available = (int64_t)size - start + previous;
        if (available < minimum) {
            if (zero_run >= 0) {
                int64_t scan = start;
                while (scan < (int64_t)size && data[scan] == 0)
                    scan++;
                zero_run = scan == (int64_t)size ? available : -1;
            }
            error = add_chunk(data, chunks, capacity, count, offset,
                (uint64_t)available, zero_run >= 0 ? 12 : 2);
            if (error) return error;
            offset += (uint64_t)available;
            start = (int64_t)size;
            previous = 0;
            zero_run = 0;
            break;
        }

        int64_t skip = minimum - previous;
        int64_t end = start + skip;
        int64_t window_start = end - 16;
        if (start + skip > 0) {
            if (zero_run >= 0) {
                int64_t scan = start;
                while (scan != end && data[scan] == 0)
                    scan++;
                zero_run = scan != end ? -1 : zero_run + end - start;
            }
            hash = 0;
            /* At an outer-loop jump previous < minimum and end >=16.
             * Cross-call history is unreachable for the single-buffer API. */
            if (window_start < 0 || end > (int64_t)size)
                return EILSEQ;
            for (int64_t scan = window_start; scan < end; scan++)
                hash = append_rabin(hash, data[scan]);
            for (int i = 0; i < 4; i++)
                regress[i] = -1;
        } else {
            /* ManagedChunker's single-buffer invocation cannot reach this. */
            return EILSEQ;
        }
        int64_t max_end = start + maximum - previous;
        if (max_end > (int64_t)size) max_end = (int64_t)size;

        if (zero_run >= 0) {
            int64_t old_end = end;
            while (end < max_end && data[end] == 0)
                end++;
            zero_run += end - old_end;
            int64_t scanned = zero_run - previous;
            error = add_chunk(data, chunks, capacity, count, offset,
                (uint64_t)zero_run, 12);
            if (error) return error;
            offset += (uint64_t)zero_run;
            start += scanned;
            previous = 0;
            zero_run = 0;
            hash = 0;
            continue;
        }

        for (;;) {
            while (end < max_end && (hash & smallest_mask) != smallest_match) {
                hash ^= g_arrPolynomialsTU[data[window_start]];
                hash = append_rabin(hash, data[end]);
                window_start++;
                end++;
            }
            int64_t processed = end - start;
            int64_t length = processed + previous;
            if ((hash & smallest_mask) == smallest_match) {
                int i;
                for (i = 3; i >= 0; i--) {
                    regress[i] = length;
                    if ((hash & masks[i]) != matches[i])
                        break;
                }
                if (i < 0) {
                    error = add_chunk(data, chunks, capacity, count, offset,
                        (uint64_t)length, 3);
                    if (error) return error;
                    offset += (uint64_t)length;
                    start += processed;
                    previous = 0;
                    zero_run = 0;
                    hash = 0;
                    break;
                }
                if (end < max_end) {
                    hash ^= g_arrPolynomialsTU[data[window_start]];
                    hash = append_rabin(hash, data[end]);
                    window_start++;
                    end++;
                    continue;
                }
            }
            if (length == maximum) {
                int lowest;
                int64_t regressed = length;
                uint32_t type = 4;
                for (lowest = 0; lowest < 4; lowest++)
                    if (regress[lowest] >= 0) break;
                if (lowest < 4) {
                    type = 7 + (uint32_t)lowest;
                    regressed = regress[lowest];
                    regress[lowest] = -1;
                    for (int i = lowest + 1; i < 4; i++) {
                        regress[i] -= regressed;
                        if (regress[i] < minimum) regress[i] = -1;
                    }
                }
                error = add_chunk(data, chunks, capacity, count, offset,
                    (uint64_t)regressed, type);
                if (error) return error;
                offset += (uint64_t)regressed;
                previous = length - regressed;
                start += processed;
                if (previous >= 16) {
                    zero_run = -1;
                } else {
                    int64_t scan = start - previous;
                    while (scan < start && data[scan] == 0) scan++;
                    zero_run = scan == start ? previous : -1;
                }
                if (previous >= minimum) {
                    max_end = start + maximum - previous;
                    if (max_end > (int64_t)size) max_end = (int64_t)size;
                    continue;
                }
                break;
            }
            error = add_chunk(data, chunks, capacity, count, offset,
                (uint64_t)length, 5);
            if (error) return error;
            offset += (uint64_t)length;
            start += processed;
            previous = 0;
            zero_run = 0;
            hash = 0;
            break;
        }
    }
    if (previous > 0) {
        error = add_chunk(data, chunks, capacity, count, offset,
            (uint64_t)previous, zero_run >= 0 ? 12 : previous < minimum ? 2 : 5);
        if (error) return error;
    }
    return 0;
}

/* Minimum chunk size bounds this list to 32 regular chunks plus the tail. */
#define MAX_BUFFER_CHUNKS (CHUNKDEDUP_PUSH_SIZE / CHUNKDEDUP_CHUNK_MIN + 2)
static int
report_buffer(struct chunkdedup_chunker *session, const uint8_t *buffer,
    size_t length, int final, size_t *consumed)
{
    struct chunkdedup_chunk chunks[MAX_BUFFER_CHUNKS];
    size_t count;
    int error = chunkdedup_regression_chunks(buffer, length, chunks,
        MAX_BUFFER_CHUNKS, &count);
    if (error) return error;
    if (!final) {
        if (count < 2) return EILSEQ;
        count--; /* DeterministicChunker does not trust the last chunk. */
    }
    *consumed = 0;
    for (size_t i = 0; i < count; i++) {
        struct chunkdedup_chunk fixed = chunks[i];
        if (fixed.offset > UINT64_MAX - session->baseline ||
            fixed.length > UINT64_MAX - session->baseline - fixed.offset)
            return EOVERFLOW;
        fixed.offset += session->baseline;
        error = session->callback(session->argument, &fixed,
            buffer + chunks[i].offset);
        if (error) return error;
        *consumed = (size_t)chunks[i].offset + chunks[i].length;
    }
    return 0;
}

int
chunkdedup_chunker_create(struct chunkdedup_chunker **result,
    chunkdedup_chunk_callback callback, void *argument)
{
    if (result == NULL || callback == NULL) return EINVAL;
    *result = NULL;
    struct chunkdedup_chunker *session = calloc(1, sizeof (*session));
    if (session == NULL) return ENOMEM;
    session->buffer = malloc(CHUNKDEDUP_PUSH_SIZE);
    if (session->buffer == NULL) { free(session); return ENOMEM; }
    session->callback = callback;
    session->argument = argument;
    *result = session;
    return 0;
}

int
chunkdedup_chunker_push(struct chunkdedup_chunker *session,
    const void *input, size_t length)
{
    if (session == NULL || (input == NULL && length != 0)) return EINVAL;
    if (session->error) return session->error;
    if (session->finished) return EINVAL;
    const uint8_t *bytes = input;
    while (length > 0) {
        size_t consumed;
        int error;
        if (session->used == 0 && length >= CHUNKDEDUP_PUSH_SIZE) {
            error = report_buffer(session, bytes, CHUNKDEDUP_PUSH_SIZE, 0,
                &consumed);
            if (error) return session->error = error;
            session->baseline += consumed;
        } else {
            size_t room = CHUNKDEDUP_PUSH_SIZE - session->used;
            consumed = length < room ? length : room;
            memcpy(session->buffer + session->used, bytes, consumed);
            session->used += consumed;
            if (session->used == CHUNKDEDUP_PUSH_SIZE) {
                size_t chunked;
                error = report_buffer(session, session->buffer,
                    CHUNKDEDUP_PUSH_SIZE, 0, &chunked);
                if (error) return session->error = error;
                session->used -= chunked;
                memmove(session->buffer, session->buffer + chunked,
                    session->used);
                session->baseline += chunked;
            }
        }
        bytes += consumed;
        length -= consumed;
    }
    return 0;
}

int
chunkdedup_chunker_finish(struct chunkdedup_chunker *session)
{
    if (session == NULL) return EINVAL;
    if (session->error) return session->error;
    if (session->finished) return 0;
    size_t consumed;
    int error = report_buffer(session, session->buffer, session->used, 1,
        &consumed);
    if (error) return session->error = error;
    session->baseline += consumed;
    session->used = 0;
    session->finished = 1;
    return 0;
}

void
chunkdedup_chunker_destroy(struct chunkdedup_chunker *session)
{
    if (session == NULL) return;
    free(session->buffer);
    free(session);
}
