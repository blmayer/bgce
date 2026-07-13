#include "server.h"
#include <ctype.h>
#include <limits.h>
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

// Parse rhs: "builtin:exit" or "command:/bin/foo arg1 arg2" (execvp, no shell)
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

/* ---------------------------------------------------------------
 * Xcursor file loader
 *
 * The Xcursor format (used by X11/Wayland themes) stores ARGB pixel
 * data with hotspot information.  Files live under a theme directory,
 * e.g. /usr/share/icons/Adwaita/cursors/left_ptr
 *
 * File layout (all little-endian):
 *   Header:  magic(4) header_size(4) version(4) ntoc(4)
 *   TOC[ntoc]: type(4) subtype(4) position(4)
 *   Image chunk (type 0xfffd0002):
 *     chunk_header(4) type(4) subtype(4) version(4)
 *     width(4) height(4) xhot(4) yhot(4) delay(4)
 *     pixels[width*height] (ARGB, 4 bytes each)
 * --------------------------------------------------------------- */

#define XCURSOR_MAGIC     0x72756358u  /* "Xcur" */
#define XCURSOR_IMAGE_TYPE 0xfffd0002u

static uint32_t read_le32(const uint8_t* p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/*
 * Load the best-fit image from an Xcursor file.
 * Prefers an image whose nominal size (subtype) is closest to CURSOR_WIDTH.
 * The image is nearest-neighbor scaled to exactly CURSOR_WIDTH x CURSOR_HEIGHT.
 * Fills hotspot_x/hotspot_y (scaled) and returns a malloc'd ARGB buffer,
 * or NULL on failure.
 */
static uint32_t* load_xcursor_file(const char* path, int* hotspot_x, int* hotspot_y) {
	FILE* f = fopen(path, "rb");
	if (!f) return NULL;

	/* Read entire file */
	fseek(f, 0, SEEK_END);
	long file_size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (file_size < 16) { fclose(f); return NULL; }

	uint8_t* data = malloc((size_t)file_size);
	if (!data) { fclose(f); return NULL; }
	if (fread(data, 1, (size_t)file_size, f) != (size_t)file_size) {
		free(data); fclose(f); return NULL;
	}
	fclose(f);

	/* Validate header */
	uint32_t magic = read_le32(data);
	if (magic != XCURSOR_MAGIC) {
		fprintf(stderr, "[BGCE] Not an Xcursor file: %s\n", path);
		free(data); return NULL;
	}
	uint32_t ntoc = read_le32(data + 12);
	if (16 + ntoc * 12 > (uint32_t)file_size) {
		free(data); return NULL;
	}

	/* Scan TOC for image entries, pick best size match */
	uint32_t best_pos = 0;
	int best_diff = INT_MAX;
	for (uint32_t i = 0; i < ntoc; i++) {
		uint8_t* toc = data + 16 + i * 12;
		uint32_t type    = read_le32(toc);
		uint32_t subtype = read_le32(toc + 4);  /* nominal size */
		uint32_t pos     = read_le32(toc + 8);
		if (type != XCURSOR_IMAGE_TYPE) continue;

		int diff = abs((int)subtype - (int)CURSOR_WIDTH);
		if (diff < best_diff) {
			best_diff = diff;
			best_pos = pos;
		}
	}
	if (best_diff == INT_MAX) {
		fprintf(stderr, "[BGCE] No image chunks in Xcursor file: %s\n", path);
		free(data); return NULL;
	}

	/* Parse image chunk at best_pos */
	if (best_pos + 36 > (uint32_t)file_size) { free(data); return NULL; }
	uint8_t* chunk = data + best_pos;
	/* chunk layout: header_size(4) type(4) subtype(4) version(4)
	 *               width(4) height(4) xhot(4) yhot(4) delay(4) pixels... */
	uint32_t chunk_header_size = read_le32(chunk);
	if (chunk_header_size < 36) chunk_header_size = 36;
	uint32_t img_w = read_le32(chunk + 16);
	uint32_t img_h = read_le32(chunk + 20);
	uint32_t xhot  = read_le32(chunk + 24);
	uint32_t yhot  = read_le32(chunk + 28);
	/* pixels start after chunk header */
	uint32_t pixel_offset = best_pos + chunk_header_size;
	if (pixel_offset + img_w * img_h * 4 > (uint32_t)file_size) {
		fprintf(stderr, "[BGCE] Truncated Xcursor image in: %s\n", path);
		free(data); return NULL;
	}

	uint32_t* pixels = (uint32_t*)(data + pixel_offset);

	/* Allocate output buffer at CURSOR_WIDTH x CURSOR_HEIGHT */
	uint32_t* buf = malloc(CURSOR_WIDTH * CURSOR_HEIGHT * sizeof(uint32_t));
	if (!buf) { free(data); return NULL; }

	/* Nearest-neighbor scale; Xcursor pixels are already ARGB */
	float x_ratio = (float)img_w / CURSOR_WIDTH;
	float y_ratio = (float)img_h / CURSOR_HEIGHT;

	for (int y = 0; y < CURSOR_HEIGHT; y++) {
		for (int x = 0; x < CURSOR_WIDTH; x++) {
			int sx = (int)(x * x_ratio);
			int sy = (int)(y * y_ratio);
			if (sx >= (int)img_w) sx = (int)img_w - 1;
			if (sy >= (int)img_h) sy = (int)img_h - 1;
			buf[y * CURSOR_WIDTH + x] = pixels[sy * (int)img_w + sx];
		}
	}

	/* Scale hotspot */
	if (hotspot_x) *hotspot_x = (img_w > 0) ? (int)(xhot * CURSOR_WIDTH / img_w) : 0;
	if (hotspot_y) *hotspot_y = (img_h > 0) ? (int)(yhot * CURSOR_HEIGHT / img_h) : 0;

	free(data);
	printf("[BGCE] Loaded Xcursor: %s (%ux%u -> %dx%d, hotspot %u,%u)\n",
	       path, img_w, img_h, CURSOR_WIDTH, CURSOR_HEIGHT, xhot, yhot);
	return buf;
}

/*
 * Standard Xcursor file names for each BGCECursorType.
 * Themes use these names under their cursors/ directory.
 */
static const char* xcursor_names[][3] = {
	[BGCE_CURSOR_DEFAULT]     = { "left_ptr",             "default",    "arrow" },
	[BGCE_CURSOR_TEXT]        = { "xterm",                 "text",       "ibeam" },
	[BGCE_CURSOR_HAND]        = { "hand2",                 "pointer",    "hand" },
	[BGCE_CURSOR_RESIZE_NS]   = { "sb_v_double_arrow",     "ns-resize",  "size_ver" },
	[BGCE_CURSOR_RESIZE_EW]   = { "sb_h_double_arrow",     "ew-resize",  "size_hor" },
	[BGCE_CURSOR_RESIZE_NWSE] = { "top_left_corner",       "nwse-resize","size_fdiag" },
	[BGCE_CURSOR_MOVE]        = { "fleur",                 "move",       "all-scroll" },
};

/*
 * Load all cursor types from an Xcursor theme directory.
 * Tries each standard name for a given type until one succeeds.
 */
static void load_cursor_theme(const char* theme_dir, struct cursor_theme* theme) {
	for (int t = 0; t < BGCE_CURSOR_COUNT; t++) {
		for (int n = 0; n < 3; n++) {
			if (!xcursor_names[t][n]) continue;

			char path[MAX_PATH_LEN];
			snprintf(path, sizeof(path), "%s/%s", theme_dir, xcursor_names[t][n]);

			int hx = 0, hy = 0;
			uint32_t* img = load_xcursor_file(path, &hx, &hy);
			if (img) {
				theme->images[t] = img;
				theme->hotspot_x[t] = hx;
				theme->hotspot_y[t] = hy;
				break; /* found one, skip alternatives */
			}
		}
		if (!theme->images[t])
			printf("[BGCE] No Xcursor file found for cursor type %d, using built-in\n", t);
	}
}

// Load config file
int load_config(struct config* config) {
	const char* home = getenv("HOME");
	char user_config[512];
	FILE* file = NULL;

	// Initialize with defaults even if the file is missing
	config->type = BG_COLOR;
	config->color = 0xAAAAAAAA; // Default gray
	config->mode = IMAGE_SCALED;
	config->path[0] = '\0';
	config->shortcut_count = 0;
	memset(&config->cursors, 0, sizeof(config->cursors));
	config->move_speed = 1.0f;
	config->pan_speed = 1.0f;
	config->natural_scrolling = 0;

	if (!home) {
		set_default_shortcuts(config);
		return -1;
	}
	snprintf(user_config, sizeof(user_config), "%s/.config/bgce.conf", home);
	file = fopen(user_config, "r");
	if (!file) {
		perror("[BGCE] Open config file");
		set_default_shortcuts(config);
		return -1;
	}

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
		char value[MAX_PATH_LEN];
		char *valp;
		/* Accept "key = value", "key=value", and trim the value. */
		if (sscanf(trimmed, "%31s = %511[^\n]", key, value) != 2 &&
		    sscanf(trimmed, "%31[^=]=%511[^\n]", key, value) != 2)
			continue;
		{
			char *kp = trim(key);
			if (kp != key)
				memmove(key, kp, strlen(kp) + 1);
		}
		valp = trim(value);

		if (strcmp(current_section, "background") == 0) {
			/*
			 * Accept keys in any order: path/mode/color are stored even
			 * before type= is seen (type used to gate path and silently
			 * drop wallpapers when path came first).
			 */
			if (strcmp(key, "type") == 0) {
				if (strcmp(valp, "color") == 0) {
					config->type = BG_COLOR;
				} else if (strcmp(valp, "image") == 0) {
					config->type = BG_IMAGE;
				}
			} else if (strcmp(key, "color") == 0) {
				config->color = parse_hex_color(valp);
			} else if (strcmp(key, "path") == 0) {
				strncpy(config->path, valp, MAX_PATH_LEN - 1);
				config->path[MAX_PATH_LEN - 1] = '\0';
			} else if (strcmp(key, "mode") == 0) {
				if (strcmp(valp, "tiled") == 0) {
					config->mode = IMAGE_TILED;
					printf("[BGCE] image tiled\n");
				} else if (strcmp(valp, "scaled") == 0) {
					config->mode = IMAGE_SCALED;
				}
			}
		} else if (strcmp(current_section, "input") == 0) {
			if (strcmp(key, "move_speed") == 0) {
				config->move_speed = strtof(valp, NULL);
				if (config->move_speed <= 0.0f)
					config->move_speed = 1.0f;
			} else if (strcmp(key, "pan_speed") == 0) {
				config->pan_speed = strtof(valp, NULL);
				if (config->pan_speed <= 0.0f)
					config->pan_speed = 1.0f;
			} else if (strcmp(key, "natural_scrolling") == 0) {
				if (strcmp(valp, "1") == 0 ||
				    strcmp(valp, "true") == 0 ||
				    strcmp(valp, "yes") == 0 ||
				    strcmp(valp, "on") == 0)
					config->natural_scrolling = 1;
				else if (strcmp(valp, "0") == 0 ||
				         strcmp(valp, "false") == 0 ||
				         strcmp(valp, "no") == 0 ||
				         strcmp(valp, "off") == 0)
					config->natural_scrolling = 0;
				else {
					fprintf(stderr,
					        "[BGCE] natural_scrolling: use "
					        "true/false (got '%s')\n",
					        valp);
				}
			} else {
				fprintf(stderr, "[BGCE] Unknown input config key: %s "
				        "(expected move_speed, pan_speed, or "
				        "natural_scrolling)\n",
				        key);
			}
		} else if (strcmp(current_section, "cursors") == 0) {
			if (strcmp(key, "theme") == 0) {
				load_cursor_theme(valp, &config->cursors);
			} else {
				fprintf(stderr, "[BGCE] Unknown cursor config key: %s "
				        "(expected 'theme')\n", key);
			}
		} else if (strcmp(current_section, "shortcuts") == 0) {
			if (config->shortcut_count < MAX_SHORTCUTS) {
				struct key_combo kc = parse_key_combo(key);
				if (kc.key != 0) {
					struct shortcut sc = {0};
					enum shortcut_parse_result rc = parse_shortcut_rhs(valp, &sc);
					if (rc == SHORTCUT_PARSE_OK) {
						sc.combo = kc;
						config->shortcuts[config->shortcut_count] = sc;
						config->shortcut_count++;
					} else if (rc == SHORTCUT_PARSE_UNSUPPORTED_BUILTIN) {
						fprintf(stderr, "[BGCE] Unsupported builtin command in shortcut '%s = %s' (must be 'exit' or 'screenshot')\n",
						        key, valp);
					} else if (rc == SHORTCUT_PARSE_EMPTY_COMMAND) {
						fprintf(stderr, "[BGCE] Empty command value in shortcut '%s = %s'\n", key, valp);
					} else {
						fprintf(stderr, "[BGCE] Invalid shortcut '%s = %s'\n", key, valp);
					}
				} else {
					fprintf(stderr, "[BGCE] Ignoring shortcut with unknown key combo: %s\n", key);
				}
			}
		}
	}

	/* Image background with no path → fall back to solid color. */
	if (config->type == BG_IMAGE && !config->path[0]) {
		fprintf(stderr, "[BGCE] background type=image but path is empty; using color\n");
		config->type = BG_COLOR;
	}

	// Add default exit/screenshot shortcuts unless the user explicitly
	// specified their own binding for those actions.
	set_default_shortcuts(config);

	fclose(file);
	return 0;
}

static const char* key_code_name(uint16_t code)
{
	static char fallback[16];

	if (code >= KEY_1 && code <= KEY_9) {
		fallback[0] = (char)('1' + (code - KEY_1));
		fallback[1] = '\0';
		return fallback;
	}
	if (code == KEY_0)
		return "0";

	switch (code) {
	case KEY_A: return "a"; case KEY_B: return "b"; case KEY_C: return "c";
	case KEY_D: return "d"; case KEY_E: return "e"; case KEY_F: return "f";
	case KEY_G: return "g"; case KEY_H: return "h"; case KEY_I: return "i";
	case KEY_J: return "j"; case KEY_K: return "k"; case KEY_L: return "l";
	case KEY_M: return "m"; case KEY_N: return "n"; case KEY_O: return "o";
	case KEY_P: return "p"; case KEY_Q: return "q"; case KEY_R: return "r";
	case KEY_S: return "s"; case KEY_T: return "t"; case KEY_U: return "u";
	case KEY_V: return "v"; case KEY_W: return "w"; case KEY_X: return "x";
	case KEY_Y: return "y"; case KEY_Z: return "z";
	case KEY_F1: return "f1"; case KEY_F2: return "f2"; case KEY_F3: return "f3";
	case KEY_F4: return "f4"; case KEY_F5: return "f5"; case KEY_F6: return "f6";
	case KEY_F7: return "f7"; case KEY_F8: return "f8"; case KEY_F9: return "f9";
	case KEY_F10: return "f10"; case KEY_F11: return "f11"; case KEY_F12: return "f12";
	case KEY_ESC: return "esc";
	case KEY_ENTER: return "enter";
	case KEY_SPACE: return "space";
	case KEY_TAB: return "tab";
	case KEY_BACKSPACE: return "backspace";
	case KEY_DELETE: return "delete";
	case KEY_INSERT: return "insert";
	case KEY_HOME: return "home";
	case KEY_END: return "end";
	case KEY_PAGEUP: return "pgup";
	case KEY_PAGEDOWN: return "pgdn";
	case KEY_UP: return "up";
	case KEY_DOWN: return "down";
	case KEY_LEFT: return "left";
	case KEY_RIGHT: return "right";
	case KEY_SYSRQ: return "sysrq";
	default:
		snprintf(fallback, sizeof(fallback), "key_%u", (unsigned)code);
		return fallback;
	}
}

void print_config(const struct config* config)
{
	int i;
	int cursor_loaded = 0;

	if (!config)
		return;

	printf("[BGCE] === loaded config ===\n");

	if (config->type == BG_COLOR) {
		printf("[BGCE] background: type=color color=#%08X\n", config->color);
	} else {
		printf("[BGCE] background: type=image path=%s mode=%s\n",
		       config->path[0] ? config->path : "(none)",
		       config->mode == IMAGE_TILED ? "tiled" : "scaled");
	}

	printf("[BGCE] shortcuts: %d\n", config->shortcut_count);
	for (i = 0; i < config->shortcut_count; i++) {
		const struct shortcut* sc = &config->shortcuts[i];
		char combo[64];
		int n = 0;

		combo[0] = '\0';
		if (sc->combo.ctrl)
			n += snprintf(combo + n, sizeof(combo) - (size_t)n, "%sctrl",
			              n ? "+" : "");
		if (sc->combo.alt)
			n += snprintf(combo + n, sizeof(combo) - (size_t)n, "%salt",
			              n ? "+" : "");
		if (sc->combo.shift)
			n += snprintf(combo + n, sizeof(combo) - (size_t)n, "%sshift",
			              n ? "+" : "");
		if (sc->combo.key) {
			n += snprintf(combo + n, sizeof(combo) - (size_t)n, "%s%s",
			              n ? "+" : "", key_code_name(sc->combo.key));
		}
		if (!combo[0])
			snprintf(combo, sizeof(combo), "(none)");

		if (sc->type == SHORTCUT_BUILTIN)
			printf("[BGCE]   %s = builtin:%s\n", combo, sc->value);
		else if (sc->type == SHORTCUT_COMMAND)
			printf("[BGCE]   %s = command:%s\n", combo, sc->value);
		else
			printf("[BGCE]   %s = (unknown type %d)\n", combo, (int)sc->type);
	}

	for (i = 0; i < BGCE_CURSOR_COUNT; i++) {
		if (config->cursors.images[i])
			cursor_loaded++;
	}
	printf("[BGCE] cursors: %d theme image(s) loaded (of %d types)\n",
	       cursor_loaded, BGCE_CURSOR_COUNT);
	printf("[BGCE] input: move_speed=%.2f pan_speed=%.2f "
	       "natural_scrolling=%s "
	       "(on-screen speed ≈ mouse × speed / zoom)\n",
	       (double)config->move_speed, (double)config->pan_speed,
	       config->natural_scrolling ? "on" : "off");
	printf("[BGCE] === end config ===\n");
}

/* Pack stbi RGBA → ARGB uint32. */
static inline uint32_t rgba_to_u32(const unsigned char *p)
{
	return ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) |
	       ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

// Apply background to a buffer (typically the full virtual desktop).
int apply_background(struct config* config, uint32_t* buffer,
                     uint32_t width, uint32_t height,
                     uint32_t tile_w, uint32_t tile_h) {
	if (!config || !buffer || width == 0 || height == 0)
		return -1;

	if (tile_w == 0)
		tile_w = width;
	if (tile_h == 0)
		tile_h = height;

	if (config->type == BG_COLOR) {
		for (uint32_t i = 0; i < width * height; i++)
			buffer[i] = config->color;
		return 0;
	}

	if (config->type != BG_IMAGE)
		return -1;

	int img_width, img_height, img_channels;
	unsigned char *img_data =
	        stbi_load(config->path, &img_width, &img_height, &img_channels, 4);
	if (!img_data) {
		fprintf(stderr, "[BGCE] Failed to load background image: %s\n",
		        config->path[0] ? config->path : "(empty path)");
		fprintf(stderr, "[BGCE] Falling back to default color #333333\n");
		for (uint32_t i = 0; i < width * height; i++)
			buffer[i] = 0xFF333333;
		return 0;
	}

	printf("[BGCE] Background image %s (%dx%d) mode=%s → canvas %ux%u\n",
	       config->path, img_width, img_height,
	       config->mode == IMAGE_TILED ? "tiled" : "scaled",
	       width, height);

	if (config->mode == IMAGE_TILED) {
		/* Repeat source texels over the whole canvas. */
		for (uint32_t y = 0; y < height; y++) {
			uint32_t img_y = y % (uint32_t)img_height;
			for (uint32_t x = 0; x < width; x++) {
				uint32_t img_x = x % (uint32_t)img_width;
				uint32_t img_idx = (img_y * (uint32_t)img_width + img_x) * 4;
				buffer[y * width + x] = rgba_to_u32(img_data + img_idx);
			}
		}
	} else {
		/*
		 * Scaled: stretch the image over the full canvas (virtual desktop).
		 * tile_w/tile_h are unused for this mode (kept for API stability).
		 */
		(void)tile_w;
		(void)tile_h;
		float x_ratio = (float)img_width / (float)width;
		float y_ratio = (float)img_height / (float)height;

		for (uint32_t y = 0; y < height; y++) {
			uint32_t img_y = (uint32_t)(y * y_ratio);
			if (img_y >= (uint32_t)img_height)
				img_y = (uint32_t)img_height - 1;
			for (uint32_t x = 0; x < width; x++) {
				uint32_t img_x = (uint32_t)(x * x_ratio);
				if (img_x >= (uint32_t)img_width)
					img_x = (uint32_t)img_width - 1;
				uint32_t img_idx =
				        (img_y * (uint32_t)img_width + img_x) * 4;
				buffer[y * width + x] = rgba_to_u32(img_data + img_idx);
			}
		}
	}

	stbi_image_free(img_data);
	return 0;
}
