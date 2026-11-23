# B3 — External links open on one key press, and failures report success

- **Status:** Fix available
- **Severity:** medium
- **Affected versions:** Confirmed in 0.4.0
- **Fixed in:** 0.4.1 (source)
- **Reported:** 2026-11-22, agy (Claude Opus 5.5)
- **Found by:** [R1](../reviews/R1-pager-renderers-and-libtam-use.md)

## What happens

With an external link focused, one press of Right, Enter or `o` starts
`xdg-open`. Right also scrolls horizontally when no link is focused, so a
reader can start a browser while meaning to scroll. The status line says
"Opened: <url>" even when `xdg-open` is missing or fails, as on a Linux
VT without a browser.

## What should happen

Opening an external program asks for confirmation, for example a second
press, as tam does. A failure to start the opener is reported.

## Steps to reproduce

Open a Markdown file containing `[site](https://example.invalid)` with
`tam-view -R`. Move the attention line onto the link and press Right. To
see the false success, start tam-view with a `PATH` that contains no
`xdg-open`. The status line still says "Opened:".

## Cause

`follow_focused_link` calls `tam_uri_open_external` directly. The libtam
function double-forks, reaps only the intermediate child, and returns 0
whatever happens to the exec.
[follow_focused_link](../src/pager.c#L1087-L1096)

## Fix and verification

Ask for a second press before handing a URL over. Have libtam report an
exec failure through a close-on-exec pipe, and start the opener in its
own session. Test with a fake `xdg-open` on `PATH` in the PTY suite.
