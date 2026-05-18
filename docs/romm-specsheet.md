# RomM API Integration Spec — Nintendo DS Homebrew

> **Target:** `rommapp/romm` self-hosted instance  
> **Purpose:** Reference document for a Nintendo DS homebrew (forked from [dsload](https://github.com/Lameguy64/dsload)) that fetches ROMs directly from a RomM instance over WiFi — no PC intermediary.  
> **API Base URL:** `http://<romm-host>/api`  
> **Swagger UI:** `http://<romm-host>/api/docs`

---

## Table of Contents

1. [Authentication](#1-authentication)
2. [Scopes Reference](#2-scopes-reference)
3. [Platforms](#3-platforms)
4. [ROMs](#4-roms)
5. [ROM Files & Download](#5-rom-files--download)
6. [Collections](#6-collections)
7. [Heartbeat / Health Check](#7-heartbeat--health-check)
8. [Practical DS Integration Flow](#8-practical-ds-integration-flow)
9. [HTTP Considerations for DS Homebrew](#9-http-considerations-for-ds-homebrew)
10. [Response Schemas (Minimal)](#10-response-schemas-minimal)

---

## 1. Authentication

RomM supports four authentication methods. For a DS homebrew, **HTTP Basic auth** or a **long-lived Client API Token** (Bearer) are the most practical — sessions and OIDC require browser-level flows.

---

### 1.1 Session Login (HTTP Basic → Cookie)

> Not recommended for embedded devices; cookies must be persisted across requests.

```
POST /api/login
Authorization: Basic <base64(username:password)>
```

- Returns a `Set-Cookie: romm_session=...` header on success (`200 OK`).
- All subsequent requests must include that cookie.
- Session is stored server-side in Redis.

---

### 1.2 OAuth2 Bearer Token (Recommended for DS)

Get a short-lived JWT access token plus a refresh token:

```
POST /api/token
Content-Type: application/x-www-form-urlencoded

grant_type=password&username=<user>&password=<pass>
```

**Response:**
```json
{
  "access_token": "<JWT>",
  "refresh_token": "<refresh_JWT>",
  "token_type": "bearer",
  "expires": 3600,
  "refresh_expires": 86400
}
```

Use the access token in all subsequent requests:
```
Authorization: Bearer <access_token>
```

**Refresh the access token** before it expires:
```
POST /api/token
Content-Type: application/x-www-form-urlencoded

grant_type=refresh_token&refresh_token=<refresh_JWT>
```

---

### 1.3 Client API Token (Best for DS — Long-Lived)

**Generate from the RomM web UI:** Settings → API Tokens → Create.

Token format: `rmm_` + 64 hex characters.

Use as a Bearer token on every request:
```
Authorization: Bearer rmm_<64hexchars>
```

- Does **not** expire by default (configurable).
- Scope-limited (you choose which scopes to assign at creation).
- No session management, no refresh needed — ideal for embedded devices.

> **Recommendation:** Create a token with `ROMS_READ`, `PLATFORMS_READ`, `COLLECTIONS_READ` scopes. Store it in your DS ROM or on the SD card.

---

### 1.4 HTTP Basic (Stateless — Simple but Less Secure)

Send credentials on every request:
```
Authorization: Basic <base64(username:password)>
```

Works with `@protected_route` endpoints. No cookie/session needed.

---

## 2. Scopes Reference

These scopes control what an API token or OAuth token can do:

| Scope              | Description                     |
|--------------------|---------------------------------|
| `ROMS_READ`        | List and download ROMs          |
| `ROMS_WRITE`       | Modify/delete ROMs (not needed) |
| `PLATFORMS_READ`   | List platforms                  |
| `COLLECTIONS_READ` | List collections and their ROMs |
| `ME_READ`          | Read own user profile           |

For a read-only DS client, you only need: **`ROMS_READ PLATFORMS_READ COLLECTIONS_READ`**

---

## 3. Platforms

### List All Platforms

```
GET /api/platforms
Authorization: Bearer <token>
```

**Query Parameters:**

| Parameter      | Type   | Description                                  |
|----------------|--------|----------------------------------------------|
| `updated_after`| string | ISO 8601 datetime — only return newer entries|

**Response:** Array of platform objects.

```json
[
  {
    "id": 4,
    "slug": "nds",
    "fs_slug": "nds",
    "name": "Nintendo DS",
    "custom_name": null,
    "rom_count": 142,
    "fs_size_bytes": 2147483648,
    "category": "Portable",
    "generation": 7
  }
]
```

### Get Single Platform

```
GET /api/platforms/{id}
Authorization: Bearer <token>
```

### Get Platform IDs Only (Lightweight)

```
GET /api/platforms/identifiers
Authorization: Bearer <token>
```

Returns: `[1, 2, 3, 4, ...]`

---

## 4. ROMs

### List ROMs (Paginated, Filterable)

```
GET /api/roms
Authorization: Bearer <token>
```

**Query Parameters:**

| Parameter               | Type    | Description                                                      |
|-------------------------|---------|------------------------------------------------------------------|
| `platform_ids`          | int[]   | Filter by platform ID (repeat param for multiple)                |
| `collection_id`         | int     | Filter by collection ID                                          |
| `search_term`           | string  | Search by ROM name                                               |
| `limit`                 | int     | Page size (default: 60)                                          |
| `offset`                | int     | Page offset (default: 0)                                         |
| `with_char_index`       | bool    | Include alphabetical index (default: true — set false to save bandwidth) |
| `with_filter_values`    | bool    | Include filter metadata (default: true — set false to save bandwidth) |

**Example — Fetch first 20 NDS ROMs:**
```
GET /api/roms?platform_ids=4&limit=20&offset=0&with_char_index=false&with_filter_values=false
```

**Response:**
```json
{
  "items": [
    {
      "id": 101,
      "name": "Mario Kart DS",
      "slug": "mario-kart-ds",
      "fs_name": "Mario Kart DS (USA).nds",
      "fs_name_no_ext": "Mario Kart DS (USA)",
      "fs_extension": "nds",
      "fs_size_bytes": 67108864,
      "platform_id": 4,
      "platform_slug": "nds",
      "regions": ["USA"],
      "languages": ["en"],
      "tags": [],
      "revision": null,
      "multi": false,
      "files": []
    }
  ],
  "total": 142,
  "limit": 20,
  "offset": 0
}
```

### Get ROM Details

```
GET /api/roms/{id}
Authorization: Bearer <token>
```

Returns a `DetailedRomSchema` with full metadata, file list, cover paths, etc.

### Get ROM IDs Only (Lightweight Sync)

```
GET /api/roms/identifiers
Authorization: Bearer <token>
```

### Search ROMs by Name

```
GET /api/roms?search_term=mario&platform_ids=4&limit=10&offset=0
```

---

## 5. ROM Files & Download

### Download a Single ROM (Single File)

```
GET /api/roms/{id}/files/content/{file_name}
Authorization: Bearer <token>
```

- Returns the raw binary file stream.
- If nginx is configured with X-Accel-Redirect, the response is served directly by nginx (transparent to client).
- `file_name` must match the ROM's `fs_name` field.

**Example:**
```
GET /api/roms/101/files/content/Mario%20Kart%20DS%20%28USA%29.nds
```

### Get ROM File Metadata

```
GET /api/roms/{id}/files
Authorization: Bearer <token>
```

Returns an array of file objects for the ROM:
```json
[
  {
    "id": 55,
    "file_name": "Mario Kart DS (USA).nds",
    "file_path": "nds/Mario Kart DS (USA).nds",
    "file_size_bytes": 67108864,
    "crc_hash": "A1B2C3D4",
    "md5_hash": "abc123...",
    "sha1_hash": "def456...",
    "category": "GAME"
  }
]
```

### Bulk Download ROMs as ZIP

```
GET /api/roms/download?rom_ids=101,102,103
Authorization: Bearer <token>
```

**Query Parameters:**

| Parameter  | Type   | Required | Description                        |
|------------|--------|----------|------------------------------------|
| `rom_ids`  | string | Yes      | Comma-separated ROM IDs            |
| `filename` | string | No       | Custom name for the resulting .zip |

**Response:** `application/zip` binary stream.

> **Note for DS homebrew:** The bulk ZIP download requires ZIP decompression on the DS side. For simplicity, prefer downloading individual ROM files via `/files/content/{name}`.

---

## 6. Collections

### List Collections

```
GET /api/collections
Authorization: Bearer <token>
```

**Response:**
```json
[
  {
    "id": 3,
    "name": "My NDS Games",
    "description": "Handpicked NDS titles",
    "is_public": true,
    "is_favorite": false,
    "rom_count": 12
  }
]
```

### Get Collection (with ROM IDs)

```
GET /api/collections/{id}
Authorization: Bearer <token>
```

Returns the collection object including its `rom_ids` array.

### List ROMs in a Collection

Use the ROMs endpoint with `collection_id` filter:

```
GET /api/roms?collection_id=3&limit=20&offset=0
Authorization: Bearer <token>
```

---

## 7. Heartbeat / Health Check

Use this on startup to verify the RomM server is reachable and get version info:

```
GET /api/heartbeat
```

No authentication required.

**Response:**
```json
{
  "version": "3.x.x",
  "env": {
    "ROMM_AUTH_ENABLED": true,
    "DISABLE_DOWNLOAD_ENDPOINT_AUTH": false
  },
  "metadata_sources": {
    "igdb_enabled": true,
    "moby_enabled": false
  }
}
```

> Check `DISABLE_DOWNLOAD_ENDPOINT_AUTH` — if `true`, download endpoints require no auth and you can skip token management entirely.

---

## 8. Practical DS Integration Flow

This is the recommended sequence for a DS homebrew using RomM as a middleware ROM server:

```
┌──────────────────────────────────────────────────────────┐
│                  DS Boot / WiFi Connect                   │
└────────────────────┬─────────────────────────────────────┘
                     │
                     ▼
        GET /api/heartbeat
        (verify server reachable, check auth required)
                     │
                     ▼
        POST /api/token  (if auth enabled)
        grant_type=password OR
        use stored rmm_ client token as Bearer
                     │
                     ▼
        GET /api/platforms
        (show user list of available platforms)
                     │
                     ▼
        GET /api/roms?platform_ids={id}&limit=20&offset=0
        &with_char_index=false&with_filter_values=false
        (paginated ROM browser — user scrolls through)
                     │
              ┌──────┴──────┐
              │  (optional) │
              ▼             ▼
   GET /api/collections   GET /api/roms?search_term=...
   (browse by collection) (search by name)
              │
              └──────┬──────┘
                     │
                     ▼
        GET /api/roms/{id}
        (user selects a ROM — get file list + metadata)
                     │
                     ▼
        GET /api/roms/{id}/files
        (confirm filename and size before download)
                     │
                     ▼
        GET /api/roms/{id}/files/content/{file_name}
        (stream binary ROM file to SD card)
                     │
                     ▼
              Launch / Done
```

---

## 9. HTTP Considerations for DS Homebrew

The DS (via dswifi / dsload architecture) uses simple HTTP/1.0 or HTTP/1.1 GET requests. Keep these constraints in mind:

### Headers to Send

```
GET /api/roms?platform_ids=4&limit=20&offset=0 HTTP/1.1
Host: 192.168.1.100:8080
Authorization: Bearer rmm_<token>
Connection: close
Accept: application/json
```

> Use `Connection: close` — the DS TCP stack typically doesn't handle persistent connections well.

### Parsing JSON on DS

RomM returns standard JSON. Key fields for a minimal DS client:

| Field             | Use                                    |
|-------------------|----------------------------------------|
| `items[]`         | Array of ROM objects in list response  |
| `total`           | Total count (for pagination math)      |
| `id`              | ROM's integer ID                       |
| `name`            | Display name                           |
| `fs_name`         | Exact filename (use for download URL)  |
| `fs_size_bytes`   | File size (for progress bar / SD check)|
| `platform_slug`   | Platform short identifier              |

### File Size Check

Always check `fs_size_bytes` before downloading. Maximum NDS ROM size is 512 MB (rare), most are 8–64 MB.

### URL Encoding

`fs_name` values may contain spaces and parentheses. Percent-encode them in URLs:
- Space → `%20`
- `(` → `%28`
- `)` → `%29`

Example:
```
Mario Kart DS (USA).nds  →  Mario%20Kart%20DS%20%28USA%29.nds
```

### Pagination Strategy

For a DS UI with limited RAM, use small pages:
```
limit=10&offset=0    → first page
limit=10&offset=10   → second page
...
```
Use the `total` field from the response to know how many pages exist.

### Download Resumption

RomM supports standard HTTP range requests via nginx. To resume an interrupted download:
```
GET /api/roms/{id}/files/content/{name}
Range: bytes=<start>-
```

---

## 10. Response Schemas (Minimal)

### SimpleRomSchema (used in list responses)

```json
{
  "id": 101,
  "name": "string",
  "slug": "string",
  "fs_name": "string",
  "fs_name_no_ext": "string",
  "fs_extension": "string",
  "fs_path": "string",
  "fs_size_bytes": 0,
  "platform_id": 0,
  "platform_slug": "string",
  "multi": false,
  "missing_from_fs": false,
  "regions": ["USA"],
  "languages": ["en"],
  "tags": [],
  "revision": null,
  "path_cover_s": "string or null",
  "path_cover_l": "string or null"
}
```

### PlatformSchema

```json
{
  "id": 0,
  "slug": "string",
  "fs_slug": "string",
  "name": "string",
  "custom_name": null,
  "rom_count": 0,
  "fs_size_bytes": 0,
  "category": "string",
  "generation": 0
}
```

### CollectionSchema

```json
{
  "id": 0,
  "name": "string",
  "description": "string or null",
  "is_public": true,
  "is_favorite": false,
  "rom_count": 0,
  "rom_ids": [1, 2, 3]
}
```

### Pagination Envelope (List Responses)

```json
{
  "items": [...],
  "total": 142,
  "limit": 20,
  "offset": 0
}
```

---

## Quick Reference — Endpoints for DS Client

| Action                        | Method | Path                                          |
|-------------------------------|--------|-----------------------------------------------|
| Health check                  | GET    | `/api/heartbeat`                              |
| Get OAuth token               | POST   | `/api/token`                                  |
| List platforms                | GET    | `/api/platforms`                              |
| List ROMs (filtered)          | GET    | `/api/roms?platform_ids=X&limit=N&offset=N`   |
| Search ROMs                   | GET    | `/api/roms?search_term=X&platform_ids=X`      |
| Get ROM details               | GET    | `/api/roms/{id}`                              |
| Get ROM file metadata         | GET    | `/api/roms/{id}/files`                        |
| **Download ROM file**         | GET    | `/api/roms/{id}/files/content/{file_name}`    |
| List collections              | GET    | `/api/collections`                            |
| Get collection + ROM IDs      | GET    | `/api/collections/{id}`                       |
| List ROMs in collection       | GET    | `/api/roms?collection_id=X`                   |
| Bulk download as ZIP          | GET    | `/api/roms/download?rom_ids=1,2,3`            |

---

*Generated from [rommapp/romm](https://github.com/rommapp/romm) source — `docs/BACKEND_ARCHITECTURE.md` and backend endpoint code, May 2026.*