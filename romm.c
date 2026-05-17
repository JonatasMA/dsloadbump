/*
 * romm.c - RoMM integration for DSLoad v2
 *
 * Implements a minimal HTTP/1.0 client using dswifi9's BSD socket API,
 * a lightweight JSON field extractor (no heap allocations), and an
 * interactive console UI for browsing and downloading ROMs from a local
 * RoMM server.
 */

#include "romm.h"

#include <nds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>


/* ==========================================================================
 * Log file
 * ========================================================================== */

static FILE *log_fp = NULL;

void romm_log_init(void) {
	log_fp = fopen(ROMM_LOG_FILE, "a");
	if (log_fp)
		fprintf(log_fp, "\n=== DSLoad v%s  RoMM session start ===\n",
		        ROMM_VERSION);
}

void romm_log_close(void) {
	if (log_fp) {
		fprintf(log_fp, "=== RoMM session end ===\n\n");
		fclose(log_fp);
		log_fp = NULL;
	}
}

void romm_log(const char *fmt, ...) {
	if (!log_fp) return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(log_fp, fmt, ap);
	va_end(ap);
	fputc('\n', log_fp);
	fflush(log_fp);
}


/* ==========================================================================
 * Config loader
 * ========================================================================== */

int romm_load_config(RommConfig *cfg) {
	memset(cfg, 0, sizeof(*cfg));
	/* Defaults */
	cfg->port = 3000;
	strcpy(cfg->download_dir, "/roms");

	FILE *fp = fopen(ROMM_CONFIG_FILE, "r");
	if (!fp) return 0;

	char line[256];
	while (fgets(line, sizeof(line), fp)) {
		/* Strip trailing whitespace */
		int len = strlen(line);
		while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'
		                   || line[len-1] == ' '))
			line[--len] = '\0';
		/* Skip blank lines and comments */
		if (line[0] == '#' || line[0] == '\0') continue;

		char *eq = strchr(line, '=');
		if (!eq) continue;
		*eq = '\0';
		const char *key = line;
		const char *val = eq + 1;

		if      (strcmp(key, "server")       == 0)
			strncpy(cfg->server,       val, sizeof(cfg->server)       - 1);
		else if (strcmp(key, "port")         == 0)
			cfg->port = atoi(val);
		else if (strcmp(key, "api_key")      == 0)
			strncpy(cfg->api_key,      val, sizeof(cfg->api_key)      - 1);
		else if (strcmp(key, "username")     == 0)
			strncpy(cfg->username,     val, sizeof(cfg->username)     - 1);
		else if (strcmp(key, "password")     == 0)
			strncpy(cfg->password,     val, sizeof(cfg->password)     - 1);
		else if (strcmp(key, "download_dir") == 0)
			strncpy(cfg->download_dir, val, sizeof(cfg->download_dir) - 1);
	}
	fclose(fp);
	return cfg->server[0] != '\0';
}


/* ==========================================================================
 * Utility: base64 encode (for HTTP Basic auth)
 * ========================================================================== */

static const char b64_chars[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64_encode(const char *in, int len, char *out, int max_out) {
	int i, j = 0;
	for (i = 0; i < len && j < max_out - 4; i += 3) {
		unsigned char a = (unsigned char)in[i];
		unsigned char b = (i+1 < len) ? (unsigned char)in[i+1] : 0;
		unsigned char c = (i+2 < len) ? (unsigned char)in[i+2] : 0;
		out[j++] = b64_chars[a >> 2];
		out[j++] = b64_chars[((a & 3) << 4) | (b >> 4)];
		out[j++] = (i+1 < len) ? b64_chars[((b & 0xF) << 2) | (c >> 6)] : '=';
		out[j++] = (i+2 < len) ? b64_chars[c & 0x3F] : '=';
	}
	if (j < max_out) out[j] = '\0';
}


/* ==========================================================================
 * Utility: URL percent-encoding
 * ========================================================================== */

static void url_encode(const char *src, char *dst, int max) {
	int di = 0;
	for (int si = 0; src[si] && di < max - 4; si++) {
		unsigned char c = (unsigned char)src[si];
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
		    (c >= '0' && c <= '9') ||
		    c == '-' || c == '_' || c == '.' || c == '~' ||
		    c == '(' || c == ')') {
			dst[di++] = (char)c;
		} else {
			di += snprintf(dst + di, max - di, "%%%02X",
			               (unsigned int)c);
		}
	}
	dst[di] = '\0';
}


/* ==========================================================================
 * Minimal JSON field extractor
 * No heap allocations; operates on a flat char buffer.
 * ========================================================================== */

/*
 * Starting at s[0] == '{', return the index of the matching '}'.
 * Returns -1 if not found.  Handles nested objects/arrays and strings.
 */
static int json_obj_end(const char *s, int len) {
	int i = 1, depth = 1, in_str = 0;
	while (i < len && depth > 0) {
		char c = s[i];
		if (!in_str) {
			if      (c == '"')             in_str = 1;
			else if (c == '{' || c == '[') depth++;
			else if (c == '}' || c == ']') {
				if (--depth == 0) return i;
			}
		} else {
			if      (c == '\\') i++;   /* skip escaped character */
			else if (c == '"')  in_str = 0;
		}
		i++;
	}
	return -1;
}

/*
 * Return a pointer to the value that follows "key": in [s, s+len).
 * Returns NULL if the key is not found.
 */
static const char *json_find_key(const char *s, int len, const char *key) {
	char needle[80];
	int nlen = snprintf(needle, sizeof(needle), "\"%s\":", key);
	for (int i = 0; i <= len - nlen; i++) {
		if (memcmp(s + i, needle, nlen) == 0)
			return s + i + nlen;
	}
	return NULL;
}

/* Extract an integer value; returns 1 on success. */
static int json_int(const char *s, int len, const char *key, int *out) {
	const char *v = json_find_key(s, len, key);
	if (!v) return 0;
	while (*v == ' ' || *v == '\t') v++;
	char *end;
	long val = strtol(v, &end, 10);
	if (end == v) return 0;
	*out = (int)val;
	return 1;
}

/* Extract a string value; returns 1 on success. */
static int json_str(const char *s, int len, const char *key,
                    char *out, int max) {
	const char *v = json_find_key(s, len, key);
	if (!v) return 0;
	while (*v == ' ' || *v == '\t') v++;
	if (*v != '"') return 0;
	v++;
	int i = 0;
	while (*v && i < max - 1) {
		if (*v == '\\') {
			v++;
			switch (*v) {
				case 'n':  out[i++] = '\n'; break;
				case 't':  out[i++] = '\t'; break;
				case '"':  out[i++] = '"';  break;
				case '\\': out[i++] = '\\'; break;
				default:   out[i++] = *v;   break;
			}
			if (*v) v++;
		} else if (*v == '"') {
			break;
		} else {
			out[i++] = *v++;
		}
	}
	out[i] = '\0';
	return 1;
}

/*
 * Case-insensitive substring search (replaces strstr for header parsing).
 */
static const char *stristr(const char *hay, const char *needle) {
	int nlen = strlen(needle);
	while (*hay) {
		int ok = 1;
		for (int i = 0; i < nlen; i++) {
			if (tolower((unsigned char)hay[i]) !=
			    tolower((unsigned char)needle[i])) {
				ok = 0;
				break;
			}
		}
		if (ok) return hay;
		hay++;
	}
	return NULL;
}


/* ==========================================================================
 * HTTP/1.0 client  (dswifi9 BSD socket API)
 * Static buffers keep everything off the DS stack.
 * ========================================================================== */

/* JSON response buffer – reused by every API call (single-threaded DS). */
static char http_buf[ROMM_HTTP_BUF_SIZE];

/* Download chunk buffer – used only during file streaming. */
static char dl_buf[32768];

/* ---- auth helpers -------------------------------------------------------- */

/*
 * Append authentication query-string parameter if api_key is configured.
 * Writes the final path (with optional ?api_key=…) into buf and returns it.
 */
static void build_url_path(const RommConfig *cfg, const char *path,
                            char *buf, int max) {
	if (cfg->api_key[0]) {
		const char *sep = strchr(path, '?') ? "&" : "?";
		snprintf(buf, max, "%s%sapi_key=%s",
		         path, sep, cfg->api_key);
	} else {
		strncpy(buf, path, max - 1);
		buf[max-1] = '\0';
	}
}

/*
 * Build an Authorization header line (Basic auth) when api_key is absent
 * but username is present.  buf receives the full "Authorization: …\r\n"
 * string (empty if not applicable).
 */
static void build_auth_header(const RommConfig *cfg, char *buf, int max) {
	buf[0] = '\0';
	if (cfg->api_key[0]) return;       /* API key takes precedence */
	if (!cfg->username[0]) return;

	char creds[128];
	char b64[200];
	snprintf(creds, sizeof(creds), "%s:%s",
	         cfg->username, cfg->password);
	base64_encode(creds, strlen(creds), b64, sizeof(b64));
	snprintf(buf, max, "Authorization: Basic %s\r\n", b64);
}

/* ---- connect ------------------------------------------------------------- */

static int http_connect(const RommConfig *cfg) {
	struct hostent *he = gethostbyname(cfg->server);
	if (!he) {
		romm_log("DNS failed for: %s", cfg->server);
		return -1;
	}
	int sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
		romm_log("socket() failed");
		return -1;
	}
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port   = htons((uint16_t)cfg->port);
	addr.sin_addr   = *(struct in_addr *)he->h_addr;

	if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		romm_log("connect() failed: %s:%d", cfg->server, cfg->port);
		closesocket(sock);
		return -1;
	}
	return sock;
}

/* ---- header parsing ------------------------------------------------------ */

/*
 * Read HTTP response headers into hdr_buf (one byte at a time to avoid
 * consuming any body bytes).  Returns header length on success, -1 on error.
 */
static int http_read_headers(int sock, char *hdr_buf, int max) {
	int len = 0;
	while (len < max - 1) {
		char c;
		if (recv(sock, &c, 1, 0) <= 0) break;
		hdr_buf[len++] = c;
		if (len >= 4 &&
		    hdr_buf[len-4] == '\r' && hdr_buf[len-3] == '\n' &&
		    hdr_buf[len-2] == '\r' && hdr_buf[len-1] == '\n') {
			hdr_buf[len] = '\0';
			return len;
		}
	}
	hdr_buf[len] = '\0';
	return -1;
}

static int http_parse_status(const char *hdr) {
	const char *sp = strchr(hdr, ' ');
	return sp ? atoi(sp + 1) : -1;
}

static int http_parse_content_length(const char *hdr) {
	const char *p = stristr(hdr, "content-length:");
	return p ? atoi(p + 15) : -1;
}

/* ---- GET → JSON buffer --------------------------------------------------- */

/*
 * Perform an HTTP/1.0 GET and buffer the entire response body in http_buf.
 * Returns body length on success, -1 on any error.
 * The caller must use http_buf immediately before the next call overwrites it.
 */
static int http_get_json(const RommConfig *cfg, const char *path) {
	char full_path[512];
	build_url_path(cfg, path, full_path, sizeof(full_path));

	char auth_hdr[256];
	build_auth_header(cfg, auth_hdr, sizeof(auth_hdr));

	romm_log("GET %s", full_path);

	int sock = http_connect(cfg);
	if (sock < 0) return -1;

	/* Send HTTP/1.0 GET request */
	char req[1024];
	int req_len = snprintf(req, sizeof(req),
		"GET %s HTTP/1.0\r\n"
		"Host: %s:%d\r\n"
		"%s"
		"\r\n",
		full_path, cfg->server, cfg->port, auth_hdr);
	if (send(sock, req, req_len, 0) < 0) {
		romm_log("send() failed");
		closesocket(sock);
		return -1;
	}

	/* Read response headers */
	char hdr[2048];
	if (http_read_headers(sock, hdr, sizeof(hdr)) < 0) {
		romm_log("Incomplete HTTP headers");
		closesocket(sock);
		return -1;
	}

	int status         = http_parse_status(hdr);
	int content_length = http_parse_content_length(hdr);
	romm_log("HTTP %d  Content-Length: %d", status, content_length);

	if (status != 200) {
		closesocket(sock);
		return -1;
	}

	/* Read body into static buffer */
	int body_len = 0;
	int limit = (content_length > 0 && content_length < ROMM_HTTP_BUF_SIZE - 1)
	            ? content_length
	            : ROMM_HTTP_BUF_SIZE - 1;

	while (body_len < limit) {
		int want = limit - body_len;
		if (want > 4096) want = 4096;
		int r = recv(sock, http_buf + body_len, want, 0);
		if (r <= 0) break;
		body_len += r;
	}
	http_buf[body_len] = '\0';

	closesocket(sock);
	romm_log("Body received: %d bytes", body_len);
	return body_len;
}

/* ---- GET → file stream --------------------------------------------------- */

/*
 * Perform an HTTP/1.0 GET and stream the response body directly to a file.
 * Prints progress to the console.
 * Returns bytes written on success, -1 on error.
 */
static int http_download_to_file(const RommConfig *cfg, const char *path,
                                 const char *local_path) {
	char full_path[512];
	build_url_path(cfg, path, full_path, sizeof(full_path));

	char auth_hdr[256];
	build_auth_header(cfg, auth_hdr, sizeof(auth_hdr));

	romm_log("DOWNLOAD %s -> %s", full_path, local_path);

	int sock = http_connect(cfg);
	if (sock < 0) return -1;

	char req[1024];
	int req_len = snprintf(req, sizeof(req),
		"GET %s HTTP/1.0\r\n"
		"Host: %s:%d\r\n"
		"%s"
		"\r\n",
		full_path, cfg->server, cfg->port, auth_hdr);
	if (send(sock, req, req_len, 0) < 0) {
		romm_log("send() failed");
		closesocket(sock);
		return -1;
	}

	/* Read headers */
	char hdr[2048];
	if (http_read_headers(sock, hdr, sizeof(hdr)) < 0) {
		romm_log("Incomplete HTTP headers");
		closesocket(sock);
		return -1;
	}

	int status         = http_parse_status(hdr);
	int content_length = http_parse_content_length(hdr);
	romm_log("HTTP %d  Content-Length: %d", status, content_length);

	if (status != 200) {
		closesocket(sock);
		return -1;
	}

	FILE *fp = fopen(local_path, "wb");
	if (!fp) {
		romm_log("Cannot create file: %s", local_path);
		closesocket(sock);
		return -1;
	}

	/* Stream body to file */
	int total          = 0;
	int last_pct_shown = -1;

	while (content_length < 0 || total < content_length) {
		int want = sizeof(dl_buf);
		if (content_length > 0) {
			int left = content_length - total;
			if (left < want) want = left;
		}
		int r = recv(sock, dl_buf, want, 0);
		if (r <= 0) break;
		fwrite(dl_buf, 1, r, fp);
		fflush(fp);
		total += r;

		/* Redraw progress line */
		if (content_length > 0) {
			int pct = total * 100 / content_length;
			if (pct != last_pct_shown) {
				printf("\r%d/%d KB (%d%%)   ",
				       total / 1024,
				       content_length / 1024,
				       pct);
				last_pct_shown = pct;
			}
		} else {
			/* Unknown size – print dots every 32 KB */
			int dots_now = total / 32768;
			int dots_old = (total - r) / 32768;
			while (dots_old < dots_now) {
				printf(".");
				dots_old++;
			}
		}
		swiWaitForVBlank();
	}
	printf("\n");

	fclose(fp);
	closesocket(sock);
	romm_log("Download complete: %d bytes", total);
	return total;
}


/* ==========================================================================
 * Supported-platform filter
 * ========================================================================== */

static const char *const supported_slugs[] = {
	"nds", "gba", "gb", "gbc", "genesis", "megadrive", "snes", NULL
};

static int is_supported(const char *fs_slug) {
	for (int i = 0; supported_slugs[i]; i++) {
		/* Case-insensitive compare */
		const char *a = fs_slug, *b = supported_slugs[i];
		int match = 1;
		while (*a || *b) {
			if (tolower((unsigned char)*a) !=
			    tolower((unsigned char)*b)) { match = 0; break; }
			a++; b++;
		}
		if (match) return 1;
	}
	return 0;
}


/* ==========================================================================
 * Public RoMM API functions
 * ========================================================================== */

int romm_get_platforms(const RommConfig *cfg,
                       RommPlatform *out, int max_count) {
	int body = http_get_json(cfg, "/api/platforms");
	if (body < 0) return -1;

	/* Response is a direct JSON array: [{...}, ...] */
	const char *p = http_buf;
	while (*p && *p != '[') p++;
	if (!*p) { romm_log("No JSON array in platforms response"); return 0; }
	p++;  /* skip '[' */

	int count = 0;
	while (*p && count < max_count) {
		while (*p == ' ' || *p == '\t' || *p == '\n' ||
		       *p == '\r' || *p == ',') p++;
		if (*p != '{') break;

		int obj_end = json_obj_end(p, body - (int)(p - http_buf));
		if (obj_end < 0) break;

		RommPlatform pl;
		memset(&pl, 0, sizeof(pl));
		if (json_int(p, obj_end + 1, "id", &pl.id) &&
		    json_str(p, obj_end + 1, "fs_slug",
		             pl.fs_slug, sizeof(pl.fs_slug)) &&
		    is_supported(pl.fs_slug)) {
			json_str(p, obj_end + 1, "name", pl.name, sizeof(pl.name));
			if (!pl.name[0])
				strncpy(pl.name, pl.fs_slug, sizeof(pl.name) - 1);
			out[count++] = pl;
			romm_log("Platform: [%d] %s (%s)",
			         pl.id, pl.name, pl.fs_slug);
		}
		p += obj_end + 1;
	}
	return count;
}


int romm_get_roms(const RommConfig *cfg, int platform_id,
                  int offset, int limit,
                  RommRom *out, int max_count, int *out_total) {
	char path[256];
	snprintf(path, sizeof(path),
	         "/api/roms?platform_ids=%d&limit=%d&offset=%d",
	         platform_id, limit, offset);

	int body = http_get_json(cfg, path);
	if (body < 0) return -1;

	/* Response: {"items":[...], "total":N, ...} */
	if (out_total) {
		int total = 0;
		json_int(http_buf, body, "total", &total);
		*out_total = total;
	}

	/* Locate "items": [ */
	const char *arr = NULL;
	const char *v = json_find_key(http_buf, body, "items");
	if (v) {
		while (*v == ' ') v++;
		if (*v == '[') arr = v;
	}
	if (!arr) { romm_log("No items array in ROMs response"); return 0; }
	arr++;  /* skip '[' */

	int count = 0;
	const char *p = arr;
	while (*p && count < max_count) {
		while (*p == ' ' || *p == '\t' || *p == '\n' ||
		       *p == '\r' || *p == ',') p++;
		if (*p != '{') break;

		int obj_end = json_obj_end(p, body - (int)(p - http_buf));
		if (obj_end < 0) break;

		RommRom rom;
		memset(&rom, 0, sizeof(rom));
		if (json_int(p, obj_end + 1, "id", &rom.id) &&
		    json_str(p, obj_end + 1, "fs_name",
		             rom.fs_name, sizeof(rom.fs_name))) {
			json_str(p, obj_end + 1, "name", rom.name, sizeof(rom.name));
			if (!rom.name[0])
				strncpy(rom.name, rom.fs_name, sizeof(rom.name) - 1);
			out[count++] = rom;
			romm_log("ROM: [%d] %s", rom.id, rom.name);
		}
		p += obj_end + 1;
	}
	return count;
}


int romm_download_rom(const RommConfig *cfg, int rom_id,
                      const char *fs_name, const char *dest_dir) {
	/* URL-encode the filename for the API path */
	char enc_name[384];
	url_encode(fs_name, enc_name, sizeof(enc_name));

	char api_path[512];
	snprintf(api_path, sizeof(api_path),
	         "/api/roms/%d/content/%s", rom_id, enc_name);

	/* Build local destination path: dest_dir/fs_name */
	char local_path[256];
	snprintf(local_path, sizeof(local_path), "%s/%s", dest_dir, fs_name);

	/* Ensure destination directory exists (best-effort) */
	mkdir(dest_dir, 0755);

	return http_download_to_file(cfg, api_path, local_path);
}


/* ==========================================================================
 * Interactive console UI
 * ========================================================================== */

static void wait_any_key(void) {
	printf("\nPress any key...\n");
	while (1) {
		swiWaitForVBlank();
		scanKeys();
		if (keysDown()) break;
	}
}

/*
 * Generic scrollable list menu.
 * items[]  – array of strings to display
 * count    – number of items
 * title    – header line (max 32 chars)
 * Returns selected index (0-based), or -1 if B / START pressed.
 */
static int menu_select(const char **items, int count, const char *title) {
	int sel   = 0;
	int dirty = 1;

	while (1) {
		swiWaitForVBlank();
		scanKeys();
		int keys = keysDown();

		if (keys & KEY_UP)    { if (--sel < 0) sel = count - 1; dirty = 1; }
		if (keys & KEY_DOWN)  { if (++sel >= count) sel = 0;    dirty = 1; }
		if (keys & KEY_A)     return sel;
		if ((keys & KEY_B) || (keys & KEY_START)) return -1;

		if (!dirty) continue;

		consoleClear();
		printf("%-32.32s\n", title);
		printf("--------------------------------\n");

		/* Show a scrolling window of up to 18 entries */
		int start = sel - 9;
		if (start < 0) start = 0;
		if (start > count - 18) start = count - 18;
		if (start < 0) start = 0;
		int end = start + 18;
		if (end > count) end = count;

		for (int i = start; i < end; i++) {
			if (i == sel)
				printf("> %-28.28s\n", items[i]);
			else
				printf("  %-28.28s\n", items[i]);
		}

		printf("--------------------------------\n");
		printf("UP/DN:nav  A:ok  B:back\n");
		dirty = 0;
	}
}


void romm_run_mode(const RommConfig *cfg) {
	romm_log_init();
	romm_log("romm_run_mode: server=%s:%d", cfg->server, cfg->port);

	consoleClear();
	printf("=== RoMM Download Mode ===\n");
	printf("Server: %s:%d\n\n", cfg->server, cfg->port);
	printf("Loading platforms...\n");

	/* --- Fetch supported platforms ---------------------------------------- */
	RommPlatform platforms[ROMM_MAX_PLATFORMS];
	int plat_count = romm_get_platforms(cfg, platforms, ROMM_MAX_PLATFORMS);

	if (plat_count <= 0) {
		printf("\nNo supported platforms found.\n");
		printf("Check server/romm.cfg.\n");
		printf("Log: %s\n", ROMM_LOG_FILE);
		romm_log("No platforms returned (count=%d)", plat_count);
		wait_any_key();
		romm_log_close();
		return;
	}

	/* --- Build platform menu labels --------------------------------------- */
	const char *plat_items[ROMM_MAX_PLATFORMS];
	char plat_labels[ROMM_MAX_PLATFORMS][50];
	for (int i = 0; i < plat_count; i++) {
		snprintf(plat_labels[i], sizeof(plat_labels[i]),
		         "%-28.28s", platforms[i].name);
		plat_items[i] = plat_labels[i];
	}

	/* --- Platform selection loop ------------------------------------------ */
	while (1) {
		int psel = menu_select(plat_items, plat_count, "Select Platform");
		if (psel < 0) break;   /* B pressed – exit RoMM mode */

		int platform_id = platforms[psel].id;
		int offset      = 0;
		int total       = 0;

		/* --- ROM browse loop ---------------------------------------------- */
		while (1) {
			consoleClear();
			printf("Loading ROMs...\n");

			RommRom roms[ROMM_MAX_ROMS];
			int rom_count = romm_get_roms(cfg, platform_id, offset,
			                              ROMM_PAGE_SIZE, roms,
			                              ROMM_MAX_ROMS, &total);

			if (rom_count < 0) {
				printf("Error fetching ROMs.\n");
				printf("Log: %s\n", ROMM_LOG_FILE);
				romm_log("romm_get_roms error (platform=%d offset=%d)",
				         platform_id, offset);
				wait_any_key();
				break;
			}

			if (rom_count == 0) {
				printf("No ROMs found.\n");
				wait_any_key();
				break;
			}

			/* Build ROM menu labels */
			const char *rom_items[ROMM_MAX_ROMS];
			char rom_labels[ROMM_MAX_ROMS][50];
			for (int i = 0; i < rom_count; i++) {
				snprintf(rom_labels[i], sizeof(rom_labels[i]),
				         "%-28.28s", roms[i].name);
				rom_items[i] = rom_labels[i];
			}

			int page_num = offset / ROMM_PAGE_SIZE + 1;
			int page_max = (total + ROMM_PAGE_SIZE - 1) / ROMM_PAGE_SIZE;
			if (page_max < 1) page_max = 1;

			char page_title[36];
			snprintf(page_title, sizeof(page_title),
			         "%.16s  p%d/%d",
			         platforms[psel].name, page_num, page_max);

			/* ROM list with paging */
			int sel   = 0;
			int dirty = 1;
			int rsel  = -2;   /* -2 = redraw/re-fetch */

			while (rsel == -2) {
				swiWaitForVBlank();
				scanKeys();
				int keys = keysDown();

				if (keys & KEY_UP)   { if (--sel < 0) sel = rom_count-1; dirty=1; }
				if (keys & KEY_DOWN) { if (++sel >= rom_count) sel=0;    dirty=1; }
				if (keys & KEY_A)    { rsel = sel; }
				if ((keys & KEY_B) || (keys & KEY_START)) { rsel = -1; }

				/* L – previous page */
				if ((keys & KEY_L) && offset >= ROMM_PAGE_SIZE) {
					offset -= ROMM_PAGE_SIZE;
					break;   /* re-fetch */
				}
				/* R – next page */
				if ((keys & KEY_R) && offset + ROMM_PAGE_SIZE < total) {
					offset += ROMM_PAGE_SIZE;
					break;   /* re-fetch */
				}

				if (!dirty) continue;

				consoleClear();
				printf("%-32.32s\n", page_title);
				printf("--------------------------------\n");
				for (int i = 0; i < rom_count; i++) {
					if (i == sel)
						printf("> %-28.28s\n", roms[i].name);
					else
						printf("  %-28.28s\n", roms[i].name);
				}
				printf("--------------------------------\n");
				printf("A:dl B:back L/R:page\n");
				dirty = 0;
			}

			if (rsel == -1) break;        /* back to platform select */
			if (rsel == -2) continue;     /* paging – re-fetch */

			/* --- Confirm download ----------------------------------------- */
			consoleClear();
			printf("=== Confirm Download ===\n\n");
			printf("%.28s\n\n",  roms[rsel].name);
			printf("%.28s\n",    roms[rsel].fs_name);
			printf("-> %.26s\n", cfg->download_dir);
			printf("\nA: Download   B: Cancel\n");

			int confirmed = 0;
			while (1) {
				swiWaitForVBlank();
				scanKeys();
				int k = keysDown();
				if (k & KEY_B) break;
				if (k & KEY_A) { confirmed = 1; break; }
			}

			if (!confirmed) continue;

			/* --- Download ------------------------------------------------- */
			consoleClear();
			printf("Downloading:\n%.28s\n\n", roms[rsel].fs_name);

			int bytes = romm_download_rom(cfg,
			                              roms[rsel].id,
			                              roms[rsel].fs_name,
			                              cfg->download_dir);
			if (bytes < 0) {
				printf("\nERROR: download failed.\n");
				printf("Check %s\n", ROMM_LOG_FILE);
				romm_log("FAIL: %s", roms[rsel].fs_name);
			} else {
				printf("\nDone!  %.2f MB\n",
				       (float)bytes / (1024.0f * 1024.0f));
				romm_log("OK: %s  (%d bytes)",
				         roms[rsel].fs_name, bytes);
			}

			wait_any_key();
		}  /* ROM browse loop */
	}  /* Platform selection loop */

	romm_log_close();
}
