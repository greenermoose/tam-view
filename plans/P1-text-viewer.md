# P1 — Reliable text viewing

Status: 0.0.1 and 0.1.0 implemented (W1, W2).

## 0.0.1

Read one file or piped stdin. Page forward and backward, find literal
text in either direction, repeat a search, and jump to a file line.

Fred approved core viewing, immediate return at EOF, and literal
search for this milestone. Use the primary screen and return the
prompt below the visible page both at EOF and on quit.

Acceptance covers layouts, argument handling, shell redirection,
pseudoterminal behavior, slow pipes, large files, resizing, job control,
and terminal mode restoration. Verify the final page on the kernel
console and tam-last. The only linked libraries are libc and libtam.

## 0.1.0

Add POSIX regex search, marks, horizontal scrolling, multiple-file
navigation, and follow mode.
