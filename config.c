#include "server.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// Helper: trim whitespace
static char* trim(char* str) {
	while (isspace((unsigned char)*str))
		str++;
	if (*str == 0)
		return str;

	char* end = str + strlen(str) - 1;
	while (end > str && isspace((unsigned char)*end))
		end--;
	end[1] = '\0';

	return str;
}

// Helper: parse hex color (#RRGGBB or #RRGGBBAA)
static uint32_t parse_hex_color(const char* str) {
	if (str[0] != '#')
		return 0xFF000000; // Default to black with full opacity

	unsigned int r, g, b, a = 255;
	if (strlen(str) == 7) { // #RRGGBB
		sscanf(str + 1, "%02x%02x%02x", &r, &g, &b);
	} else if (strlen(str) == 9) { // #RRGGBBAA
		sscanf(str + 1, "%02x%02x%02x%02x", &r, &g, &b, &a);
	} else {
		return 0xFF000000; // Default to black with full opacity
	}

	return (a << 24) | (r << 16) | (g << 8) | b;
}

// Parse config file
int parse_config(struct config* config) {
	const char* home = getenv("HOME");
	char user_config[512];
	if (!home) {
		return -1;
	}
	snprintf(user_config, sizeof(user_config), "%s/.config/bgce.conf", home);
	FILE* file = fopen(user_config, "r");
	if (!file) {
		perror("[BGCE] Open config file");
		return -1;
	}

	// Initialize with defaults
	config->type = BG_COLOR;
	config->color = 0xAAAAAAAA; // Default gray

	char line[1024];
	char current_section[256] = "";

	while (fgets(line, sizeof(line), file)) {
		char* trimmed = trim(line);

		// Skip empty lines and comments
		if (trimmed[0] == '\0' || trimmed[0] == '#' || trimmed[0] == ';') {
			continue;
		}

		// Check for section
		if (trimmed[0] == '[' && trimmed[strlen(trimmed) - 1] == ']') {
			strncpy(current_section, trimmed + 1, strlen(trimmed) - 2);
			current_section[strlen(trimmed) - 2] = '\0';
			continue;
		}

		// Parse key-value pairs
		char* equals = strchr(trimmed, '=');
		if (!equals)
			continue;

		char key[32];
		char value[128];
		sscanf(trimmed, "%s = %[^\n]", key, value);

		if (strcmp(current_section, "background") == 0) {
			if (strcmp(key, "type") == 0) {
				if (strcmp(value, "color") == 0) {
					config->type = BG_COLOR;
				} else if (strcmp(value, "image") == 0) {
					config->type = BG_IMAGE;
				}
			} else if (strcmp(key, "color") == 0 && config->type == BG_COLOR) {
				config->color = parse_hex_color(value);
			} else if (strcmp(key, "path") == 0 && config->type == BG_IMAGE) {
				strncpy(config->path, value, MAX_PATH_LEN - 1);
				config->path[MAX_PATH_LEN - 1] = '\0';
			} else if (strcmp(key, "mode") == 0 && config->type == BG_IMAGE) {
				if (strcmp(value, "tiled") == 0) {
					config->mode = IMAGE_TILED;
					printf("[BGCE] image tiled\n");
				} else if (strcmp(value, "scaled") == 0) {
					config->mode = IMAGE_SCALED;
				}
			}
		}
	}

	fclose(file);
	return 0;
}

// Apply background to a buffer
int apply_background(struct config* config, uint32_t* buffer, uint32_t width, uint32_t height) {
	if (config->type == BG_COLOR) {
		// Fill with solid color
		for (uint32_t i = 0; i < width * height; i++) {
			buffer[i] = config->color;
		}
		return 0;
	} else if (config->type == BG_IMAGE) {
		// Load and apply image
		int img_width, img_height, img_channels;
		unsigned char* img_data = stbi_load(config->path, &img_width, &img_height, &img_channels, 4);
		if (!img_data) {
			fprintf(stderr, "Failed to load image: %s\n", config->path);
			// Fallback to a default color (dark gray with full opacity)
			fprintf(stderr, "[BGCE] Falling back to default color #333333\n");
			for (uint32_t i = 0; i < width * height; i++) {
				buffer[i] = 0xFF333333;
			}
			return 0;
		}

		if (config->mode == IMAGE_TILED) {
			// Tile the image
			for (uint32_t y = 0; y < height; y++) {
				for (uint32_t x = 0; x < width; x++) {
					uint32_t img_x = x % img_width;
					uint32_t img_y = y % img_height;
					uint32_t img_idx = (img_y * img_width + img_x) * 4;
					uint32_t buf_idx = y * width + x;

					// Convert RGBA to uint32_t
					buffer[buf_idx] =
					        (img_data[img_idx + 3] << 24) |
					        (img_data[img_idx] << 16) |
					        (img_data[img_idx + 1] << 8) |
					        img_data[img_idx + 2];
				}
			}
		} else {
			// Scale the image (simple nearest-neighbor)
			// TODO: improve this
			float x_ratio = (float)img_width / width;
			float y_ratio = (float)img_height / height;

			for (uint32_t y = 0; y < height; y++) {
				for (uint32_t x = 0; x < width; x++) {
					uint32_t img_x = (uint32_t)(x * x_ratio);
					uint32_t img_y = (uint32_t)(y * y_ratio);
					uint32_t img_idx = (img_y * img_width + img_x) * 4;
					uint32_t buf_idx = y * width + x;

					// Convert RGBA to uint32_t
					buffer[buf_idx] =
					        (img_data[img_idx + 3] << 24) |
					        (img_data[img_idx] << 16) |
					        (img_data[img_idx + 1] << 8) |
					        img_data[img_idx + 2];
				}
			}
		}

		stbi_image_free(img_data);
		return 0;
	}

	return -1;
}
