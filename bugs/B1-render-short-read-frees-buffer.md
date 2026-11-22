# B1 — A short read frees the render buffer, then returns it

- **Status:** Confirmed
- **Severity:** critical
- **Affected versions:** Confirmed in 0.4.0
- **Fixed in:** Open
- **Reported:** 2026-11-22, agy (Claude Opus 5.5)
- **Found by:** [R1](../reviews/R1-pager-renderers-and-libtam-use.md)

## What happens

If a document has fewer bytes than its reported size when it is rendered,
the renderer uses freed memory and frees it a second time. The pager can
crash or corrupt its heap.

## What should happen

The renderer renders the bytes it could read and carries on.

## Steps to reproduce

Build with `make sanitize`. In a test linked with the render objects,
write a short Markdown text to a `tmpfile()`. Make a spool `struct source`
for it with `eof` true and `size` larger than the text, then call
`view_render_source` with `force_render` set. ASan reports
heap-use-after-free at `src/render.c:794`. In use, the trigger is a file
that shrinks while it is rendered or re-rendered after a resize.

## Cause

On `VIEW_EOF`, the copy loop frees `buf`, sets `len` and breaks out of the
loop. The function then writes `buf[len]` and returns `buf`. The caller
renders from it and frees it.
[read_all_source](../src/render.c#L781-L797)

## Fix and verification

Keep the buffer on EOF and free it only on the error path. Add the
reproducer to `tests/test-render.c` so that `make sanitize` covers it.
