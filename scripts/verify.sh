#!/bin/sh
set -eu
repo_dir=$(CDPATH= cd "$(dirname "$0")/.." && pwd -P)
exec python3 "$repo_dir/chunkdedup/workflow/run.py" --repo "$repo_dir" --kit "$repo_dir/chunkdedup" "$@"
