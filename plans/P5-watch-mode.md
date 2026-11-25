# P5 — Watch mode

**Status:** Implemented and verified in development.

Watch the active named file in the interactive pager. Keep the last line
visible as data is appended if the last line was already on screen.
Keep earlier pages steady and preserve visible text across rewrites
where possible. Detect atomic saves and recover when a path reappears.
Use `--no-watch` for a static snapshot. Keep explicit `F` follow and
Ctrl-C to stop it.

Acceptance covers updates to raw and rendered files, file replacement,
truncation, prior-page navigation, snapshots and live pipes in the PTY
suite. Existing redirected output and EOF exit behavior are preserved.
