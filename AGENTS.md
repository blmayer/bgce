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

### BGTK (Brian's Graphical Toolkit)

[x] **Step 1: Analyze Client API**
    *   Check `bgce.h` to see what functions are available for client connection, buffer retrieval, and especially **event handling**.
    *   Check `client.c` to see how an application currently interacts with `libbgce.so`.

[ ] **Step 2: Define BGTK Interface (`bgtk.h`)**
    * Define the core structure for a widget (`struct BGTK_Widget`).
    * Define the event structure (`struct BGTK_Event`).
    [ ] Improve widget structure: add options (like alignment), child widgets.
    * Define API functions:
        * `BGTK_Context *bgtk_init(void)`: Connects to BGCE server, gets buffer/dimensions.
        * `void bgtk_main_loop(BGTK_Context *ctx)`: Blocking loop to handle events and redraws.
        * `BGTK_Widget *bgtk_label_new(const char *text)`: Creates a label.
        * `BGTK_Widget *bgtk_button_new(const char *text, void (*callback)(void))`
        * `void bgtk_add_widget(BGTK_Context *ctx, BGTK_Widget *widget, int x, int y, int w, int h)`: Simple absolute positioning for now.

[ ] **Step 3: Implement BGTK Core (`bgtk.c`)**
    [X] Implement initialization, event queueing, and simple drawing (e.g., drawing rectangles for buttons, text rendering).
    [ ] Only call draw when changes are made, like input.
    [ ] Implement proper hit detection: using widget trees and coordinates, e.g. click on x,y -> search the tree until last widget is found, then send the input to that widget.

[x] **Step 4: Integrate and Test**
    [X] Create a new client file (or update `client.c`) to demonstrate a basic BGTK application. -> app.c
    [ ] Ensure events (like mouse clicks) are routed to the appropriate widget callbacks. This needs server-side changes.

---

Author: Brian Mayer
Project: BGCE (Brian’s Graphical Computer Environment)
