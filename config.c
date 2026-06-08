#include "server.h"
#include <ctype.h>
#include <linux/input.h>
#include <stdbool.h>
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

// Map key name string to Linux key code (KEY_*)
static uint16_t parse_key_name(const char* name) {
	if (!name || !*name)
		return 0;

	// Normalize: lowercase, strip optional KEY_ prefix
	char buf[32];
	strncpy(buf, name, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';
	for (char* p = buf; *p; p++)
		*p = tolower((unsigned char)*p);

	// Strip KEY_ prefix if present
	const char* k = buf;
	if (strncmp(buf, "key_", 4) == 0)
		k = buf + 4;

	// Common aliases
	if (strcmp(k, "sysrq") == 0 || strcmp(k, "print") == 0 || strcmp(k, "printscreen") == 0 || strcmp(k, "prtsc") == 0)
		return KEY_SYSRQ;
	if (strcmp(k, "esc") == 0 || strcmp(k, "escape") == 0)
		return KEY_ESC;
	if (strcmp(k, "ret") == 0 || strcmp(k, "return") == 0 || strcmp(k, "enter") == 0)
		return KEY_ENTER;
	if (strcmp(k, "space") == 0)
		return KEY_SPACE;
	if (strcmp(k, "tab") == 0)
		return KEY_TAB;
	if (strcmp(k, "backspace") == 0 || strcmp(k, "bksp") == 0)
		return KEY_BACKSPACE;
	if (strcmp(k, "del") == 0 || strcmp(k, "delete") == 0)
		return KEY_DELETE;
	if (strcmp(k, "ins") == 0 || strcmp(k, "insert") == 0)
		return KEY_INSERT;
	if (strcmp(k, "home") == 0)
		return KEY_HOME;
	if (strcmp(k, "end") == 0)
		return KEY_END;
	if (strcmp(k, "pgup") == 0 || strcmp(k, "pageup") == 0)
		return KEY_PAGEUP;
	if (strcmp(k, "pgdn") == 0 || strcmp(k, "pagedown") == 0)
		return KEY_PAGEDOWN;
	if (strcmp(k, "up") == 0)
		return KEY_UP;
	if (strcmp(k, "down") == 0)
		return KEY_DOWN;
	if (strcmp(k, "left") == 0)
		return KEY_LEFT;
	if (strcmp(k, "right") == 0)
		return KEY_RIGHT;

	// Function keys
	if (k[0] == 'f' && k[1] >= '1' && k[1] <= '9') {
		int n = atoi(k + 1);
		if (n >= 1 && n <= 12) {
			static const uint16_t fkeys[] = {
				0, KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
				KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12
			};
			return fkeys[n];
		}
	}

	// Single letter keys a-z
	if (k[0] >= 'a' && k[0] <= 'z' && k[1] == '\0') {
		// KEY_A is 30, KEY_B is 48, etc. They are not contiguous in a nice way.
		// Use a small table for letters and digits.
		static const struct { char c; uint16_t code; } letters[] = {
			{'a', KEY_A}, {'b', KEY_B}, {'c', KEY_C}, {'d', KEY_D}, {'e', KEY_E},
			{'f', KEY_F}, {'g', KEY_G}, {'h', KEY_H}, {'i', KEY_I}, {'j', KEY_J},
			{'k', KEY_K}, {'l', KEY_L}, {'m', KEY_M}, {'n', KEY_N}, {'o', KEY_O},
			{'p', KEY_P}, {'q', KEY_Q}, {'r', KEY_R}, {'s', KEY_S}, {'t', KEY_T},
			{'u', KEY_U}, {'v', KEY_V}, {'w', KEY_W}, {'x', KEY_X}, {'y', KEY_Y},
			{'z', KEY_Z},
		};
		for (size_t i = 0; i < sizeof(letters) / sizeof(letters[0]); i++) {
			if (letters[i].c == k[0])
				return letters[i].code;
		}
	}

	// Digits 0-9
	if (k[0] >= '0' && k[0] <= '9' && k[1] == '\0') {
		static const uint16_t digits[] = {
			KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9
		};
		return digits[k[0] - '0'];
	}

	// Fallback: try KEY_XXX directly via a small set of known names
	if (strcmp(k, "leftctrl") == 0 || strcmp(k, "ctrl") == 0)
		return KEY_LEFTCTRL;
	if (strcmp(k, "rightctrl") == 0)
		return KEY_RIGHTCTRL;
	if (strcmp(k, "leftalt") == 0 || strcmp(k, "alt") == 0)
		return KEY_LEFTALT;
	if (strcmp(k, "rightalt") == 0)
		return KEY_RIGHTALT;
	if (strcmp(k, "leftshift") == 0 || strcmp(k, "shift") == 0)
		return KEY_LEFTSHIFT;
	if (strcmp(k, "rightshift") == 0)
		return KEY_RIGHTSHIFT;

	return 0;
}

// Parse a combo string like "ctrl+alt+q" or "sysrq" into a key_combo
static struct key_combo parse_key_combo(const char* str) {
	struct key_combo kc = {0, 0, 0, 0};
	if (!str || !*str)
		return kc;

	char buf[128];
	strncpy(buf, str, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';

	// Split on '+'
	char* token = strtok(buf, "+");
	while (token) {
		// Trim token
		while (isspace((unsigned char)*token))
			token++;
		char* end = token + strlen(token) - 1;
		while (end > token && isspace((unsigned char)*end)) {
			*end = '\0';
			end--;
		}

		uint16_t code = parse_key_name(token);
		if (code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL)
			kc.ctrl = 1;
		else if (code == KEY_LEFTALT || code == KEY_RIGHTALT)
			kc.alt = 1;
		else if (code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT)
			kc.shift = 1;
		else if (code != 0)
			kc.key = code;

		token = strtok(NULL, "+");
	}

	return kc;
}

// Parse a shortcut rhs of the form "builtin:exit" or "command:alacritty -e foo"
// Stores into out and returns SHORTCUT_PARSE_OK on success.
static enum shortcut_parse_result parse_shortcut_rhs(const char* rhs, struct shortcut* out) {
	if (!rhs || !*rhs || !out)
		return SHORTCUT_PARSE_BAD_FORMAT;

	char buf[512];
	strncpy(buf, rhs, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';

	char* colon = strchr(buf, ':');
	if (!colon)
		return SHORTCUT_PARSE_BAD_FORMAT;

	*colon = '\0';
	char* typestr = trim(buf);
	char* valstr = trim(colon + 1);

	if (strcmp(typestr, "builtin") == 0) {
		out->type = SHORTCUT_BUILTIN;
		if (strcmp(valstr, "exit") == 0) {
			strcpy(out->value, "exit");
			return SHORTCUT_PARSE_OK;
		} else if (strcmp(valstr, "screenshot") == 0) {
			strcpy(out->value, "screenshot");
			return SHORTCUT_PARSE_OK;
		}
		return SHORTCUT_PARSE_UNSUPPORTED_BUILTIN;
	} else if (strcmp(typestr, "command") == 0) {
		if (*valstr == '\0')
			return SHORTCUT_PARSE_EMPTY_COMMAND;
		out->type = SHORTCUT_COMMAND;
		strncpy(out->value, valstr, sizeof(out->value) - 1);
		out->value[sizeof(out->value) - 1] = '\0';
		return SHORTCUT_PARSE_OK;
	}

	return SHORTCUT_PARSE_BAD_FORMAT;
}

// Ensure the default exit and screenshot shortcuts exist unless the user
// has explicitly provided their own binding for those actions.
static void set_default_shortcuts(struct config* config) {
	bool has_exit = false;
	bool has_screenshot = false;

	for (int i = 0; i < config->shortcut_count; i++) {
		if (config->shortcuts[i].type == SHORTCUT_BUILTIN) {
			if (strcmp(config->shortcuts[i].value, "exit") == 0)
				has_exit = true;
			else if (strcmp(config->shortcuts[i].value, "screenshot") == 0)
				has_screenshot = true;
		}
	}

	if (!has_exit && config->shortcut_count < MAX_SHORTCUTS) {
		int idx = config->shortcut_count;
		memset(&config->shortcuts[idx], 0, sizeof(config->shortcuts[0]));
		config->shortcuts[idx].combo.ctrl = 1;
		config->shortcuts[idx].combo.alt = 1;
		config->shortcuts[idx].combo.key = KEY_Q;
		config->shortcuts[idx].type = SHORTCUT_BUILTIN;
		strcpy(config->shortcuts[idx].value, "exit");
		config->shortcut_count++;
	}

	if (!has_screenshot && config->shortcut_count < MAX_SHORTCUTS) {
		int idx = config->shortcut_count;
		memset(&config->shortcuts[idx], 0, sizeof(config->shortcuts[0]));
		config->shortcuts[idx].combo.key = KEY_SYSRQ;
		config->shortcuts[idx].type = SHORTCUT_BUILTIN;
		strcpy(config->shortcuts[idx].value, "screenshot");
		config->shortcut_count++;
	}
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
	config->shortcut_count = 0;

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
		} else if (strcmp(current_section, "shortcuts") == 0) {
			if (config->shortcut_count < MAX_SHORTCUTS) {
				struct key_combo kc = parse_key_combo(key);
				if (kc.key != 0) {
					struct shortcut sc = {0};
					enum shortcut_parse_result rc = parse_shortcut_rhs(value, &sc);
					if (rc == SHORTCUT_PARSE_OK) {
						sc.combo = kc;
						config->shortcuts[config->shortcut_count] = sc;
						config->shortcut_count++;
					} else if (rc == SHORTCUT_PARSE_UNSUPPORTED_BUILTIN) {
						fprintf(stderr, "[BGCE] Unsupported builtin command in shortcut '%s = %s' (must be 'exit' or 'screenshot')\n",
						        key, value);
					} else if (rc == SHORTCUT_PARSE_EMPTY_COMMAND) {
						fprintf(stderr, "[BGCE] Empty command value in shortcut '%s = %s'\n", key, value);
					} else {
						fprintf(stderr, "[BGCE] Invalid shortcut '%s = %s'\n", key, value);
					}
				} else {
					fprintf(stderr, "[BGCE] Ignoring shortcut with unknown key combo: %s\n", key);
				}
			}
		}
	}

	// Add default exit/screenshot shortcuts unless the user explicitly
	// specified their own binding for those actions.
	set_default_shortcuts(config);

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
