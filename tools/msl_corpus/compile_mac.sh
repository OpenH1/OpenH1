#!/bin/sh
# compile_mac.sh <corpus dir> <log>: compiles each .metal file of a corpus
# (msl_corpus) with Apple's Metal compiler, on macOS with Xcode's command
# line tools, as the Metal backend's newLibraryWithSource would (MSL 3.0,
# the position invariant), and logs each file's errors and warnings, then a
# count of each
corpus=$1
log=$2
tmp=$(mktemp -d)
ls "$corpus" | grep '\.metal$' | xargs -P "$(sysctl -n hw.ncpu)" -I{} sh -c '
	if ! xcrun -sdk macosx metal -std=metal3.0 -fpreserve-invariance -c "$1/$2" -o "$3/$2.air" > "$3/$2.txt" 2>&1; then
		echo "(failed)" >> "$3/$2.txt"
	fi
	rm -f "$3/$2.air"' sh "$corpus" {} "$tmp"
{
	xcrun -sdk macosx metal --version 2>&1 | head -1
	for f in "$tmp"/*.txt; do
		if [ -s "$f" ]; then
			echo "=== $(basename "$f" .txt)"
			cat "$f"
		fi
	done
	echo "files $(ls "$corpus" | grep -c '\.metal$'), failed $(cat "$tmp"/*.txt | grep -c '^(failed)$'), errors $(cat "$tmp"/*.txt | grep -c ' error: '), warnings $(cat "$tmp"/*.txt | grep -c ' warning: ')"
} > "$log"
rm -rf "$tmp"
tail -1 "$log"
