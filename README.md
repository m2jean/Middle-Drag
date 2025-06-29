A program to repurpose middle mouse button dragging as touch dragging.
Middle mouse clicks remains middle mouse clicks if there's no movement.

__Technical details__
* Need to ignore injected events when handling hooked events
* Need to periodically inject touch update events to keep the touch contact alive
* `mouse_event` (or `SendInput`) is very slow when called inside a low level input callback
