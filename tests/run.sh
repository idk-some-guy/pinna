#!/bin/bash
cd "$(dirname "$0")/.."
bash tests/build.sh "$@" || echo "some tests did not build"
total=0
passed=0
for src in tests/*_test.cpp tests/audio_offline/*.mm; do
	cmd=$(head -n 1 "$src")
	cmd=${cmd#// }
	compile=${cmd%% && build/tests/*}
	out=$(echo "$compile" | sed -E 's/.* -o (build\/tests\/[^ ]+)$/\1/')
	total=$((total + 1))
	if [ -x "$out" ] && perl -e 'alarm shift; exec @ARGV' 60 "$out" > "$out.log" 2>&1 < /dev/null; then
		echo "PASS $(basename "$out")"
		passed=$((passed + 1))
	else
		echo "FAIL $(basename "$out")"
		[ -f "$out.log" ] && tail -n 20 "$out.log" | sed 's/^/    /'
	fi
done
echo "$passed of $total passed"
[ $passed -eq $total ]
