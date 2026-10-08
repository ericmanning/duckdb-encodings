#!/bin/bash
# Time read_csv decoding of one file with one encodings extension build, 3 runs.
#
# Usage: bench.sh <duckdb-cli> <file.csv> <encoding> [extension-path]
#
# With an extension path, that build is LOADed (the CLI must be the matching
# official release, e.g. v1.5.6). Without one, the CLI's own encodings
# extension is used (in-tree builds link it statically). The output columns
# are a row count and hash fingerprints, which must match across builds.
set -euo pipefail

cli="$1"
file="$2"
encoding="$3"
extension="${4:-}"

query="SELECT count(*), sum(hash(name)), sum(hash(description)) FROM read_csv('$file', encoding='$encoding', header=true);"
flags=()
preamble=""
if [ -n "$extension" ]; then
	# Disable autoload so a failed LOAD errors instead of silently falling back to the official extension
	flags=(-unsigned)
	preamble="SET autoinstall_known_extensions=false; SET autoload_known_extensions=false; LOAD '$extension';"
fi

{
	echo "$preamble"
	echo ".timer on"
	for _ in 1 2 3; do
		echo "$query"
	done
} | "$cli" ${flags[@]+"${flags[@]}"} -list -noheader
