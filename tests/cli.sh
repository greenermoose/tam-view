#!/bin/sh
set -eu
bin=$(realpath "$1")
expected=$2
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
cd "$scratch"

test "$("$bin" --version)" = "tam-view $expected"
"$bin" --help > help
grep -F 'Usage: tam-view' help >/dev/null
printf 'one\000two\033[2J\r\nthree' > ./-dash
"$bin" --raw -- -dash > output
cmp -- -dash output
"$bin" - < ./-dash > output
cmp -- -dash output
cat ./-dash | "$bin" > output
cmp -- -dash output
printf 'second\n' > second
cat ./-dash second > both
"$bin" -- -dash second > output
cmp both output

if "$bin" --invalid 2> error || "$bin" absent 2> error ||
   "$bin" . 2> error || "$bin" one two 2> error; then
    exit 1
fi
printf 'CLI: ok\n'
