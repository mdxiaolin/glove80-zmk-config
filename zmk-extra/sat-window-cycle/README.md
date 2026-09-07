# SAT window cycle behavior

`zmk,behavior-sat-window-cycle` implements the PACS Super Alt-Tab state
machine used by the Glove80 keymap. The first tap presses left Alt and taps
Tab. Each further tap refreshes a one-second release window and taps Tab. A
physical key press cancels the window before normal keycode dispatch; the
configured Shift positions are ignored so Shift+SAT sends reverse Alt+Tab.

The behavior tracks whether it pressed Alt itself. Timeout, another-key
cancellation, layer deactivation, and queue cleanup release only that owned
press, preserving a physical Alt modifier held by the user.
