# tam-view roadmap

`tam-view` is Tamlinux's terminal text, Markdown, HTML, and Mermaid
viewer, written in C17 on top of `libtam`.

- **Current version:** `0.4.6` (branch `dev`)
- **Progress:** All 5 phases done (in maintenance)
- **Versioning:** Semantic versioning; during 0.x, minor marks a phase and
  patch marks milestones or fixes within it.

## Phase summary

| Phase | Versions | Goal | Plan | Status |
| --- | --- | --- | --- | --- |
| **P1** | 0.0.1–0.1.0 | Reliable terminal text viewer with regex search, marks, horizontal scroll, and follow mode | [P1-text-viewer.md](plans/P1-text-viewer.md) | Done |
| **P2** | 0.2.0 | Render Markdown, HTML, and Mermaid diagrams as readable terminal text | [P2-rendering.md](plans/P2-rendering.md) | Done |
| **P3** | 0.3.0–0.3.1 | ANSI SGR styling, live regex search highlighting, and EOF wait default | [P3-styling-and-highlighting.md](plans/P3-styling-and-highlighting.md) | Done (2026-11-22) |
| **P4** | 0.4.0–0.4.6 | Link navigation, page overlap, `libtam-render` extraction, and `libtam` TTY/pause integration | [P4-link-navigation.md](plans/P4-link-navigation.md) | Done (2026-11-26) |
| **P5** | 0.4.2+ | Watch active named files by default as data is appended | [P5-watch-mode.md](plans/P5-watch-mode.md) | Done (2026-11-25) |

## Phases and milestones

### Phase P1: Reliable text viewer (0.0.1–0.1.0) — [P1-text-viewer.md](plans/P1-text-viewer.md)

| Version | Milestone | Status |
| --- | --- | --- |
| **0.0.1** | First text viewer: page files or piped input, jump to line, literal search, leave last view on exit | Done |
| **0.1.0** | POSIX regex search, marks, horizontal scrolling, file list, and follow mode | Done |

### Phase P2: Render markup and diagrams (0.2.0) — [P2-rendering.md](plans/P2-rendering.md)

| Version | Milestone | Status |
| --- | --- | --- |
| **0.2.0** | Render mode for Markdown, HTML, and Mermaid flowcharts (including Linux console ASCII layout) in the same pager | Done |

### Phase P3: Styling and search highlighting (0.3.0–0.3.1) — [P3-styling-and-highlighting.md](plans/P3-styling-and-highlighting.md)

| Version | Milestone | Status |
| --- | --- | --- |
| **0.3.0** | Optional ANSI SGR styling and live regex search highlighting via sidecar spans (`--color`/`--no-color`, `NO_COLOR`, `C` toggle) | Done (2026-11-22) |
| **0.3.1** | Wait for `q` at EOF by default; add `-E`/`--quit-at-eof` option | Done (2026-11-22) |

### Phase P4: Link navigation and libtam integration (0.4.0–0.4.6) — [P4-link-navigation.md](plans/P4-link-navigation.md)

| Version | Milestone | Status |
| --- | --- | --- |
| **0.4.0** | Hyperlink focus/following (local docs/anchors with back-history, external `xdg-open`, OSC 8), attention line, footer, `g` guide | Done (2026-11-22) |
| **0.4.1** | Review R1 fixes for piped rendering, cache invalidation, HTML entities, list wrapping, and Mermaid state diagrams | Done (2026-11-25) |
| **0.4.2** | Configurable page overlap (`--overlap`, default 2 rows) and full-window top-aligned layout | Done (2026-11-25) |
| **0.4.3** | Move renderer into `libtam-render.a` (`libtam` 0.13.0) so `tam-view` and `tam-knox` share one copy | Done (2026-11-26) |
| **0.4.4** | Suspend/resume, crash restore, clock, output loop, and locale setup through `libtam` | Done (2026-11-26) |
| **0.4.5** | `Ctrl+P` pauses viewer per keybinding standard; `Ctrl+Z` (Undo) does nothing in viewer | Done (2026-11-26) |
| **0.4.6** | `Ctrl+P` pauses through `tam_tty_job_stop` (`libtam` 0.14.1), including session leaders | Done (2026-11-26) |

### Phase P5: Watch mode (0.4.2+) — [P5-watch-mode.md](plans/P5-watch-mode.md)

| Version | Milestone | Status |
| --- | --- | --- |
| **0.4.2** | Watch named files by default in the interactive pager, keeping the tail visible when already at EOF | Done (2026-11-25) |

## How a milestone is done

A milestone is done when its acceptance checks in the phase plan pass and
Fred has tried the build. When a milestone lands, bump `VERSION`, update
the phase plan's status, and update this roadmap in the same change.
