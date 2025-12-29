# BGCE Project: Design and Implementation Notes

## Objective

Implement a minimal Linux graphical environment that runs without root, manages a framebuffer directly, and provides a simple IPC interface for clients to draw.

## Design Decisions

- **Language:** C
- **Privilege Model:** Runs as user in `video` group; no `setuid` or `sudo` required.
- **Display Backend:** Uses `/dev/fb0` (Linux framebuffer device) directly.
- **Architecture:**
  - `bgced`: server that manages framebuffers, input, and client communication.
  - `libbgce.so`: client library that handles IPC and buffer management.
  - `bgce-client`: sample program using the library.
- **IPC:** UNIX domain sockets for commands, FIFO for input events.
- **Buffers:** Each client has an off-screen buffer; server composites into real framebuffer.
- **Rendering:** CPU-based memory blitting only (no OpenGL/DRI accel for now).
- **Focus Handling:** Server tracks which client receives keyboard/mouse events.
- **Event Model:** Blocking reads (no polling); async draw calls.
- **Security:** Clients are sandboxed via file descriptors; no root operations.
- **Future Work:**
  - Multi-client stacking and Z-order
  - Input event routing
  - Efficient dirty-region drawing
  - Minimal window manager support


## Current Milestone

### High-evel features
[X] Implement config file
[X] Implement taking screenshots
[ ] Implement a cache file to memorize the last location of clients, in order to reopen them on the same place

### Protocol features
[X] Add suport for moving a buffer at a specific location
[X] Add suport for resizing a buffer -> clients should just ask for a buffer, the same way

### Internal stuff
[ ] Create more tests
  - [ ] test if events go to the correct client and only the one


---

Author: Brian Mayer
Project: BGCE (Brian’s Graphical Computer Environment)
