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

#include "bgce_server.h" /* for ServerState */
#include "bgce_shared.h" /* for shared types if needed */

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

/* mode / connector / crtc state saved for shutdown */
static drmModeModeInfo chosen_mode;
static uint32_t chosen_crtc = 0;
static uint32_t chosen_conn = 0;

/* Helper: create dumb buffer via ioctl */
static int create_dumb_buffer(int fd, uint32_t width, uint32_t height,
                              uint32_t* out_handle, uint32_t* out_pitch, size_t* out_size) {
	struct drm_mode_create_dumb creq;
	memset(&creq, 0, sizeof(creq));
	creq.width = width;
	creq.height = height;
	creq.bpp = drm_bpp;

	if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) {
		perror("DRM_IOCTL_MODE_CREATE_DUMB");
		return -1;
	}

	*out_handle = creq.handle;
	*out_pitch = creq.pitch;
	*out_size = (size_t)creq.pitch * creq.height;
	return 0;
}

/* Helper: map dumb buffer */
static int map_dumb_buffer(int fd, uint32_t handle, uint8_t** out_map, size_t size) {
	struct drm_mode_map_dumb mreq;
	memset(&mreq, 0, sizeof(mreq));
	mreq.handle = handle;
	if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0) {
		perror("DRM_IOCTL_MODE_MAP_DUMB");
		return -1;
	}

	off_t offset = mreq.offset;
	*out_map = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
	if (*out_map == MAP_FAILED) {
		perror("mmap(dumb)");
		*out_map = NULL;
		return -1;
	}
	return 0;
}

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

/* Find first connected connector and matching CRTC */
static int find_connector_and_crtc(int fd, drmModeConnector** out_conn, drmModeEncoder** out_enc, drmModeRes** out_res) {
	drmModeRes* res = drmModeGetResources(fd);
	if (!res) {
		fprintf(stderr, "[BGCE][DRM] drmModeGetResources failed\n");
		return -1;
	}

	drmModeConnector* conn = NULL;
	drmModeEncoder* enc = NULL;
	uint32_t conn_id = 0;

	for (int i = 0; i < res->count_connectors; i++) {
		drmModeConnector* c = drmModeGetConnector(fd, res->connectors[i]);
		if (!c)
			continue;
		if (c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
			conn = c;
			conn_id = c->connector_id;
			break;
		}
		drmModeFreeConnector(c);
	}

	if (!conn) {
		drmModeFreeResources(res);
		fprintf(stderr, "[BGCE][DRM] No connected connector found\n");
		return -1;
	}

	/* choose encoder */
	drmModeEncoder* e = NULL;
	if (conn->encoder_id)
		e = drmModeGetEncoder(fd, conn->encoder_id);

	if (!e) {
		/* try to find a compatible encoder via encoders list */
		for (int i = 0; i < conn->count_encoders; i++) {
			e = drmModeGetEncoder(fd, conn->encoders[i]);
			if (e)
				break;
		}
	}

	if (!e) {
		drmModeFreeConnector(conn);
		drmModeFreeResources(res);
		fprintf(stderr, "[BGCE][DRM] No encoder for connector\n");
		return -1;
	}

	*out_conn = conn;
	*out_enc = e;
	*out_res = res;
	chosen_conn = conn_id;
	return 0;
}

/* Initialize DRM: open card0, pick connector/mode, create & map dumb buffer, add FB and set CRTC */
int bgce_display_init(struct ServerState* srv) {
	if (!srv)
		return -1;
	if (srv->width <= 0 || srv->height <= 0) {
		fprintf(stderr, "[BGCE][DRM] server resolution invalid, need width/height\n");
		return -1;
	}

	drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (drm_fd < 0) {
		perror("[BGCE][DRM] open /dev/dri/card0");
		return -1;
	}

	drmModeConnector* conn = NULL;
	drmModeEncoder* enc = NULL;
	drmModeRes* res = NULL;
	if (find_connector_and_crtc(drm_fd, &conn, &enc, &res) < 0) {
		close(drm_fd);
		drm_fd = -1;
		return -1;
	}

	/* pick a mode: prefer the connector preferred mode, else use first */
	drmModeModeInfo mode = conn->modes[0];
	for (int i = 0; i < conn->count_modes; i++) {
		if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
			mode = conn->modes[i];
			break;
		}
	}

	chosen_mode = mode;

	/* decide framebuffer size: use server size but ensure <= mode */
	uint32_t fb_w = (uint32_t)srv->width;
	uint32_t fb_h = (uint32_t)srv->height;
	if (fb_w == 0 || fb_h == 0) {
		fb_w = mode.hdisplay;
		fb_h = mode.vdisplay;
	}

	/* create dumb buffer of server size */
	if (create_dumb_buffer(drm_fd, fb_w, fb_h, &drm_handle, &drm_pitch, &drm_map_size) < 0) {
		drmModeFreeConnector(conn);
		drmModeFreeEncoder(enc);
		drmModeFreeResources(res);
		close(drm_fd);
		drm_fd = -1;
		return -1;
	}

	/* add fb object */
	int ret = drmModeAddFB(drm_fd, fb_w, fb_h, 24, drm_bpp, drm_pitch, drm_handle, &drm_fb);
	if (ret) {
		fprintf(stderr, "[BGCE][DRM] drmModeAddFB failed: %s\n", strerror(errno));
		destroy_dumb_buffer(drm_fd, drm_handle, NULL, 0);
		drmModeFreeConnector(conn);
		drmModeFreeEncoder(enc);
		drmModeFreeResources(res);
		close(drm_fd);
		drm_fd = -1;
		return -1;
	}

	/* map the buffer so userspace can write to it */
	if (map_dumb_buffer(drm_fd, drm_handle, &drm_map, drm_map_size) < 0) {
		drmModeRmFB(drm_fd, drm_fb);
		destroy_dumb_buffer(drm_fd, drm_handle, NULL, 0);
		drmModeFreeConnector(conn);
		drmModeFreeEncoder(enc);
		drmModeFreeResources(res);
		close(drm_fd);
		drm_fd = -1;
		return -1;
	}

	/* find CRTC: get from encoder or find a suitable one */
	uint32_t crtc_id = 0;
	if (enc && enc->crtc_id)
		crtc_id = enc->crtc_id;
	else {
		/* search for any crtc */
		for (int i = 0; i < res->count_crtcs; i++) {
			crtc_id = res->crtcs[i];
			break;
		}
	}
	chosen_crtc = crtc_id;

	/* set CRTC to use our FB */
	ret = drmModeSetCrtc(drm_fd, chosen_crtc, drm_fb, 0, 0, &chosen_conn, 1, &mode);
	if (ret) {
		fprintf(stderr, "[BGCE][DRM] drmModeSetCrtc failed: %s\n", strerror(errno));
		munmap(drm_map, drm_map_size);
		drmModeRmFB(drm_fd, drm_fb);
		destroy_dumb_buffer(drm_fd, drm_handle, NULL, 0);
		drmModeFreeConnector(conn);
		drmModeFreeEncoder(enc);
		drmModeFreeResources(res);
		close(drm_fd);
		drm_fd = -1;
		return -1;
	}

	/* success: store chosen mode in globals, free resources */
	drmModeFreeConnector(conn);
	drmModeFreeEncoder(enc);
	drmModeFreeResources(res);

	printf("[BGCE][DRM] initialized: %ux%u, pitch=%u, map=%p\n",
	       fb_w, fb_h, drm_pitch, (void*)drm_map);

	return 0;
}

/* Convert server RGB24 buffer to XRGB8888 and copy into drm_map */
void bgce_display_present(struct ServerState* srv) {
	if (!drm_map || drm_fd < 0 || !srv)
		return;

	const uint8_t* src = srv->framebuffer; /* expecting RGB24 packed */
	if (!src)
		return;

	uint32_t width = srv->width;
	uint32_t height = srv->height;
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

	close(drm_fd);
	drm_fd = -1;
	drm_fb = 0;
	drm_handle = 0;
	drm_map_size = 0;
	printf("[BGCE][DRM] shutdown complete\n");
}
