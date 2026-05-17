/*
 * romm.h - RoMM (ROM Manager) integration for DSLoad v2
 *
 * Provides WiFi-based ROM downloading from a local RoMM server
 * (https://github.com/rommapp/romm) using its HTTP/JSON API.
 * Supported platforms: NDS, GBA, GB, GBC, Mega Drive/Genesis, SNES.
 *
 * Authentication:
 *   - API key  : recommended; set api_key= in romm.cfg
 *   - Username / Password: falls back to HTTP Basic auth header
 *
 * A debug log is written to ROMM_LOG_FILE on the SD card during the
 * development phase.
 */

#pragma once

#include <stdio.h>
#include <stdarg.h>

/* --------------------------------------------------------------------------
 * Version / file paths
 * -------------------------------------------------------------------------- */

#define ROMM_VERSION     "2.00"
#define ROMM_CONFIG_FILE "/romm.cfg"
#define ROMM_LOG_FILE    "/romm_debug.log"

/* --------------------------------------------------------------------------
 * Tuning constants
 * -------------------------------------------------------------------------- */

#define ROMM_MAX_PLATFORMS  16
#define ROMM_MAX_ROMS       15   /* ROMs shown per page (fits on 32×24 console) */
#define ROMM_PAGE_SIZE      15
#define ROMM_HTTP_BUF_SIZE  65536  /* 64 KB – JSON API response buffer          */

/* --------------------------------------------------------------------------
 * Data structures
 * -------------------------------------------------------------------------- */

/* Configuration – loaded from ROMM_CONFIG_FILE */
typedef struct {
	char server[64];       /* RoMM host: IP address or hostname */
	int  port;             /* HTTP port (default 3000)          */
	char api_key[128];     /* API key (preferred auth method)   */
	char username[32];     /* Username for Basic auth fallback  */
	char password[64];     /* Password for Basic auth fallback  */
	char download_dir[64]; /* Destination directory on SD card  */
} RommConfig;

typedef struct {
	int  id;
	char name[48];
	char fs_slug[16];
} RommPlatform;

typedef struct {
	int  id;
	char name[64];
	char fs_name[128];
} RommRom;

/* --------------------------------------------------------------------------
 * Config
 * -------------------------------------------------------------------------- */

/* Load config from ROMM_CONFIG_FILE.
 * Returns 1 if server= is set (minimum viable config), 0 otherwise. */
int romm_load_config(RommConfig *cfg);

/* --------------------------------------------------------------------------
 * Debug logging  (romm_log_init / romm_log_close bracket a RoMM session)
 * -------------------------------------------------------------------------- */

void romm_log_init(void);
void romm_log_close(void);
void romm_log(const char *fmt, ...);

/* --------------------------------------------------------------------------
 * RoMM API  (WiFi must already be up)
 * -------------------------------------------------------------------------- */

/* Returns number of supported platforms found, or -1 on HTTP error. */
int romm_get_platforms(const RommConfig *cfg,
                       RommPlatform *out, int max_count);

/* Returns number of ROMs returned (<= max_count), or -1 on HTTP error.
 * *out_total receives the server-side total for the platform (for paging). */
int romm_get_roms(const RommConfig *cfg, int platform_id,
                  int offset, int limit,
                  RommRom *out, int max_count, int *out_total);

/* Download a single ROM to dest_dir/fs_name.
 * Prints progress dots to the console during transfer.
 * Returns total bytes written, or -1 on error. */
int romm_download_rom(const RommConfig *cfg, int rom_id,
                      const char *fs_name, const char *dest_dir);

/* --------------------------------------------------------------------------
 * Interactive mode  (full platform → ROM → download UI)
 * -------------------------------------------------------------------------- */

void romm_run_mode(const RommConfig *cfg);
