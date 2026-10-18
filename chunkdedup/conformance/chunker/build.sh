#!/bin/sh
# Portable build wrapper: Apple uses CommonCrypto; other systems use libcrypto.
set -eu
cd "$(dirname "$0")"
case $(uname -s) in
    Darwin) exec make "$@" ;;
    *) exec make LDLIBS=-lcrypto "$@" ;;
esac
