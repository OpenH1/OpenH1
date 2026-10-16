#!/bin/sh
# check.sh <corpus dir> <log>: checks each module of a corpus (spirv_corpus)
# with spirv-val (SPIRV-Tools) for Vulkan 1.1, as the Vulkan backend hands
# them to the driver, logs every message, then the count of modules that
# failed; exits nonzero if any did
corpus=$1
log=$2
: > "$log"
total=0
failed=0
for module in "$corpus"/*.spv; do
	total=$((total + 1))
	if ! output=$(spirv-val --target-env vulkan1.1 "$module" 2>&1); then
		failed=$((failed + 1))
		printf '%s\n%s\n' "$(basename "$module")" "$output" >> "$log"
	fi
done
printf '%d modules, %d failed spirv-val\n' "$total" "$failed" | tee -a "$log"
[ "$failed" -eq 0 ]
