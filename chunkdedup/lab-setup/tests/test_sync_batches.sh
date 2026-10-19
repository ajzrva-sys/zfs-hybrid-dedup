#!/bin/sh
# SPDX-License-Identifier: MIT
# Compile the production queue functions; no filesystem or kernel is emulated.
set -eu
test "$#" -eq 1 || { echo 'usage: test_sync_batches.sh OPENZFS_SOURCE' >&2; exit 2; }
source=$1/cmd/zfs/zfs_chunkdedup_user.c
test -f "$source" || { echo 'production hybrid worker is missing' >&2; exit 2; }
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/zfs-sync-batch-queue.XXXXXX")
trap 'rm -rf -- "$temporary"' EXIT HUP INT TERM
extract()
{
    awk -v name="$1" -v declaration="$2" '
        $0 == declaration { pending = 1; next }
        pending && $0 ~ ("^" name "\\(") {
            active = 1; found++; print declaration
        }
        { pending = 0 }
        active { print; if ($0 == "}") exit }
        END { if (found != 1) exit 1 }
    ' "$source"
}
awk '$1 == "#define" && $2 == "CD_SYNC_BATCH_FILES" { print; found++ }
     END { if (found != 1) exit 1 }' "$source" > "$temporary/queue.inc"
extract cd_deferred 'static boolean_t' >> "$temporary/queue.inc"
extract cd_catalog_enqueue 'static int' >> "$temporary/queue.inc"
extract cd_catalog_retry 'static int' >> "$temporary/queue.inc"
compiler=${CC:-cc}
"$compiler" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -fno-omit-frame-pointer -g \
    "-DSYNC_BATCH_QUEUE_SOURCE=\"$temporary/queue.inc\"" \
    "$here/sync_batch_queue_test.c" -o "$temporary/sync-batch-queue-test"
"$temporary/sync-batch-queue-test"
