# BGCE Project: Design and Implementation Notes

## Objective

Implement a minimal Linux graphical environment that runs without root, manages a framebuffer directly, and provides a simple IPC interface for clients to draw.

## Design Decisions

- **Language:** C
- **Privilege Model:** Runs as user in `video` group; no `setuid` or `sudo` required.
- **Display Backend:** Uses `/dev/fb0` (Linux framebuffer device) directly. Optional `BACKEND=drm` uses libdrm/KMS.
- **Architecture:**
  - `bgced`: server that manages framebuffers, input, and client communication.
  - `libbgce.so`: client library that handles IPC and buffer management.
  - `bgce-client`: sample program using the library.
- **IPC:** UNIX domain sockets for commands, FIFO for input events.
- **Buffers:** Each client has an off-screen buffer; server composites into real framebuffer.
- **Rendering:** CPU-based memory blitting only (no OpenGL/DRI accel for now).
- **MSG_DRAW:** Blit only the client that asked, clipped by opaque windows
  above (subtract their rects). Do **not** full-stack recompose wallpaper /
  clients below on ordinary draw — that was a regression (blink/refill).
- **Window move:** expose = old screen rect \\ new (L when translating);
  underlay those pieces with every client *except* the mover (bottom→top,
  always includes wallpaper); then full blit of the mover at the new place.
  No FB scroll of the body; no underlay under the mover’s new footprint.
  Full stack remains for erase / pan edges / zoom.
- **Focus Handling:** Server tracks which client receives keyboard/mouse events.
- **Event Model:** Blocking reads (no polling); async draw calls.
- **Security:** Clients are sandboxed via file descriptors; no root operations.
- **Future Work:**
  - Multi-client stacking and Z-order
  - Input event routing
  - Efficient dirty-region drawing
  - Minimal window manager support


## Headless / mock tests (like BGTK)

No framebuffer or input devices required. Develop and verify compositor
behaviour by inspecting PNGs:

```bash
make headless && ./headless
# writes headless_00_bg.png … headless_14_alt_shift_tab.png
```

API (see `mock.h`): `bgce_mock_init`, `bgce_mock_add_client`, `bgce_mock_draw`,
`bgce_mock_click`, `bgce_mock_move`, `bgce_mock_alt_tab`, `bgce_mock_set_viewport`,
`bgce_mock_screenshot`, `bgce_mock_fini`.

On macOS, `compat/linux/` provides stub headers so headless builds without
kernel headers. Real `bgce` still requires Linux.

## Current Milestone

### High-evel features
[X] Implement config file
[X] Implement taking screenshots
[X] Implement zoomable/pannable virtual desktop (4× area, zoom 50%–400%)
[X] Headless mock + screenshot tests (`make headless`)
[X] Cache file to memorize last client locations (`~/.cache/bgce/windows.cache`)

### Protocol features
[X] Add suport for moving a buffer at a specific location
[X] Add suport for resizing a buffer -> clients should just ask for a buffer, the same way

### Internal stuff
[ ] Create more tests
  - [ ] test if events go to the correct client and only the one
  - [X] headless stacking / move / erase screenshots
  - [X] headless location-cache restore
  - [X] headless Alt+Tab / viewport restore

### Location cache
- File: `$XDG_CACHE_HOME/bgce/windows.cache` or `~/.cache/bgce/windows.cache`
- Format: `app_id x y [zoom_pct pan_x pan_y]` (world x/y; optional viewport)
- Identity: Linux `SO_PEERCRED` + `/proc/pid/comm` (fallback `"client"`)
- Restore position on first buffer request; restore viewport on Alt+Tab
- Save on move end, MSG_MOVE, disconnect, and when leaving a window via Alt+Tab
- **Desktop pan alone does not write the cache**
- Same binary name shares one slot (last place wins)

### Zoom / pan controls
- **Alt + scroll**: zoom in/out toward the cursor (integer percent, 50%–400%, step 10%; 100% = 1:1)
- **Alt + left-drag on empty space**: pan the viewport over the virtual desktop
- **Alt + left-drag on a client**: move window (existing)
- **Alt + right-drag on a client**: resize window (existing)
- **Alt + Tab** / **Alt + Shift + Tab**: cycle focus (raise app); pan + zoom to
  that app’s last viewport so it appears where it was left (does not move the
  window rect). Shift reverses the cycle.
- Virtual desktop is 2× the physical resolution in each axis (4× area). Client
  positions and buffer sizes are in world pixels; the compositor scales to the
  screen using the current zoom and pan.


---

Author: Brian Mayer
Project: BGCE (Brian’s Graphical Computer Environment)
