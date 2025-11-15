#include <fcntl.h>
#include <errno.h>
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

extern int setup_vt_handling(void);
extern struct ServerState server;

/* Globals (simple API) */
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

/* Initialize DRM: open card1, pick connector/mode, create & map dumb buffer, add FB and set CRTC */
int init_display(struct ServerState* srv) {
	srv->display.drm_fd = open("/dev/dri/card1", O_RDWR);
	if (srv->display.drm_fd < 0) {
		perror("open /dev/dri/card1");
		return -1;
	}

	if (setup_vt_handling() < 0) {
		return -1;
	}

	srv->display.resources = drmModeGetResources(srv->display.drm_fd);
	if (!srv->display.resources) {
		perror("drmModeGetResources");
		close(srv->display.drm_fd);
		return -1;
	}

	drmModeConnector* conn = NULL;
	drmModeEncoder* enc = NULL;

	for (int i = 0; i < srv->display.resources->count_connectors; i++) {
		conn = drmModeGetConnector(srv->display.drm_fd, srv->display.resources->connectors[i]);
		if (conn && conn->connection == DRM_MODE_CONNECTED) {
			enc = drmModeGetEncoder(srv->display.drm_fd, conn->encoder_id);
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

	srv->display.connector = conn;
	srv->display.encoder = enc;
	srv->display.crtc_id = enc->crtc_id;

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
	if (drmIoctl(srv->display.drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) {
		perror("DRM_IOCTL_MODE_CREATE_DUMB");
		return -1;
	}

	struct drm_mode_map_dumb mreq = {.handle = creq.handle};
	if (drmIoctl(srv->display.drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0) {
		perror("DRM_IOCTL_MODE_MAP_DUMB");
		return -1;
	}

	srv->display.framebuffer = mmap(0, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED,
	                                srv->display.drm_fd, mreq.offset);
	if (srv->display.framebuffer == MAP_FAILED) {
		perror("mmap");
		return -1;
	}

	if (drmModeAddFB(srv->display.drm_fd, srv->display.mode.hdisplay, srv->display.mode.vdisplay, 24, 32, pitch,
	                 creq.handle, &srv->display.fb_id)) {
		perror("drmModeAddFB");
		return -1;
	}

	if (drmModeSetCrtc(srv->display.drm_fd, srv->display.crtc_id, srv->display.fb_id, 0, 0,
	                   &srv->display.connector->connector_id, 1, &mode)) {
		perror("drmModeSetCrtc");
		return -1;
	}

	memset(srv->display.framebuffer, 0, fb_size);
	return 0;
}

/* Convert server RGB24 buffer to XRGB8888 and copy into drm_map */
void draw(struct ServerState* srv, struct Client cli) {
	if (!srv || !srv->display.framebuffer || !cli.buffer)
		return;

	uint32_t screen_w = srv->display.mode.hdisplay;
	uint32_t screen_h = srv->display.mode.vdisplay;

	uint32_t client_w = cli.width;
	uint32_t client_h = cli.height;

	int cx = cli.x;
	int cy = cli.y;

	uint32_t* dst = (uint32_t*)srv->display.framebuffer;
	uint32_t* src = (uint32_t*)cli.buffer;

	/* ---------------- Clip Region ---------------- */

	int start_x = cx < 0 ? 0 : cx;
	int start_y = cy < 0 ? 0 : cy;

	int end_x = cx + client_w;
	int end_y = cy + client_h;

	if (end_x > (int)screen_w)
		end_x = screen_w;
	if (end_y > (int)screen_h)
		end_y = screen_h;

	/* Entire window is outside screen */
	if (start_x >= end_x || start_y >= end_y)
		return;

	int src_start_x = start_x - cx;
	int src_start_y = start_y - cy;

	int copy_w = end_x - start_x;
	int copy_h = end_y - start_y;

	uint32_t screen_stride_pixels = screen_w; /* No stride stored → compute */

	/* ------------- Copy to DRM FB --------------- */

	for (int y = 0; y < copy_h; y++) {
		uint32_t* drow = dst + (start_y + y) * screen_stride_pixels + start_x;
		uint32_t* srow = src + (src_start_y + y) * client_w + src_start_x;
		memcpy(drow, srow, copy_w * 4);
	}

	/* ------------- Present via DRM ------------- */

	drmModeCrtc* crtc = drmModeGetCrtc(srv->display.drm_fd, srv->display.crtc_id);
	if (crtc) {
		int ret = drmModeSetCrtc(
		        srv->display.drm_fd,
		        srv->display.crtc_id,
		        srv->display.fb_id,
		        0, 0,
		        &srv->display.connector_id, 1,
		        &crtc->mode);
		drmModeFreeCrtc(crtc);

		if (ret < 0) {
			fprintf(stderr, "[BGCE] drmModeSetCrtc failed in draw(): %s\n",
			        strerror(errno));
		}
	}
}

/* Shutdown and cleanup */
void bgce_display_shutdown(void) {
	if (server.display.drm_fd < 0)
		return;

	if (drm_map) {
		munmap(drm_map, drm_map_size);
		drm_map = NULL;
	}
	if (drm_fb)
		drmModeRmFB(server.display.drm_fd, drm_fb);

	if (drm_handle) {
		struct drm_mode_destroy_dumb dreq;
		memset(&dreq, 0, sizeof(dreq));
		dreq.handle = drm_handle;
		ioctl(server.display.drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
	}
	drmDropMaster(server.display.drm_fd);

	close(server.display.drm_fd);
	server.display.drm_fd = -1;
	drm_fb = 0;
	drm_handle = 0;
	drm_map_size = 0;
	printf("[BGCE][DRM] shutdown complete\n");
}
