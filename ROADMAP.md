# tam-view roadmap

## 0.0.1 — First text viewer

Page through a file or piped input. Move backward and forward, jump to a
line, and search for literal text. Leave the last view visible on exit.

## 0.1.0 — More ways to navigate text

Add POSIX regular-expression search, marks, horizontal scrolling, a
file list, and follow mode. Keep the default exit behavior.

## 0.2.0 — Render documents and diagrams

Keep raw text as the default. Add a render mode for Markdown, HTML, and
Mermaid that lays the document out as readable terminal text. Start with
document structure and Mermaid flowcharts that fit a Linux console
including an ASCII layout. Use the same pager for both viewing modes.

## 0.3.0 — ANSI terminal styling and search highlighting

Add optional ANSI SGR styling for rendered Markdown, HTML, and Mermaid
documents and live regex search highlighting in both raw and rendered
modes using sidecar byte-offset style spans so search, wrapping, and
input sanitization remain exact. Support `--color`/`--no-color`,
`NO_COLOR`, `TERM=dumb`, `TERM=linux`, live `C` color toggle, and `Esc`
highlight clearing.
