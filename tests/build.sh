#!/bin/bash
cd "$(dirname "$0")/.."
config=""
if [ "$1" = "--config" ]; then
	config="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"
	[ -f "$config" ] || { echo "no such config header: $2"; exit 2; }
fi
mkdir -p build/tests
failed=0
for src in tests/*_test.cpp tests/audio_offline/*.mm; do
	cmd=$(head -n 1 "$src")
	cmd=${cmd#// }
	compile=${cmd%% && build/tests/*}
	out=$(echo "$compile" | sed -E 's/.* -o (build\/tests\/[^ ]+)$/\1/')
	if [ -n "$config" ]; then
		define="-DPINNA_CONFIG_HEADER='\"$config\"'"
		compile=${compile/clang++ /clang++ $define }
	fi
	rm -f "$out"
	if ! eval "$compile" > "$out.build.log" 2>&1 < /dev/null; then
		echo "BUILD FAIL $src"
		sed 's/^/    /' "$out.build.log"
		failed=$((failed + 1))
	fi
done
[ $failed -eq 0 ]
