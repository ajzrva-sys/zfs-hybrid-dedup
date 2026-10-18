#!/usr/bin/env python3
"""Exercise the actual CLI batch functions with real streaming chunk boundaries.

Only the kernel ioctl and nvlist transport are mocked. No filesystem or VM
actions occur; the test compiles extracted CLI functions in a temporary folder.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve()
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--openzfs', type=Path, help='OpenZFS tree with the prototype patch applied')
parser.add_argument('--chunker', type=Path, help='directory containing chunkdedup_chunker.c')
options = parser.parse_args()
roots = [HERE.parents[2], HERE.parents[1]]
OPENZFS = options.openzfs or next((base / 'openzfs' for base in roots
                                  if (base / 'openzfs/cmd/zfs/zfs_chunkdedup_user.c').is_file()), None)
CHUNKER = options.chunker or next((base / 'chunker' for base in roots
                                  if (base / 'chunker/chunkdedup_chunker.c').is_file()), None)
if OPENZFS is None or CHUNKER is None:
    parser.error('provide --openzfs PATH and --chunker PATH, or use adjacent source trees')
OPENZFS = OPENZFS.resolve()
CHUNKER = CHUNKER.resolve()
SOURCE = OPENZFS / 'cmd/zfs/zfs_chunkdedup_user.c'
if not SOURCE.is_file() or not (CHUNKER / 'chunkdedup_chunker.c').is_file():
    parser.error('source files are missing from the supplied directories')
temporary_root = next((base for base in roots if base.name == 'work'), None)


def function(source, name):
    start = source.index('static int\n' + name + '(')
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


PREAMBLE = r'''
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "chunkdedup_chunker.h"
typedef unsigned int uint_t;
typedef int boolean_t;
#define B_TRUE 1
#define B_FALSE 0
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define ZFS_CHUNKDEDUP_MAX_RANGE 1048576U
#define ZFS_CHUNKDEDUP_MAX_CHUNKS 64U
static volatile sig_atomic_t cd_stop;
typedef struct nvlist {
    uint64_t offset, length, *offsets, *lengths;
    uint_t count, length_count, hashes;
} nvlist_t;
static unsigned attempts, successes, fail_at;
static uint64_t installed, last_length;
static boolean_t enabled = B_TRUE, already;
static nvlist_t *fnvlist_alloc(void) { return calloc(1, sizeof(nvlist_t)); }
static void fnvlist_free(nvlist_t *args) { free(args); }
static void fnvlist_add_string(nvlist_t *args, const char *key, const char *value)
{ (void)args; (void)key; (void)value; }
static void fnvlist_add_uint64(nvlist_t *args, const char *key, uint64_t value)
{
    if (!strcmp(key, "offset")) args->offset = value;
    if (!strcmp(key, "length")) args->length = value;
}
static void fnvlist_add_uint64_array(nvlist_t *args, const char *key,
    uint64_t *values, uint_t count)
{
    if (!strcmp(key, "chunk_offsets")) { args->offsets = values; args->count = count; }
    else { assert(!strcmp(key, "chunk_lengths")); args->lengths = values; args->length_count = count; }
}
static void fnvlist_add_uint8_array(nvlist_t *args, const char *key,
    uint8_t *values, uint_t count)
{ (void)values; assert(!strcmp(key, "chunk_hashes")); args->hashes = count; }
static int cd_enabled(const char *dataset, boolean_t *value)
{ (void)dataset; *value = enabled; return 0; }
static int lzc_chunkdedup(const char *dataset, nvlist_t *args, void *out)
{
    (void)dataset; (void)out;
    attempts++;
    assert(args->offset == installed && args->length > 0);
    assert(args->length <= ZFS_CHUNKDEDUP_MAX_RANGE);
    assert(args->count > 0 && args->count == args->length_count);
    assert(args->hashes == 32 * args->count);
    uint64_t covered = args->offsets[0];
    assert(covered <= args->offset);
    for (uint_t i = 0; i < args->count; i++) {
        assert(args->offsets[i] == covered && args->lengths[i] > 0);
        covered += args->lengths[i];
    }
    assert(covered >= args->offset + args->length);
    if (attempts == fail_at) return EIO;
    successes++;
    installed += args->length;
    last_length = args->length;
    return already ? EALREADY : 0;
}
'''

TESTS = r'''
static void reset(void)
{
    attempts = successes = fail_at = 0;
    installed = last_length = 0;
    cd_stop = already = 0;
    enabled = B_TRUE;
}
static void run(size_t size, uint32_t seed, int zeros)
{
    reset();
    struct cd_worker worker = { .initial.st_size = (off_t)size };
    struct chunkdedup_chunker *chunker;
    assert(!chunkdedup_chunker_create(&chunker, cd_found, &worker));
    uint8_t buffer[262144];
    for (size_t offset = 0; offset < size;) {
        size_t length = MIN(sizeof(buffer), size - offset);
        for (size_t i = 0; i < length; i++) {
            seed = seed * 1664525U + 1013904223U;
            buffer[i] = zeros ? 0 : (uint8_t)(seed >> 24);
        }
        assert(!chunkdedup_chunker_push(chunker, buffer, length));
        offset += length;
    }
    assert(!chunkdedup_chunker_finish(chunker));
    assert(worker.position == size && installed == size && worker.count == 0);
    unsigned batches = (unsigned)((size + ZFS_CHUNKDEDUP_MAX_RANGE - 1) /
        ZFS_CHUNKDEDUP_MAX_RANGE);
    assert(worker.batches == batches && successes == batches);
    assert(last_length == (size % ZFS_CHUNKDEDUP_MAX_RANGE ?
        size % ZFS_CHUNKDEDUP_MAX_RANGE : ZFS_CHUNKDEDUP_MAX_RANGE));
    chunkdedup_chunker_destroy(chunker);
}
int main(void)
{
    const size_t sizes[] = { 1, 120, 4097, 131071, 1048576, 1048577,
        1048699, 2097275, 16777339 };
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        run(sizes[i], 451, 0);
        run(sizes[i], 17, 1);
    }
    reset();
    struct cd_worker worker = { .initial.st_size = 1048699 };
    struct chunkdedup_chunk chunk = { .offset = 0, .length = 65535 };
    assert(!cd_found(&worker, &chunk, NULL));
    for (uint64_t off = 65535; off < 983039; off += 131072) {
        chunk.offset = off; chunk.length = 131072;
        assert(!cd_found(&worker, &chunk, NULL));
    }
    chunk.offset = 983039; chunk.length = 65660;
    assert(!cd_found(&worker, &chunk, NULL));
    assert(successes == 2 && installed == 1048699 && last_length == 123);
    reset();
    worker = (struct cd_worker){ .initial.st_size = 1048699 };
    already = B_TRUE;
    chunk.offset = 0; chunk.length = 1048699;
    assert(!cd_found(&worker, &chunk, NULL));
    assert(successes == 2 && worker.position == 1048699);
    reset();
    worker = (struct cd_worker){ .initial.st_size = 1048699 };
    fail_at = 2;
    assert(cd_found(&worker, &chunk, NULL) == EIO);
    assert(attempts == 2 && successes == 1 && worker.position == 1048576);
    reset(); cd_stop = 1;
    assert(cd_found(&worker, &chunk, NULL) == ECANCELED && !attempts);
    reset();
    worker = (struct cd_worker){ .initial.st_size = 1048576, .count = 1 };
    worker.lengths[0] = 123;
    assert(cd_submit(&worker) == EPROTO && !attempts);
    worker.lengths[0] = 1048576; worker.watch = B_TRUE; enabled = B_FALSE;
    assert(cd_submit(&worker) == ECANCELED && !attempts);
    puts("actual CLI batch functions: streaming boundaries, EOF tails, overlap, errors and cancellation PASS");
    return 0;
}
'''

source = SOURCE.read_text()
start = source.index('struct cd_worker {')
end = source.index('};', start) + 2
unit = PREAMBLE + source[start:end]
unit += '\nstatic void cd_identity(nvlist_t *args, const struct stat *st) '
unit += '{ (void)args; (void)st; }\n'
unit += function(source, 'cd_submit') + '\n' + function(source, 'cd_found')
unit += TESTS
with tempfile.TemporaryDirectory(prefix='worker-batches-', dir=temporary_root) as directory:
    path = Path(directory)
    (path / 'test.c').write_text(unit)
    args = [os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
            '-fsanitize=address,undefined', '-g', '-I' + str(CHUNKER),
            str(path / 'test.c'), str(CHUNKER / 'chunkdedup_chunker.c'),
            '-o', str(path / 'test')]
    if os.uname().sysname != 'Darwin':
        args.append('-lcrypto')
    subprocess.run(args, check=True)
    subprocess.run([str(path / 'test')], check=True)
