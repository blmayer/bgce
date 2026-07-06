/*
 * DRM/KMS display backend (optional). Build with BACKEND=drm / -DBGCE_USE_DRM.
 * Still uses software cursor from display.c (no libdrm cursor ioctls required
 * beyond modeset). Links -ldrm.
 */
#include "server.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__has_include)
#  if __has_include(<libdrm/drm.h>)
#    include <libdrm/drm.h>
#    include <libdrm/drm_mode.h>
#  else
#    include <drm/drm.h>
#    include <drm/drm_mode.h>
#  endif
#else
#  include <drm/drm.h>
#  include <drm/drm_mode.h>
#endif
#include <xf86drm.h>
#include <xf86drmMode.h>

extern struct ServerState server;

static uint32_t conn_id;
static uint32_t fb_id;
static uint32_t scanout_handle;
static uint64_t scanout_size;
static drmModeConnector *connector;
static drmModeRes *resources;
static drmModeEncoder *encoder;
static drmModeCrtc *saved_crtc;

static int drm_create_dumb(int fd, uint32_t width, uint32_t height, uint32_t bpp,
                           struct drm_mode_create_dumb *create)
{
	memset(create, 0, sizeof(*create));
	create->width = width;
	create->height = height;
	create->bpp = bpp;
	if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, create) < 0) {
		perror("[BGCE] DRM_IOCTL_MODE_CREATE_DUMB");
		return -1;
	}
	return 0;
}

static int drm_map_dumb(int fd, uint32_t handle, uint64_t *offset)
{
	struct drm_mode_map_dumb map = {0};
	map.handle = handle;
	if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
		perror("[BGCE] DRM_IOCTL_MODE_MAP_DUMB");
		return -1;
	}
	*offset = map.offset;
	return 0;
}

static int drm_destroy_dumb(int fd, uint32_t handle)
{
	struct drm_mode_destroy_dumb dest = {0};
	dest.handle = handle;
	if (ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dest) < 0) {
		perror("[BGCE] DRM_IOCTL_MODE_DESTROY_DUMB");
		return -1;
	}
	return 0;
}

int init_display(void)
{
	int drm_fd = -1;
	for (int card = 0; card < 10; card++) {
		char path[64];
		snprintf(path, sizeof(path), "/dev/dri/card%d", card);
		drm_fd = open(path, O_RDWR | O_CLOEXEC);
		if (drm_fd >= 0)
			break;
	}
	if (drm_fd < 0) {
		perror("[BGCE] open drm device (tried /dev/dri/card0..9)");
		return 1;
	}
	server.display_fd = drm_fd;
	setup_vt_handling();

	resources = drmModeGetResources(drm_fd);
	if (!resources) {
		fprintf(stderr, "[BGCE] drmModeGetResources failed\n");
		close(drm_fd);
		server.display_fd = -1;
		return 1;
	}

	drmModeModeInfo chosen_mode;
	bool found = false;
	uint32_t crtc_id = 0;
	for (int i = 0; i < resources->count_connectors; i++) {
		connector = drmModeGetConnector(drm_fd, resources->connectors[i]);
		if (!connector)
			continue;
		if (connector->connection == DRM_MODE_CONNECTED &&
		    connector->count_modes > 0) {
			chosen_mode = connector->modes[0];
			conn_id = connector->connector_id;
			found = true;
			break;
		}
		drmModeFreeConnector(connector);
		connector = NULL;
	}
	if (!found) {
		fprintf(stderr, "[BGCE] No connected connector with modes found\n");
		drmModeFreeResources(resources);
		resources = NULL;
		close(drm_fd);
		server.display_fd = -1;
		return 1;
	}

	if (connector->encoder_id)
		encoder = drmModeGetEncoder(drm_fd, connector->encoder_id);
	if (encoder && encoder->crtc_id) {
		crtc_id = encoder->crtc_id;
	} else {
		for (int i = 0; i < resources->count_encoders; i++) {
			drmModeEncoder *enc = drmModeGetEncoder(drm_fd, resources->encoders[i]);
			if (!enc)
				continue;
			for (int c = 0; c < resources->count_crtcs; c++) {
				if (enc->possible_crtcs & (1u << c)) {
					crtc_id = resources->crtcs[c];
					break;
				}
			}
			drmModeFreeEncoder(enc);
			if (crtc_id)
				break;
		}
	}
	if (!crtc_id) {
		fprintf(stderr, "[BGCE] Failed to find a suitable CRTC\n");
		release_display();
		return 1;
	}
	server.crtc_id = crtc_id;
	saved_crtc = drmModeGetCrtc(drm_fd, crtc_id);

	uint32_t width = chosen_mode.hdisplay;
	uint32_t height = chosen_mode.vdisplay;
	uint32_t bpp = 32;
	server.display_w = width;
	server.display_h = height;
	server.display_bpp = bpp;

	struct drm_mode_create_dumb create = {0};
	if (drm_create_dumb(drm_fd, width, height, bpp, &create) < 0)
		return -1;
	scanout_handle = create.handle;
	scanout_size = create.size;
	uint32_t scanout_pitch = create.pitch;
	server.display_pitch = scanout_pitch;
	server.fb_size = (size_t)scanout_size;

	uint64_t scanout_offset;
	if (drm_map_dumb(drm_fd, scanout_handle, &scanout_offset) < 0)
		return -1;
	server.framebuffer = mmap(NULL, scanout_size, PROT_READ | PROT_WRITE,
	                          MAP_SHARED, drm_fd, scanout_offset);
	if (server.framebuffer == MAP_FAILED) {
		perror("[BGCE] mmap scanout");
		return -1;
	}
	memset(server.framebuffer, 0, scanout_size);

	bool fb2_ok = false;
#ifdef DRM_FORMAT_XRGB8888
	{
		uint32_t handles[4] = {scanout_handle, 0, 0, 0};
		uint32_t pitches[4] = {scanout_pitch, 0, 0, 0};
		uint32_t offsets[4] = {0, 0, 0, 0};
		if (drmModeAddFB2(drm_fd, width, height, DRM_FORMAT_XRGB8888,
		                  handles, pitches, offsets, &fb_id, 0) == 0)
			fb2_ok = true;
	}
#endif
	if (!fb2_ok) {
		if (drmModeAddFB(drm_fd, width, height, 24, bpp, scanout_pitch,
		                 scanout_handle, &fb_id) != 0) {
			fprintf(stderr, "[BGCE] drmModeAddFB failed\n");
			return -1;
		}
	}

	if (drmModeSetCrtc(drm_fd, crtc_id, fb_id, 0, 0, &conn_id, 1,
	                   &chosen_mode) != 0) {
		fprintf(stderr, "[BGCE] drmModeSetCrtc failed: %s\n", strerror(errno));
		return -1;
	}

	printf("[BGCE] DRM connector %u CRTC %u mode %ux%u@%u pitch=%u\n",
	       conn_id, crtc_id, width, height, chosen_mode.vrefresh, scanout_pitch);

	if (display_cursor_init() != 0) {
		fprintf(stderr, "[BGCE] software cursor init failed\n");
		release_display();
		return 1;
	}
	return 0;
}

void release_display(void)
{
	release_vt();
	display_cursor_fini();

	int drm_fd = server.display_fd;
	if (fb_id && drm_fd >= 0)
		drmModeRmFB(drm_fd, fb_id);
	fb_id = 0;

	if (server.framebuffer && server.framebuffer != MAP_FAILED) {
		munmap(server.framebuffer, scanout_size ? scanout_size : server.fb_size);
		server.framebuffer = NULL;
	}
	if (scanout_handle && drm_fd >= 0)
		drm_destroy_dumb(drm_fd, scanout_handle);
	scanout_handle = 0;

	if (saved_crtc && drm_fd >= 0) {
		drmModeSetCrtc(drm_fd, saved_crtc->crtc_id, saved_crtc->buffer_id,
		               saved_crtc->x, saved_crtc->y, &conn_id, 1,
		               &saved_crtc->mode);
		drmModeFreeCrtc(saved_crtc);
		saved_crtc = NULL;
	}
	if (connector) {
		drmModeFreeConnector(connector);
		connector = NULL;
	}
	if (resources) {
		drmModeFreeResources(resources);
		resources = NULL;
	}
	if (encoder) {
		drmModeFreeEncoder(encoder);
		encoder = NULL;
	}
	if (drm_fd >= 0) {
		close(drm_fd);
		server.display_fd = -1;
	}
	printf("[BGCE] Display released.\n");
}
