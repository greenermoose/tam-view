# P4 — Link navigation

Status: implemented in 0.4.0 (with shared primitives in `libtam` 0.9.0 `tam_pager`).

Recorded sidecar link metadata (byte spans and target URIs) and anchor targets
(Markdown heading slugs and HTML `id`/`name`) alongside rendered Markdown and
HTML documents without embedding raw control bytes into the rendered buffer.

Implemented behavior:

- **Shared `libtam` primitives (`<tam_pager.h>`)**: attention-line and
  focused-link SGR helpers, safe OSC 8 open/close formatters, standard
  `<path> | L<line> <top>-<bot>/<total>    vX.Y.Z` footer formatter, URI
  classification, percent-decoding, local path resolution, GitHub-style
  heading slugification, and shell-free `xdg-open` dispatch.
- **Mode-aware attention line**: Rendered Markdown and HTML show a
  highlighted attention line; `Down`/`Up` steps through links on the row
  and moves the attention line row-by-row with automatic link highlighting.
  Plain text, raw mode, and standalone Mermaid diagrams scroll directly
  with `Down`/`Up` without an attention line.
- **Link following & history**: `Right` (or `Enter`/`o`) follows the
  highlighted link (`#anchor` jumps, local/relative files with optional
  `#anchor`, or `xdg-open` for `http://`/`https://`/`mailto:`); `Left`
  (or `Backspace`) returns via the navigation history stack; `Tab`/`Shift-Tab`
  cycles links across the document.
- **Keybindings guide (`g`)**: Pressing `g` toggles the scrollable
  keybindings guide on and off (`Home`/`<`/`1g` jump to line 1; `Ng`
  jumps to line `N`).
- **OSC 8 terminal hyperlinks**: Emitted on capable interactive terminals
  and controlled via `--hyperlinks[=auto|always|never]` / `--no-hyperlinks`
  (omitted on `TERM=linux`, `TERM=dumb`, `--no-color`, `NO_COLOR`, and
  redirected stdout by default).
