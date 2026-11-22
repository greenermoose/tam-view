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
"$bin" -r -- -dash > output
cmp -- -dash output
"$bin" - < ./-dash > output
cmp -- -dash output
cat ./-dash | "$bin" > output
cmp -- -dash output
printf 'second\n' > second
cat ./-dash second > both
"$bin" -- -dash second > output
cmp both output

printf '# Heading\n\n* item\n' > sample.md
"$bin" -R --ascii sample.md > md_out
grep -F 'Heading' md_out >/dev/null
grep -F '* item' md_out >/dev/null
# Redirected -R defaults to plain text (no ESC bytes).
if grep -q "$(printf '\033')" md_out; then
    exit 1
fi
# Explicit --color emits ANSI SGR on rendered stdout.
"$bin" -R --ascii --color sample.md > md_color
grep -q "$(printf '\033\[')" md_color
# --no-color and NO_COLOR=1 suppress ANSI SGR.
"$bin" -R --ascii --no-color sample.md > md_nocolor
if grep -q "$(printf '\033')" md_nocolor; then
    exit 1
fi
NO_COLOR=1 "$bin" -R --ascii sample.md > md_envnocolor
if grep -q "$(printf '\033')" md_envnocolor; then
    exit 1
fi

printf 'flowchart TD\nA[One] --> B[Two]\n' > sample.mmd
"$bin" --render --ascii sample.mmd > mmd_out
grep -F '| One |' mmd_out >/dev/null
grep -F '| Two |' mmd_out >/dev/null

printf '<p>&ldquo;hi&rdquo;</p>\n' > sample.html
TERM=dumb "$bin" -R --ascii sample.html > html_out
grep -F '"hi"' html_out >/dev/null

if "$bin" --invalid 2> error || "$bin" absent 2> error ||
   "$bin" . 2> error || "$bin" one two 2> error; then
    exit 1
fi
printf 'CLI: ok\n'
