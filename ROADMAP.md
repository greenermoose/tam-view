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

## 0.4.0 — Link navigation

Record link targets alongside rendered documents so users can focus and
follow hyperlinks from Markdown and HTML. Support keyboard link cycling
and following (opening local/relative documents and anchors inside
tam-view with back-history, and handing external URLs to `xdg-open`),
plus optional OSC 8 terminal hyperlinks on capable terminals.

## 0.4.2 — Page overlap and full-window layout

Full-page navigation repeats two displayed rows by default. Set the
count in both directions with `--overlap`. Short files and short final
pages start at the top of the terminal, with the footer at the bottom.


## 0.4.3 — Renderer in libtam

The document source, layout and Markdown/HTML/Mermaid renderers moved
into libtam's `libtam-render.a` (libtam 0.13.0), so tam-knox shares one
copy instead of carrying its own. tam-view links the archive; its
behaviour is unchanged. The renderer's tests moved to libtam with it.

## 0.4.4 — More of libtam

Ctrl+Z suspends and resumes through libtam (still SIGSTOP, which also
stops a session leader), the terminal is restored after a crash or an
exit that skips cleanup, and the clock, output loop and locale setup use
libtam.

## 0.4.5 — Ctrl+P pauses, Ctrl+Z is Undo

Following the tam-idol keybinding standard: Ctrl+P pauses the viewer
(Pause, as the shell's VSUSP), and Ctrl+Z, which is Undo everywhere,
does nothing in a viewer instead of suspending it.

## 0.4.6 — Pause through libtam

Ctrl+P pauses through `tam_tty_job_stop` (libtam 0.14.1), which now also
pauses a session leader; tam-view no longer raises SIGSTOP itself.
