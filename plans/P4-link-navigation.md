# P4 — Link navigation

Status: planned for 0.4.0.

Record link metadata (byte span and target URI) alongside rendered
Markdown and HTML documents so hyperlinks can be focused and followed
without embedding control codes into the rendered text buffer.

Scope to design before implementation:

- **Keyboard navigation in the pager**: cycle visible/document links
  (e.g. `Tab` / `Shift-Tab`), visually highlight the focused link, and
  follow it (e.g. `Enter` / `o`), with a navigation history stack to
  return to the previous document and position.
- **Local vs. external targets**: resolve relative file paths and
  in-document `#anchor` links directly inside `tam-view`; hand external
  `http://` / `https://` URLs to `xdg-open` (keeping `tam-view` free of
  networking library dependencies).
- **OSC 8 terminal hyperlinks**: optionally emit clickable OSC 8
  sequences on capable terminals while omitting them on `TERM=linux`,
  `TERM=dumb`, `--no-color`, and redirected stdout.
