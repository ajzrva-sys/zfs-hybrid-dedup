/* SPDX-License-Identifier: CDDL-1.0 */
#ifndef CHUNKDEDUP_HYBRID_H
#define CHUNKDEDUP_HYBRID_H
/* Qualification builds may override this; normal binaries have no fault path. */
#ifndef CD_RECOVERY_POINT
#define CD_RECOVERY_POINT(name) ((void)0)
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdatomic.h>

enum cd_method { CD_METHOD_AUTO, CD_METHOD_BLOCK, CD_METHOD_CHUNK };

/* Avoid both multiplication overflow and floating point at the 20% boundary. */
static inline enum cd_method
cd_hybrid_choose(uint64_t native_bytes, uint64_t chunk_bytes, int uncertain)
{
	uint64_t limit = (native_bytes / 5) * 4 +
	    ((native_bytes % 5) * 4) / 5;
	return (!uncertain && native_bytes != 0 && chunk_bytes <= limit ?
	    CD_METHOD_CHUNK : CD_METHOD_BLOCK);
}

/* The catalogs contain only numeric fields and hexadecimal keys, never paths. */
int cd_hybrid_sort(FILE *, FILE **, const char *);
int cd_hybrid_sort_parallel(FILE *, FILE **, const char *, unsigned int,
    const atomic_int *);
int cd_hybrid_sort_parallel_to(FILE *, FILE *, const char *, unsigned int,
    const atomic_int *, int);
int cd_hybrid_add(uint64_t *, uint64_t);
int cd_hybrid_alloc(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
    uint64_t *);
#endif
