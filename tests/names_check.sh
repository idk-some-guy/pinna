#!/bin/bash
cd "$(dirname "$0")/.."
PATHS=()
while IFS= read -r path; do
	PATHS+=("$path")
done < <(git ls-files | grep -vxE 'README\.md|LICENSE|tests/names_check\.sh')
fail=0
total=0
report() {
	local label="$1" hits="$2"
	total=$((total + 1))
	if [ -n "$hits" ]; then
		echo "FAIL $label"
		echo "$hits" | sed 's/^/    /'
		fail=$((fail + 1))
	else
		echo "PASS $label"
	fi
}
check() {
	local label="$1"; shift
	report "$label" "$(grep -rnE "$@" "${PATHS[@]}")"
}
checki() {
	local label="$1"; shift
	report "$label" "$(grep -rniE "$@" "${PATHS[@]}")"
}
checki reVC reVC
checki re3 '(^|[^a-z])re3'
check vice '(^|[^A-Za-z])([Vv]ice[a-z]*|VICE)([^a-z]|$)|Vice[A-Z]|vice::'
checki gta '(^|[^a-z])gta'
checki miami miami
checki sampman sampman
checki police police
checki cutscene cutscene
check ped '(^|[^A-Za-z])[Pp]eds?([^a-z]|$)|Ped[A-Z]'
check game '(^|[^A-Za-z])[Gg]ame|Game[A-Z]'
check FEA_ 'FEA_'
check AUDIO_CA 'AUDIO_CA'
check SetChannel3DPosition 'SetChannel3DPosition'
checki user-track 'user[ -]track'
checki studio-names 'rockstar|liberty|vercetti|tommy|librw|renderware'
checki sfx-bank 'sfx\.(SDT|RAW)'
checki sound-cache 'sound(_ca)?\.cache'
check bank '(^|[^v])[Bb]ank'
checki radio radio
checki frontend frontend
checki menu menu
checki adf '\.adf'
checki ini '\.ini([^a-z]|$)'
checki SpeakerType SpeakerType
checki XorReader 'xor(reader)?'
check all-caps-names '(^|[^A-Z])(GAME|PED|BANK|SFX)([^A-Z]|$)'
check all-caps-literal '"[A-Z]{3,}( [A-Z0-9]+)+"'
foreign=$(grep -rnE '^[[:space:]]*#[[:space:]]*include[[:space:]]+"[^"]*"' "${PATHS[@]}" | while IFS= read -r line; do
	file=${line%%:*}
	inc=$(echo "$line" | sed -E 's/^[^"]*"([^"]*)".*/\1/')
	dir=$(dirname "$file")
	roots="$dir src src/coreaudio"
	[[ $file == src/* ]] || roots="$roots tests tests/support"
	found=0
	for r in $roots; do [ -e "$r/$inc" ] && found=1 && break; done
	[ $found -eq 1 ] || echo "$line"
done)
report include-outside-tree "$foreign"
echo "names: $((total - fail)) of $total PASS"
[ $fail -eq 0 ]
