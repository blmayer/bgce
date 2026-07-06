/*
 * Linux framebuffer (/dev/fb0) display backend — default, no libdrm.
 */
#include "server.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

extern struct ServerState server;

static struct fb_var_screeninfo saved_vinfo;
static int have_saved_vinfo;

int init_display(void)
{
	static const char *paths[] = {
		"/dev/fb0", "/dev/fb1", "/dev/fb2", NULL
	};
	int fd = -1;
	const char *used = NULL;
	int i;

	{
		int saw_perm = 0;
		int saw_missing = 0;

		for (i = 0; paths[i]; i++) {
			fd = open(paths[i], O_RDWR | O_CLOEXEC);
			if (fd >= 0) {
				used = paths[i];
				break;
			}
			/* Must not fail silently: after setup_log_file, only the log
			 * would see perror unless we also write the real console. */
			bgce_announce("[BGCE] open %s: %s\n", paths[i], strerror(errno));
			if (errno == EACCES || errno == EPERM)
				saw_perm = 1;
			else if (errno == ENOENT)
				saw_missing = 1;
		}
		if (fd < 0) {
			bgce_announce(
			        "[BGCE] failed to open any of /dev/fb0, /dev/fb1, /dev/fb2\n");
			if (saw_perm) {
				bgce_announce(
				        "[BGCE] no permission for the framebuffer device.\n"
				        "[BGCE] add your user to the 'video' group, then re-login:\n"
				        "[BGCE]   sudo usermod -aG video \"$USER\"\n"
				        "[BGCE] and check: ls -l /dev/fb0\n");
			} else if (saw_missing) {
				bgce_announce(
				        "[BGCE] no framebuffer device node found "
				        "(is fbdev available on this system?)\n");
			}
			return 1;
		}
	}

	struct fb_var_screeninfo vinfo;
	struct fb_fix_screeninfo finfo;
	memset(&vinfo, 0, sizeof(vinfo));
	memset(&finfo, 0, sizeof(finfo));
	if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
		bgce_announce("[BGCE] FBIOGET_VSCREENINFO on %s: %s\n", used,
		              strerror(errno));
		close(fd);
		return 1;
	}
	if (ioctl(fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
		bgce_announce("[BGCE] FBIOGET_FSCREENINFO on %s: %s\n", used,
		              strerror(errno));
		close(fd);
		return 1;
	}
	saved_vinfo = vinfo;
	have_saved_vinfo = 1;

	if (vinfo.bits_per_pixel != 32) {
		vinfo.bits_per_pixel = 32;
		vinfo.xoffset = 0;
		vinfo.yoffset = 0;
		if (ioctl(fd, FBIOPUT_VSCREENINFO, &vinfo) < 0) {
			bgce_announce(
			        "[BGCE] need 32bpp framebuffer (have %u): %s\n",
			        saved_vinfo.bits_per_pixel, strerror(errno));
			close(fd);
			return 1;
		}
		if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
		    ioctl(fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
			perror("[BGCE] re-read screen info");
			close(fd);
			return 1;
		}
	}

	if (vinfo.bits_per_pixel != 32) {
		bgce_announce("[BGCE] unsupported bpp %u (want 32)\n",
		              (unsigned)vinfo.bits_per_pixel);
		close(fd);
		return 1;
	}

	size_t visible = (size_t)finfo.line_length * (size_t)vinfo.yres;
	size_t size = (size_t)finfo.smem_len;
	if (size < visible)
		size = visible;

	void *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		bgce_announce("[BGCE] mmap %s: %s\n", used, strerror(errno));
		close(fd);
		return 1;
	}

	server.display_fd = fd;
	server.framebuffer = map;
	server.fb_size = size;
	server.display_w = vinfo.xres;
	server.display_h = vinfo.yres;
	server.display_bpp = vinfo.bits_per_pixel;
	server.display_pitch = finfo.line_length;
	memset(map, 0, visible);

	setup_vt_handling();

	printf("[BGCE] fbdev %s %ux%u pitch=%u bpp=%u\n",
	       used, server.display_w, server.display_h,
	       server.display_pitch, server.display_bpp);

	if (display_cursor_init() != 0) {
		bgce_announce("[BGCE] software cursor init failed\n");
		release_display();
		return 1;
	}
	return 0;
}

void release_display(void)
{
	release_vt();
	display_cursor_fini();

	if (server.framebuffer && server.framebuffer != MAP_FAILED) {
		munmap(server.framebuffer, server.fb_size);
		server.framebuffer = NULL;
		server.fb_size = 0;
	}
	if (server.display_fd >= 0) {
		if (have_saved_vinfo)
			(void)ioctl(server.display_fd, FBIOPUT_VSCREENINFO, &saved_vinfo);
		close(server.display_fd);
		server.display_fd = -1;
	}
	have_saved_vinfo = 0;
	printf("[BGCE] Display released.\n");
}
