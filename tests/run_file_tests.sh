#!/bin/sh
set -eu

tool=${1:?usage: run_file_tests.sh PATH_TO_FUSION_FILE}
here=$(cd "$(dirname "$0")" && pwd)
data="$here/data"
tolerance=1e-9
failures=0
cases=0

value_of() {
    printf '%s\n' "$1" | awk -v key="$2" 'index($0, key " = ") == 1 { print substr($0, length(key) + 4) }'
}

close_enough() {
    awk -v a="$1" -v b="$2" -v tol="$tolerance" 'BEGIN {
        d = a - b; if (d < 0) d = -d;
        m = (a < 0 ? -a : a); n = (b < 0 ? -b : b); s = (m > n ? m : n); if (s < 1) s = 1;
        exit !(d <= tol * s)
    }'
}

check() {
    label=$1 actual=$2 expected=$3
    if [ -z "$actual" ] || ! close_enough "$actual" "$expected"; then
        echo "  FAIL $label: got '${actual}', expected $expected"
        failures=$((failures + 1))
    fi
}

while read -r file window f1 f2_formula f2_pair; do
    case $file in ''|'#'*) continue ;; esac
    cases=$((cases + 1))
    set -- "$data/$file"
    [ "$window" != 0 ] && set -- -n "$window" "$@"

    out_formula=$("$tool" -m formula "$@")
    out_pair=$("$tool" -m pair-mean "$@")

    before=$failures
    check "$file f1" "$(value_of "$out_formula" 'fusion function 1')" "$f1"
    check "$file f2[formula]" "$(value_of "$out_formula" 'fusion function 2')" "$f2_formula"
    check "$file f2[pair-mean]" "$(value_of "$out_pair" 'fusion function 2')" "$f2_pair"
    [ "$before" -eq "$failures" ] && echo "[  OK  ] $file" || echo "[ FAIL ] $file"
done < "$data/cases.txt"

echo "$cases file cases, $failures failures"
[ "$failures" -eq 0 ]
