# BGCE: Brian's Graphical Computer Environment

BGCE is a minimal graphical environment for Linux written in C. It acts as a bare-bones window system, similar in concept to Xorg or Wayland, but much simpler and designed to run **without root privileges**.

## Overview

The system is split into two components:

- **Server:** Manages video buffer, input events, and window stacking.
- **Client:** Applications that render to their own buffers and request drawing through the server.

The server maintains:
- A **real display buffer** (the VGA framebuffer)
- A **per-client off-screen buffer**
- Basic **input handling** (keyboard + mouse)
- **IPC** using UNIX domain sockets (for commands) and FIFOs (for input events)

The client communicates with the server via a shared library exposing these APIs:

```c
ServerInfo getServerInfo(); // Returns buffer resolution, color depth, etc.
Buffer* getBuffer(int width, int height); // Returns a pointer to the client’s buffer
void draw(int x, int y, int z); // Requests server to draw client buffer at position
bool getFocus(); // Checks if this client has input focus
void resizeBuffer(int width, int height); // Optional: resize client buffer
```

### Input
- The server listens to keyboard and mouse events.
- Input is written to a FIFO that clients can read from.
- Focus determines which client receives input.

### Drawing
- Each client owns its own off-screen buffer.
- The server composites buffers into the display based on Z-order.
- For now, **only one client** is supported.

### Privileges
- BGCE must **run as a normal user** (no root, no setuid).
- The user should be in the **video** group to access `/dev/fb0` (framebuffer device).

### Event Loop
- Communication is **blocking** for events and **asynchronous** for draw requests.
- Future versions will include multiple clients and input focus management.

## Build Instructions

```bash
git clone <repo-url>
cd BGCE
make
./bgced   # Start server
./bgce-client  # Start test client
```

## Goals

- Minimal, understandable graphics stack in C
- No external dependencies (no OpenGL, no X11, no Wayland)
- Educational reference for framebuffer-based compositing and IPC

## License

MIT License
