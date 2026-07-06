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

But talk is cheap, BGCE now can take screenshots, so here's one:

![](https://terminal.pink/bgce/screenshot.png)


I also posted a video on my blog:

[Post](https://blog.terminal.pink/posts/bgce.html)


### Goals

- Minimal, understandable graphics stack in C
- No external dependencies by default (no OpenGL, X11, Wayland, or libdrm)
- Educational reference for framebuffer-based compositing and IPC


### Input
- The server listens to keyboard and mouse events.
- Focus determines which client receives input.
- **Zoom / pan (virtual desktop):**
  - `Alt` + scroll wheel — zoom in/out (50%–400%), centered on the cursor
  - `Alt` + left-drag on empty desktop — pan the viewport
  - The virtual desktop is **2×** the physical resolution in each axis (**4×** the screen area), so you can place work areas far apart and zoom into focus when needed


### Drawing
- Each client owns its own off-screen buffer (sizes and positions are in virtual-desktop / world pixels).
- The server composites buffers into the display based on Z-order, scaled by the current zoom and pan.


### Privileges
- BGCE must **run as a normal user** (no root, no setuid).
- The user should be in the **video** group to access `/dev/fb0` (framebuffer device).


### Event Loop
- Communication is **blocking** for events and **asynchronous** for draw requests.
- Future versions will include multiple clients and input focus management.


## Download

A source snapshot is published with the project site:

- **[bgce.tar.gz](https://terminal.pink/bgce/bgce.tar.gz)** — current tree (no git history)

```bash
curl -fsSL -o bgce.tar.gz https://terminal.pink/bgce/bgce.tar.gz
tar xzf bgce.tar.gz
cd bgce
```

Or clone the repository if you prefer full history:

```bash
git clone https://terminal.pink/bgce
cd bgce
```

## Build Instructions

On lin0 use `/bin/cc` and install into the flat root (`/bin`, `/lib`, `/include` — no `/usr`):

```bash
make CC=cc all client          # default: /dev/fb0, no libdrm
# make CC=cc BACKEND=drm all   # optional DRM/KMS + libdrm
make install
./bgce   # Start server
# Go to a different tty
./client  # Start test client
```

Default build needs only kernel headers (`linux/fb.h`) and `/dev/fb0`
(DRM fbdev emulation is fine). `BACKEND=drm` needs libdrm.

Regenerate the site snapshot after tagging a release (or anytime):

```bash
git archive --worktree-attributes --format=tar.gz --prefix=bgce/ -o www/bgce.tar.gz HEAD
```

(`www/bgce.tar.gz` and `www/screenshot.png` are omitted from the archive via `.gitattributes`.)


## Configuration

BGCE supports configuration for the background through a config file. By default, it looks for `~/.config/bgce.conf`.

## Logging

All server messages are written to a default log file:

- `$XDG_CACHE_HOME/bgce/bgce.log` (if `XDG_CACHE_HOME` is set)
- otherwise `~/.cache/bgce/bgce.log`

The path is printed to the terminal you used to start `bgce` so you know where to look (`tail -f` it, etc.). All `[BGCE]` output after that point goes into the log file.


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

[shortcuts]
# Define keyboard shortcuts. Format:
# <combo> = <type>:<value>
#
# type is one of:
#   builtin  - value must be "exit" or "screenshot"
#   command  - value is a shell command to run in a new process
#
# Combo examples: ctrl+alt+q, sysrq, f12, ctrl+shift+s, super+r

# Defaults (active when no [shortcuts] section is present or it is empty):
# ctrl+alt+q = builtin:exit
# sysrq = builtin:screenshot
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

With custom shortcuts:

```ini
[shortcuts]
ctrl+alt+q = builtin:exit
sysrq = builtin:screenshot

# Launch programs (commands run via /bin/sh -c in a separate process)
ctrl+alt+t = command:alacritty
super+r = command:rofi -show drun
f12 = command:sh -c 'import -window root ~/screenshot.png'
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
