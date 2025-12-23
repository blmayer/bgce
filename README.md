# BGCE: Brian's Graphical Computer Environment

```
	┌────────────────────────────────────┐
	│ BGCE.                              │
	│     |\                             │
	│      ^                             │
	│                   ┌───────┐        │
	│      ┌───────┐    │       │        │
	│      │       │    │       │        │
	│      │       │    │       │        │
	│      │       │    │       │        │
	│      └───────┘    └───────┘        │
	│                                    │
	└────────────────────────────────────┘
```

BGCE is a minimal graphical environment for Linux written in C. It acts as a bare-bones window system, similar in concept to Xorg or Wayland, but much simpler and designed to run **without root privileges**.


## Overview

The system is split into two components:

- **Server:** Manages video buffer, input events, and window stacking.
- **Client:** Sample application that renders its own buffer by requesting the server.

The server maintains:
- A **real display buffer** (the VGA framebuffer)
- A **per-client off-screen buffer**
- Basic **input handling** (keyboard + mouse)
- **IPC** using UNIX domain sockets for commands and input events

The client communicates with the server via a shared library exposing these APIs:

```c
ServerInfo getServerInfo(); // Returns buffer resolution, color depth, etc.
Buffer* getBuffer(int width, int height); // Returns a pointer to the client’s buffer
void draw(); // Requests server to draw the client buffer
```

I posted a video on my blog:

[Post](https://blog.terminal.pink/posts/bgce.html)


### Goals

- Minimal, understandable graphics stack in C
- No external dependencies (no OpenGL, no X11, no Wayland)
- Educational reference for framebuffer-based compositing and IPC


### Input
- The server listens to keyboard and mouse events.
- Focus determines which client receives input.


### Drawing
- Each client owns its own off-screen buffer.
- The server composites buffers into the display based on Z-order.


### Privileges
- BGCE must **run as a normal user** (no root, no setuid).
- The user should be in the **video** group to access `/dev/dri/card1` (framebuffer device).


### Event Loop
- Communication is **blocking** for events and **asynchronous** for draw requests.
- Future versions will include multiple clients and input focus management.


## Build Instructions

```bash
git clone <repo-url>
cd BGCE
make all client
./bgce   # Start server
# Go to a different tty
./client  # Start test client
```


## Configuration

BGCE supports configuration for the background through a config file. By default, it looks for `~/.config/bgce.conf`.


### Config File Format

```ini
[background]
# Background type: "color" or "image"
type = color

# For color background:
color = #RRGGBB   # Hex color code (e.g., #FF0000 for red)
# or
color = #RRGGBBAA # Hex color with alpha (e.g., #FF000080 for semi-transparent red)

# For image background:
#path = /path/to/image.png
#mode = tiled     # or "scaled"
```

### Example Config File

```ini
[background]
type = color
color = #336699
```

or

```ini
[background]
type = image
path = /usr/share/backgrounds/default.png
mode = scaled
```


### Notes

- Scaled images look horrible.


## Developing client applications

BGCE clients should write directly to the buffer
and call draw(), so the server will draw it to the
screen. When users resize the window an event is
sent to the client, applications then should adjust
its content and call `draw()`.

You are free to choose any libraries to help with
drawing graphical elements. To be honest I don't
know if the most common ones can directly handle
buffers. So I am creating a toolkit:
[BGTK](https://terminal.pink/bgtk/index.html), check it
out if you want.


## License

BSD 2-clause.
