#!/usr/bin/env python3
"""Compile actual BP inspection/helper bodies and check prefetch isolation."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--repo', type=Path, required=True)
p.add_argument('--dmu', type=Path, help='optional predecessor DMU negative control')
p.add_argument('--ioctl', type=Path, help='optional predecessor inspect negative control')
a = p.parse_args()
repo = a.repo.resolve()
dmu = (a.dmu or repo / 'module/zfs/dmu.c').read_text()
ioctl = (a.ioctl or repo / 'module/zfs/zfs_ioctl.c').read_text()
header = (repo / 'include/sys/dmu.h').read_text()


def function(source, name):
    matches = re.findall(r'^(?:static )?int\n' + re.escape(name) +
                         r'\([^{}]*\)\n\{.*?^\}', source, re.M | re.S)
    if len(matches) != 1:
        raise SystemExit('FAIL: expected one actual source function: ' + name)
    return matches[0]


flags = []
for name in ('DMU_READ_PREFETCH', 'DMU_READ_NO_PREFETCH'):
    value = re.search(r'\b' + name + r'\s*=\s*([^,]+),', header)
    if value is None:
        raise SystemExit('FAIL: missing actual flag definition: ' + name)
    flags.append(name + '=' + value[1])

prelude = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
typedef int dmu_flags_t;
typedef struct { int unused; } dmu_buf_t;
typedef struct { uint64_t birth, identity; int metadata, hole; } blkptr_t;
typedef struct { void *os_spa; } objset_t;
typedef struct { struct { struct { int dr_brtwrite;
    blkptr_t dr_overridden_by; } dl; } dt; } dbuf_dirty_record_t;
typedef struct { dbuf_dirty_record_t *head; } list_t;
typedef struct { dmu_buf_t db; int db_mtx; list_t db_dirty_records;
    blkptr_t *db_blkptr; } dmu_buf_impl_t;
typedef struct { objset_t *z_os; } zfsvfs_t;
typedef struct { zfsvfs_t *fs; uint64_t z_blksz, z_size, z_id; } znode_t;
#define ZTOZSB(zp) ((zp)->fs)
#define FALSE 0
#define FTAG NULL
#define SPA_MAXBLOCKSIZE (16U << 20)
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define ISP2(a) (((a) & ((a)-1)) == 0)
#define SET_ERROR(e) (e)
#define ASSERT3U(a,op,b) assert((uint64_t)(a) op (uint64_t)(b))
#define mutex_enter(m) ((void)(m))
#define mutex_exit(m) ((void)(m))
#define BP_ZERO(bp) memset((bp), 0, sizeof(*(bp)))
#define BP_IS_METADATA(bp) ((bp)->metadata)
#define BP_IS_HOLE(bp) ((bp)->hole)
#define BP_GET_PHYSICAL_BIRTH(bp) ((bp)->birth)
static int list_is_empty(list_t *list) { return list->head == NULL; }
static void *list_head(list_t *list) { return list->head; }
static uint64_t spa_freeze_txg(void *spa) { return UINT64_MAX; }
static uint64_t spa_last_synced_txg(void *spa) { return 10; }
static dmu_buf_impl_t db;
static dmu_buf_t *buffers[] = { &db.db };
static int observed_flags, hold_error, releases;
static int dmu_buf_hold_array(objset_t *os, uint64_t object, uint64_t offset,
    uint64_t length, int read, const void *tag, int *count, dmu_buf_t ***out,
    dmu_flags_t flags) {
    assert(!read && length == 131072 && offset == 0);
    observed_flags = flags; *count = 1; *out = buffers; return hold_error;
}
static void dmu_buf_rele_array(dmu_buf_t **bufs, int count, const void *tag) {
    assert(bufs == buffers && count == 1); releases++;
}
'''
prototypes = r'''
int dmu_read_l0_bps(objset_t *, uint64_t, uint64_t, uint64_t, blkptr_t *, size_t *);
int dmu_read_l0_bps_flags(objset_t *, uint64_t, uint64_t, uint64_t,
    blkptr_t *, size_t *, dmu_flags_t);
'''
checks = r'''
int main(void) {
    objset_t os = {0}; zfsvfs_t fs = { &os };
    znode_t file = { &fs, 131072, 131072, 7 };
    blkptr_t original = { .birth = 1, .identity = 123 }, result;
    db.db_blkptr = &original;
    size_t count = 1;
    assert(!dmu_read_l0_bps(&os, 7, 0, 131072, &result, &count));
    assert(observed_flags == DMU_READ_PREFETCH && result.identity == 123);
    assert(!cd_native_bp(&file, 0, &result));
    assert(observed_flags == DMU_READ_NO_PREFETCH && result.identity == 123);
    assert(releases == 2);
    hold_error = ESRCH;
    assert(cd_native_bp(&file, 0, &result) == ENXIO && releases == 2);
    hold_error = 0;
    dbuf_dirty_record_t dirty = {0}; db.db_dirty_records.head = &dirty;
    assert(cd_native_bp(&file, 0, &result) == EAGAIN && releases == 3);
    db.db_dirty_records.head = NULL; original.birth = 11;
    assert(cd_native_bp(&file, 0, &result) == EAGAIN && releases == 4);
    db.db_blkptr = NULL; memset(&result, 0xa5, sizeof(result));
    assert(!cd_native_bp(&file, 0, &result));
    blkptr_t zero = {0}; assert(!memcmp(&result, &zero, sizeof(result)));
    assert(releases == 5);
    puts("PASS: actual inspection suppresses prefetch; original wrapper and BP checks preserved");
    return 0;
}
'''
source = prelude + '\nenum {' + ','.join(flags) + '};\n' + prototypes
source += function(dmu, 'dmu_read_l0_bps_flags') + '\n'
source += function(dmu, 'dmu_read_l0_bps') + '\n'
source += function(ioctl, 'cd_native_bp') + '\n' + checks
with tempfile.TemporaryDirectory(prefix='inspect-prefetch-') as directory:
    directory = Path(directory)
    (directory / 'test.c').write_text(source)
    subprocess.run([os.environ.get('CC', 'clang'), '-std=c11', '-Wall', '-Wextra',
                    '-Werror', '-Wno-unused-parameter', '-fsanitize=address,undefined',
                    '-g', str(directory / 'test.c'), '-o', str(directory / 'test')], check=True)
    subprocess.run([str(directory / 'test')], check=True, timeout=10,
                   env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',
                            UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1'))
