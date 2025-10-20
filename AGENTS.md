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

Phase 1: Single-client prototype with framebuffer output and working IPC communication.

### Components to Implement First

1. `bgced` server skeleton:
   - Open `/dev/fb0`
   - Create UNIX socket for client connections
   - Manage shared memory for client buffers
   - Composite and blit to display buffer

2. `libbgce.so`:
   - Connect to server
   - Allocate shared buffer
   - Implement API stubs (`getServerInfo`, `getBuffer`, `draw`)

3. `bgce-client`:
   - Simple test: fill buffer with color, send draw request

---

Author: Brian Mayer
Project: BGCE (Brian’s Graphical Computer Environment)
