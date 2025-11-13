#define _POSIX_C_SOURCE 200112L
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "server.h" /* for ServerState */

/*
 * Simple DRM/KMS display backend using a dumb buffer.
 *
 * Notes:
 *  - Assumes the user has permission to open /dev/dri/card0 (video group).
 *  - Uses XRGB8888 framebuffer format (32bpp) and converts incoming RGB24.
 *  - Performs an initial modeset and then writes into the mapped buffer on present.
 *  - This is single-buffered and uses drmModeSetCrtc for the initial commit.
 *    For smoother presentation, page-flip + double buffering + atomic are recommended.
 */

/* Globals (simple API) */
static int drm_fd = -1;
static uint32_t drm_fb = 0;
static uint32_t drm_handle = 0;
static uint32_t drm_pitch = 0;
static uint32_t drm_bpp = 32;
static uint8_t* drm_map = NULL;
static size_t drm_map_size = 0;

/* Helper: destroy dumb buffer (unmap + ioctl destroy) */
static void destroy_dumb_buffer(int fd, uint32_t handle, uint8_t* map, size_t size) {
	if (map) {
		munmap(map, size);
	}
	if (handle) {
		struct drm_mode_destroy_dumb dreq;
		memset(&dreq, 0, sizeof(dreq));
		dreq.handle = handle;
		ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
	}
}

/* Initialize DRM: open card0, pick connector/mode, create & map dumb buffer, add FB and set CRTC */
int init_display(struct ServerState* srv) {
	srv->drm_fd = open("/dev/dri/card1", O_RDWR);
	if (srv->drm_fd < 0) {
		perror("open /dev/dri/card1");
		return -1;
	}

	srv->resources = drmModeGetResources(srv->drm_fd);
	if (!srv->resources) {
		perror("drmModeGetResources");
		close(srv->drm_fd);
		return -1;
	}

	drmModeConnector* conn = NULL;
	drmModeEncoder* enc = NULL;

	for (int i = 0; i < srv->resources->count_connectors; i++) {
		conn = drmModeGetConnector(srv->drm_fd, srv->resources->connectors[i]);
		if (conn && conn->connection == DRM_MODE_CONNECTED) {
			enc = drmModeGetEncoder(srv->drm_fd, conn->encoder_id);
			if (enc)
				break;
			drmModeFreeConnector(conn);
			conn = NULL;
		} else if (conn) {
			drmModeFreeConnector(conn);
		}
	}

	if (!conn || !enc) {
		fprintf(stderr, "[BGCE] No connected display found\n");
		return -1;
	}

	srv->connector = conn;
	srv->encoder = enc;
	srv->crtc_id = enc->crtc_id;

	drmModeModeInfo mode = conn->modes[0];
	srv->display.mode.hdisplay = mode.hdisplay;
	srv->display.mode.vdisplay = mode.vdisplay;
	srv->color_depth = 32;

	printf("[BGCE] Using display %dx%d (%s)\n",
	       mode.hdisplay, mode.vdisplay, mode.name);

	uint32_t handle;
	uint32_t pitch = srv->display.mode.hdisplay * 4;
	size_t fb_size = pitch * srv->display.mode.vdisplay;

	struct drm_mode_create_dumb creq = {0};
	creq.width = srv->display.mode.hdisplay;
	creq.height = srv->display.mode.vdisplay;
	creq.bpp = 32;
	if (drmIoctl(srv->drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) {
		perror("DRM_IOCTL_MODE_CREATE_DUMB");
		return -1;
	}

	struct drm_mode_map_dumb mreq = {.handle = creq.handle};
	if (drmIoctl(srv->drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0) {
		perror("DRM_IOCTL_MODE_MAP_DUMB");
		return -1;
	}

	srv->display.framebuffer = mmap(0, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED,
	                        srv->drm_fd, mreq.offset);
	if (srv->display.framebuffer == MAP_FAILED) {
		perror("mmap");
		return -1;
	}

	if (drmModeAddFB(srv->drm_fd, srv->display.mode.hdisplay, srv->display.mode.vdisplay, 24, 32, pitch,
	                 creq.handle, &srv->fb_id)) {
		perror("drmModeAddFB");
		return -1;
	}

	if (drmModeSetCrtc(srv->drm_fd, srv->crtc_id, srv->fb_id, 0, 0,
	                   &srv->connector->connector_id, 1, &mode)) {
		perror("drmModeSetCrtc");
		return -1;
	}

	memset(srv->display.framebuffer, 0, fb_size);
	return 0;
}

/* Convert server RGB24 buffer to XRGB8888 and copy into drm_map */
void draw(struct ServerState* srv) {
	if (!drm_map || drm_fd < 0 || !srv)
		return;

	const uint8_t* src = srv->display.framebuffer; /* expecting RGB24 packed */
	if (!src)
		return;

	uint32_t width = srv->display.mode.hdisplay;
	uint32_t height = srv->display.mode.vdisplay;
	uint32_t pitch = drm_pitch;

	/* clamp to created buffer size */
	uint32_t copy_w = width;
	uint32_t copy_h = height;
	/* we don't have stored fb_w/fb_h separately; assume compatible */

	for (uint32_t y = 0; y < copy_h; y++) {
		uint8_t* dst = drm_map + y * pitch;
		const uint8_t* s = src + (y * width * 3);
		for (uint32_t x = 0; x < copy_w; x++) {
			uint8_t r = s[0];
			uint8_t g = s[1];
			uint8_t b = s[2];
			/* XRGB8888 (skip alpha) */
			uint32_t px = (r << 16) | (g << 8) | b;
			((uint32_t*)dst)[x] = px;
			s += 3;
		}
	}
	/* If you want, explicit flush or drmModePageFlip can be used here */
	/* This simple program writes into the scanout buffer directly. */
}

/* Shutdown and cleanup */
void bgce_display_shutdown(void) {
	if (drm_fd < 0)
		return;

	if (drm_map) {
		munmap(drm_map, drm_map_size);
		drm_map = NULL;
	}
	if (drm_fb)
		drmModeRmFB(drm_fd, drm_fb);

	if (drm_handle) {
		struct drm_mode_destroy_dumb dreq;
		memset(&dreq, 0, sizeof(dreq));
		dreq.handle = drm_handle;
		ioctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
	}
	drmDropMaster(drm_fd);

	close(drm_fd);
	drm_fd = -1;
	drm_fb = 0;
	drm_handle = 0;
	drm_map_size = 0;
	printf("[BGCE][DRM] shutdown complete\n");
}
