# P3 — ANSI terminal styling and search highlighting

Status: 0.3.0 implemented (W4).

Add optional ANSI SGR styling for rendered Markdown, HTML, and Mermaid
documents and live POSIX regex search highlighting in both raw and
rendered modes.

Use a sidecar byte-offset style-span table (`struct view_styles`) rather
than embedding escape sequences in rendered text so that:

- untrusted control bytes (including `\033`) in raw or rendered input
  always remain sanitized by `layout.c`;
- regex search, word wrapping, table column alignment, horizontal
  scrolling, and line chopping operate on plain UTF-8 offsets;
- every styled row resets SGR (`\033[0m`) before `\r\n`, leaving the
  shell prompt clean on exit or suspend.

Support `--color[=auto|always|never]`, `--no-color`, `NO_COLOR`,
`TERM=dumb`, Linux VT-safe SGR attributes on `TERM=linux`, live `C`
color toggling in the pager, and `Esc` to clear active search
highlighting.
