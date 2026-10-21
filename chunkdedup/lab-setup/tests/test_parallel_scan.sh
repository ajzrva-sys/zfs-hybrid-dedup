#!/bin/sh
# SPDX-License-Identifier: MIT
# Compile and exercise the actual production parallel executor, without ZFS I/O.
set -eu
test "$#" -eq 1 || { echo 'usage: test_parallel_scan.sh OPENZFS_SOURCE' >&2; exit 2; }
headers=$1/cmd/zfs
test -f "$headers/chunkdedup_parallel.h" || { echo 'production parallel executor is missing' >&2; exit 2; }
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/zfs-parallel-scan.XXXXXX")
trap 'rm -rf -- "$temporary"' EXIT HUP INT TERM
compiler=${CC:-cc}
"$compiler" -std=c11 -Wall -Wextra -Werror -pthread -g \
    -I"$headers" "$here/parallel_scan_test.c" -o "$temporary/parallel-scan-test"
"$temporary/parallel-scan-test"
"$compiler" -std=c11 -Wall -Wextra -Werror -pthread -g \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$headers" "$here/parallel_scan_test.c" -o "$temporary/parallel-scan-sanitized"
"$temporary/parallel-scan-sanitized"

source=$1/cmd/zfs/zfs_chunkdedup_user.c
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
awk '/^struct cd_catalog_file \{/ { active=1 }
     active { print }
     /^static boolean_t cd_deferred\(int\);/ { if (active) exit }' "$source" > "$temporary/types.inc"
for name in cd_catalog_seen cd_scan_job cd_scan; do
    awk -v name="$name" '$0 == "struct " name " {" { active=1; found++ }
        active { print; if ($0 == "};") exit }
        END { if (found != 1) exit 1 }' "$source" >> "$temporary/types.inc"
done
: > "$temporary/functions.inc"
extract cd_deferred 'static boolean_t' >> "$temporary/functions.inc"
for name in cd_catalog_seen_slot cd_catalog_seen cd_catalog_record \
    cd_catalog_next cd_catalog_append cd_catalog_init; do
    extract "$name" 'static int' >> "$temporary/functions.inc"
done
extract cd_catalog_fini 'static void' >> "$temporary/functions.inc"
for name in cd_scan_work cd_scan_copy cd_scan_take cd_scan_drain cd_scan_init; do
    extract "$name" 'static int' >> "$temporary/functions.inc"
done
extract cd_scan_fini 'static void' >> "$temporary/functions.inc"
extract cd_scan_same 'static boolean_t' >> "$temporary/functions.inc"
extract cd_scan_submit 'static int' >> "$temporary/functions.inc"
"$compiler" -std=c11 -Wall -Wextra -Werror -pthread -g \
    -fsanitize=address,undefined -fno-omit-frame-pointer -I"$headers" \
    "-DSCAN_TYPES_SOURCE=\"$temporary/types.inc\"" \
    "-DSCAN_FUNCTIONS_SOURCE=\"$temporary/functions.inc\"" \
    "$here/parallel_scan_glue_test.c" -o "$temporary/parallel-scan-glue"
"$temporary/parallel-scan-glue"

# Exercise the current sharing driver through the same local verification gate.
python3 "$here/test_parallel_share.py" --repo "$1"

# Compare the actual in-memory catalog path with its disk fallback and report.
python3 "$here/test_catalog_memory.py" --repo "$1"

# Compare real parallel /usr/bin/sort processes with the preserved serial path.
python3 "$here/test_parallel_sort.py" --repo "$1"

# Exercise the actual optional FreeBSD dirty-data gauge with portable stubs.
python3 "$here/test_dirty_data.py" --repo "$1"
