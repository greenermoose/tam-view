# more

more is a C terminal pager. This project uses the util-linux version's
behavior as a reference.

- Adopt (P1) scroll forward through the primary screen and leave viewed
  text in place when returning to the shell.
- Adopt (P1) return when the visible page reaches the end of input.
- Keep the backward movement and search commands familiar to less users.

References are the installed more(1) manual and `more --help`. These
notes describe behavior. Implementation is original to tam-view.
