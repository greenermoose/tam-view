# B2 — Rendered view of piped input is cut at the first chunk

- **Status:** Confirmed
- **Severity:** medium
- **Affected versions:** Confirmed in 0.4.0
- **Fixed in:** Open
- **Reported:** 2026-11-22, agy (Claude Opus 5.5)
- **Found by:** [R1](../reviews/R1-pager-renderers-and-libtam-use.md)

## What happens

When Markdown or HTML is piped into `tam-view -R` and arrives in more than
one piece, only the first chunk (up to 64 KiB) is rendered. The footer
shows the shortened document as complete.

## What should happen

The rendered view shows the whole input, or says that more input is
still arriving.

## Steps to reproduce

Make a Markdown file of 200 numbered paragraphs, about 128 KiB. In a
terminal, write its first half into a pipe read by `tam-view -R
--no-color`, wait one second, then write the rest and close the pipe.
Press `G`. The last paragraph shown is about number 101. Opening the file
by name shows all 200. Resizing the terminal re-renders the document,
and that render includes what has arrived since.

## Cause

`read_all_source` reads one block from a pipe and stops if the writer
has not finished. The rendered snapshot is replaced only when the
width changes. [read_all_source](../src/render.c#L762-L769)

## Fix and verification

Read piped input to EOF, up to `RENDER_MAX_INPUT`, before rendering it, or
re-render when the source grows. Add a PTY test that pipes a document of
more than 64 KiB in two parts and checks its last paragraph.
