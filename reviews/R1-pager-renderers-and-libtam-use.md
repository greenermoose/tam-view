# R1 — Pager, renderers and libtam use

- **Date:** 2026-11-22
- **Reviewer:** agy 1.5.14, Claude Opus 5.5 (High)
- **Revision:** `aa6c8402a848b60223fbf8c232b979d9857e97bd` (0.4.0, built against libtam `72318d0`, 0.9.0)
- **Scope:** `src/`, `include/` and `tests/`, with the calls into libtam
  (`tam_pager`, `tam_key`, `tam_tty`, `tam_uni`). tam's review R1 and its
  bugs B1–B6 were used as a checklist of defects that might recur here.

## Summary

Memory handling is careful almost everywhere: every `realloc` goes
through a temporary pointer, and control characters from documents
reach the terminal only as visible caret text. The most serious finding
is in the renderer's input reader. A short read frees the buffer and then
returns it, which corrupts the heap (B1). Rendered view of piped input
silently drops everything after the first 64 KiB (B2). External links open
on a single key press, and a failed open is reported as success (B3).
Of tam's six bugs, B6 does not recur as memory corruption here, and B4 has
a milder analogue.

## Findings

### 1. A short read frees the render buffer, then returns it (critical)

- **Where:** [read_all_source](../src/render.c#L781-L797)
- **What:** On `VIEW_EOF` part-way through the copy, the loop frees `buf`,
  sets `len` and breaks out of the loop. The function then writes the
  terminator into the freed block and returns it. The caller renders from
  it and frees it a second time.
- **Evidence:** A spool source whose size is larger than its contents
  makes ASan report heap-use-after-free at `render.c:794`. In use, the
  trigger is a file that shrinks while it is rendered or re-rendered.
- **Fix:** Keep the buffer and shorten `len` on EOF. Free only on the
  error path. Add the reproducer to `tests/test-render.c`.
- **Status:** [B1](../bugs/B1-render-short-read-frees-buffer.md)

### 2. Rendered view of piped input is cut at the first chunk (medium)

- **Where:** [read_all_source](../src/render.c#L762-L769)
- **What:** For a pipe, the reader takes one 64 KiB block and stops if the
  writer has not finished. The rendered snapshot is not refreshed as more
  input arrives. The footer shows the shortened document as complete.
- **Evidence:** In a PTY, a 200-paragraph Markdown file written into a
  pipe in two halves one second apart rendered through paragraph 101 only.
- **Fix:** Read to EOF (within `RENDER_MAX_INPUT`) before rendering piped
  input, or re-render when the source grows.
- **Status:** [B2](../bugs/B2-piped-render-truncated.md)

### 3. External links open on one key press; failures report success (medium)

- **Where:** [follow_focused_link](../src/pager.c#L1087-L1096) ·
  libtam `tam_uri_open_external`
- **What:** Right, Enter or `o` hands the focused URL to `xdg-open` at
  once. Right also scrolls when no link is focused, so a scroll can start
  a browser. "Opened: …" is shown even when `xdg-open` is missing or fails,
  which is the usual case on a Linux VT. tam asks for a second key press
  before handing a URL over.
- **Evidence:** In libtam, the opener reaps only the intermediate child and
  returns 0 without the exec status. The pager prints success for any 0.
- **Fix:** Confirm with a second key press, and have the opener report an
  exec failure through a close-on-exec pipe.
- **Status:** [B3](../bugs/B3-external-link-single-press.md)

### 4. Oversized numeric character references wrap around (low)

- **Where:** [entity decoding](../src/render_html.c#L27-L57)
- **What:** The code-point accumulator overflows, so `&#4294967361;`
  renders as `A`. References above U+10FFFF and `&#0;` stay literal, and
  surrogates become U+FFFD. HTML maps all of these to U+FFFD.
- **Evidence:** `<p>&#4294967361;</p>` rendered with `-R --no-color`.
- **Fix:** Stop accumulating once the value passes 0x10FFFF and emit
  U+FFFD for zero, surrogates and out-of-range values.
- **Status:** open

### 5. Anchors in links to other files are decoded twice (low)

- **Where:** [jump_to_anchor](../src/pager.c#L1019)
- **What:** `tam_uri_resolve_local` already percent-decodes the fragment.
  `jump_to_anchor` decodes it again, so `other.md#a%2520b` looks for
  `a b` instead of `a%20b`. Same-file `#anchor` links are decoded once.
- **Evidence:** Calling the libtam function directly on that link returns
  `a%20b`, which the pager decodes again.
- **Fix:** Decode in one place, and state the contract in `tam_pager.h`.
- **Status:** open

### 6. Repeated identical headings make rendering slow (low)

- **Where:** [rbuf_add_heading_anchor](../src/render.c#L348-L373)
- **What:** Each duplicate heading tries up to 1,000 `-N` suffixes, each
  with a linear scan of the anchors. After 1,000 duplicates a heading gets
  no anchor. A resize re-renders the document.
- **Evidence:** 3,000 `# Same` headings took about 5 s to render. Every
  other stress input (deep nesting, 30,000 links, a 2,000-node Mermaid
  chain, a 2 MB line) took 3–66 ms.
- **Fix:** Keep a per-slug counter or a hash of anchors.
- **Status:** open

### 7. A failed back step loses its history entry (low)

- **Where:** [go_back](../src/pager.c#L1133-L1141) ·
  [follow_focused_link cleanup](../src/pager.c#L1117-L1121)
- **What:** The entry is popped before `switch_file`, so a failed load
  drops it. This is the same code path as tam's B6, but it is not a memory
  error here, because `nav_entry` owns no strings. If `push_history`
  silently fails to allocate, the cleanup after a failed link removes an
  older entry instead.
- **Evidence:** Source inspection.
- **Fix:** Pop only after a successful switch, and have `push_history`
  report failure.
- **Status:** open

### 8. Resize moves link focus to the first link on the line (low)

- **Where:** [refresh_size](../src/pager.c#L1946) ·
  [catch_link_on_attn](../src/pager.c#L516-L523)
- **What:** After a resize, focus goes to the first link on the attention
  line, not to the link that was focused. This is a milder form of tam's
  B4. The scroll position itself is kept.
- **Evidence:** Source inspection. In a PTY resize from 100 to 40
  columns, the top paragraph stayed the same.
- **Fix:** Restore by link index when that link is still on the line.
- **Status:** open

### 9. A PTY check races the footer, and another pins the version (low)

- **Where:** [test-pty.c](../tests/test-pty.c#L653-L656)
- **What:** After following a link, the test checks the footer right after
  `expect(…, "Section Two")`, with no wait loop. Separately, the test
  looks for the literal `v0.4.0`, so the next version bump breaks it.
- **Evidence:** `make test` failed once in four runs (`L6` missing).
- **Fix:** Wait for the footer as the other checks do, and pass the
  version in from the Makefile, as `tests/cli.sh` already does.
- **Status:** open

### 10. Spool files stay open in child processes (low)

- **Where:** [source_open](../src/source.c#L27) · [view_render_source](../src/render.c#L840)
- **What:** `tmpfile()` descriptors lack close-on-exec, so `xdg-open` and
  any browser it starts inherit them.
- **Evidence:** Source inspection.
- **Fix:** Set `FD_CLOEXEC` after `tmpfile()`.
- **Status:** open

### 11. The build does not check the libtam version (low)

- **Where:** [Makefile](../Makefile)
- **What:** tam-view needs libtam 0.9.0 for `tam_pager.h`. Against an older
  prefix the build fails with a missing-header error, not a clear
  message. tam's B1 shows the cost of a dependency without a version gate.
- **Evidence:** Source inspection.
- **Fix:** Add a `libtam-check` target that tests `TAM_VERSION_*`, and pin
  the libtam revision when tam-view is packaged.
- **Status:** open

## tam's R1 against tam-view

| tam | tam-view |
|---|---|
| B6 failed back/forward frees a live entry | Not memory corruption here; the entry is lost instead (finding 7) |
| Unchecked `realloc` growth | Not affected: every `realloc` in `src/` goes through a temporary pointer and is checked |
| B4 resize selects an earlier link | Milder analogue (finding 8) |
| B3 address parsing | Not affected: the scheme is parsed from the start, and the fragment and query are split off before decoding; double decoding is separate (finding 5) |
| Duplicate key and UTF-8 decoders | Not affected: tam-view uses `tam_key` and `tam_uni` |
| Timeouts that restart on each wait | Not applicable: the poll timeouts serve the Escape delay and follow mode |
| B1 dependency pin | Not yet packaged; gate it first (finding 11) |
| B2, B5 | Not applicable |

## Limits

- Builds and tests ran on an exported copy of the revision against an
  exported libtam `72318d0`. `make test` and `make sanitize` pass apart
  from finding 9.
- The renderers were exercised with 21 adversarial inputs under ASan and
  UBSan: unterminated comments, tags, fences and links, deep nesting, wide
  and combining table cells, and malformed Mermaid. None failed. The
  review read `render_md.c` and `render_mermaid.c` less closely than the
  pager.
- Terminal behaviour was checked with PTY scripts, not on a physical Linux
  VT or in tam-last.
