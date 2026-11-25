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
grep -F -- '--no-watch' help >/dev/null
printf 'one\000two\033[2J\r\nthree' > ./-dash
"$bin" --raw -- -dash > output
cmp -- -dash output
"$bin" -r -- -dash > output
cmp -- -dash output
"$bin" --no-watch -- -dash > output
cmp -- -dash output
"$bin" - < ./-dash > output
cmp -- -dash output
cat ./-dash | "$bin" > output
cmp -- -dash output
printf 'second\n' > second
cat ./-dash second > both
"$bin" -- -dash second > output
cmp both output

printf '# Heading\n\n* [item](https://tamlinux.org)\n' > sample.md
"$bin" -R --ascii sample.md > md_out
grep -F 'Heading' md_out >/dev/null
grep -F '* item (https://tamlinux.org)' md_out >/dev/null
# Redirected -R defaults to plain text (no ESC bytes, no OSC 8).
if grep -q "$(printf '\033')" md_out; then
    exit 1
fi
# Explicit --color emits ANSI SGR on rendered stdout (still no OSC 8 unless --hyperlinks).
"$bin" -R --ascii --color sample.md > md_color
grep -q "$(printf '\033\[')" md_color
if grep -q "$(printf '\033\]8;;')" md_color; then
    exit 1
fi
# Explicit --hyperlinks emits OSC 8 on redirected -R.
"$bin" -R --ascii --hyperlinks sample.md > md_osc8
grep -q "$(printf '\033\]8;;https://tamlinux.org\033\\\\')" md_osc8
# --no-hyperlinks suppresses OSC 8.
"$bin" -R --ascii --color --no-hyperlinks sample.md > md_noosc8
if grep -q "$(printf '\033\]8;;')" md_noosc8; then
    exit 1
fi
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

python3 - "$bin" <<'PY'
import subprocess
import sys
import time

viewer = subprocess.Popen([sys.argv[1], '-R', '--ascii'],
                          stdin=subprocess.PIPE, stdout=subprocess.PIPE)
assert viewer.stdin is not None and viewer.stdout is not None
viewer.stdin.write(b'# First\n\n' + b'line\n' * 14000)
viewer.stdin.flush()
time.sleep(0.1)
viewer.stdin.write(b'\nLast paragraph\n')
viewer.stdin.close()
output = viewer.stdout.read()
assert viewer.wait() == 0 and b'Last paragraph' in output

html = subprocess.run([sys.argv[1], '-R', '--ascii'],
                      input=b'<p>HTML sentinel</p>', capture_output=True,
                      check=True).stdout
assert b'HTML sentinel' in html and b'<p>' not in html
PY

if "$bin" --invalid 2> error || "$bin" absent 2> error ||
   "$bin" . 2> error || "$bin" one two 2> error; then
    exit 1
fi
printf 'CLI: ok\n'
