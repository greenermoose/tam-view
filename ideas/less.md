# less

less is a C terminal pager for files and command output.

- Adopt (P1) separate keys for line, half-page and page motion in both
  directions, direct line jumps, and repeatable searches.
- Consider (0.1.0) regex search, named positions, and follow mode.
- Reject a screen switch as the default. Keep the selected page visible
  when returning to the prompt.

References are the installed less(1) manual and `less --help`. These
notes describe behavior. Implementation is original to tam-view.
