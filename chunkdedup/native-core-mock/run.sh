#!/bin/sh
# Compile the supplied actual core against this in-memory model, never a pool.
set -eu
if [ "$#" -ne 1 ]; then
    printf 'Usage: %s /path/to/patched-openzfs\n' "$0" >&2
    exit 64
fi
mock_dir=$(CDPATH= cd "$(dirname "$0")" && pwd -P)
mock_zfs_root=$(CDPATH= cd "$1" && pwd -P)
for mock_input in module/zfs/zfs_chunkdedup.c include/sys/zfs_chunkdedup.h; do
    if [ ! -f "$mock_zfs_root/$mock_input" ]; then
        printf 'Missing patched source: %s\n' "$mock_input" >&2
        exit 66
    fi
done
mock_build_dir=$(mktemp -d "${TMPDIR:-/tmp}/zfs-chunkdedup-mock.XXXXXX")
trap 'rm -rf "$mock_build_dir"' EXIT HUP INT TERM
mock_cc=${CC:-clang}
printf 'Compiler: '
"$mock_cc" --version | head -n 1
printf 'Core: %s\n' "$mock_zfs_root/module/zfs/zfs_chunkdedup.c"
# The digest adapter uses CommonCrypto on macOS and OpenSSL elsewhere.
case $(uname -s) in
    Darwin) mock_crypto_lib= ;;
    *) mock_crypto_lib=-lcrypto ;;
esac
"$mock_cc" -std=gnu99 -Wall -Wextra -Werror \
    -Wno-deprecated-declarations -fsanitize=address,undefined -g \
    -I "$mock_dir/include" -I "$mock_zfs_root/include" \
    -I "$mock_zfs_root/module/zfs" "$mock_dir/core_test.c" \
    -o "$mock_build_dir/core_test" $mock_crypto_lib
# Fixtures intentionally survive until process exit; this is not a leak test.
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    "$mock_build_dir/core_test"
