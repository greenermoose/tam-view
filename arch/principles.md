# Architecture

- Regular files stay on disk. A block cache serves random reads and the
  pager lays out only the rows needed for the current view or operation.
  Unseekable input is read on demand into a temporary file so the same
  layout and search code can page backward. A slow producer and the
  terminal keyboard are polled independently.
- A display row starts at a byte offset and records any unfinished tab.
  Layout measures Unicode cell width, expands tabs, normalizes CRLF, and
  translates control characters before the text reaches the terminal.
  Search uses source bytes and a bounded literal matcher. A line number
  jump walks logical line endings.
- The current screen records enough row positions to scroll forward on
  the primary screen. Backward movement redraws from the status line.
  The status line doubles as the shell prompt row after cleanup.
- libtam provides terminal mode management, key decoding, and UTF-8
  operations. The pager uses CR, LF, and basic ANSI cursor movement and
  line erase. It restores termios before suspension and exit.
- A resize regenerates wrapping around the current source position.
  Reverse movement through a very long wrapped logical line can rescan
  that line to recover its row boundaries.
- A future renderer will supply layout input to the same pager. File
  source access, navigation, and terminal handling are separate so
  renderers can be developed without changing terminal behavior.
