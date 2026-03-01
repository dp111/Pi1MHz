/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2025 Ennebi Elettronica (https://ennebielettronica.com)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include "bsp/board_api.h"
#include "tusb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "../BeebSCSI/filesystem.h"
#include "../BeebSCSI/fatfs/ff.h"
#include "../rpi/asm-helpers.h"
#include "../rpi/systimer.h"

_Noreturn void reboot_now(void);
extern void _copyandreboot(void *src, int num_bytes);

//--------------------------------------------------------------------+
// Dataset
//--------------------------------------------------------------------+

//------------- device info -------------//
#define DEV_INFO_MANUFACTURER   "TinyUSB"
#define DEV_INFO_MODEL          "Pi1MHz MTP"
#define DEV_INFO_VERSION        "1.0"
#define DEV_PROP_FRIENDLY_NAME  "Pi1MHz MTP"

//------------- storage info -------------//
#define STORAGE_DESCRIPTION { 'F', 'i', 'l', 'e', 's', 'y', 's', 't', 'e', 'm', 0 }
#define VOLUME_IDENTIFIER { 'P', 'i', '1', 'M', 'H', 'z', 0 }

enum {
  STORAGE_DESC_LEN = TU_ARRAY_SIZE((uint16_t[]) STORAGE_DESCRIPTION),
  VOLUME_ID_LEN = TU_ARRAY_SIZE((uint16_t[])VOLUME_IDENTIFIER)
};

typedef MTP_STORAGE_INFO_STRUCT(STORAGE_DESC_LEN, VOLUME_ID_LEN) storage_info_t;

storage_info_t storage_info = {
  #ifdef CFG_EXAMPLE_MTP_READONLY
  .storage_type = MTP_STORAGE_TYPE_FIXED_ROM,
  #else
  .storage_type = MTP_STORAGE_TYPE_FIXED_RAM,
  #endif

  .filesystem_type = MTP_FILESYSTEM_TYPE_GENERIC_HIERARCHICAL,
  .access_capability = MTP_ACCESS_CAPABILITY_READ_WRITE,
  .max_capacity_in_bytes = 0, // calculated at runtime
  .free_space_in_bytes = 0, // calculated at runtime
  .free_space_in_objects = 0, // calculated at runtime
  .storage_description = {
    .count = (TU_FIELD_SIZE(storage_info_t, storage_description)-1) / sizeof(uint16_t),
    .utf16 = STORAGE_DESCRIPTION
  },
  .volume_identifier = {
    .count = (TU_FIELD_SIZE(storage_info_t, volume_identifier)-1) / sizeof(uint16_t),
    .utf16 = VOLUME_IDENTIFIER
  }
};

//--------------------------------------------------------------------+
// MTP FILESYSTEM
//--------------------------------------------------------------------+
#define FS_INFO_BUFFER_SIZE 256u
#define FS_INFO_NAME_OFFSET 127u
#define FS_NAME_MAX_LEN 127u
#define FS_SCAN_MAX_ENTRIES 4096u
#define FS_PATH_MAX 512u
#define FS_FIXED_DATETIME "20250808T173500.0" // "YYYYMMDDTHHMMSS.s"
#define FS_KERNEL_NOW_FALLBACK_CAPACITY (512u * 1024u)

typedef struct {
  uint32_t handle;
  uint32_t parent;
  bool is_dir;
  uint32_t size;
  char name[FS_NAME_MAX_LEN + 1];
  char path[FS_PATH_MAX];
} fs_entry_t;

typedef struct {
  uint32_t handle;
  uint32_t parent;
  bool is_dir;
  uint32_t size;
  char* path;
} fs_cache_entry_t;

typedef struct {
  bool valid;
  fs_cache_entry_t* entries;
  uint32_t count;
  uint32_t capacity;
} fs_cache_t;

typedef bool (*fs_walk_cb_t)(const fs_entry_t* entry, void* user_data);

typedef struct {
  bool active;
  bool file_open;
  uint32_t handle;
  FIL file;
  uint32_t transferred;
  uint32_t size;
  uint8_t* data;
} read_state_t;

typedef struct {
  bool active;
  bool is_dir;
  bool is_kernel_now;
  bool file_open;
  bool size_known;
  uint16_t object_format;
  uint16_t protection_status;
  uint32_t parent;
  uint16_t association_type;
  char name[FS_NAME_MAX_LEN + 1];
  char path[FS_PATH_MAX];
  uint8_t* kernel_data;
  uint32_t kernel_capacity;
  FIL file;
  uint32_t transferred;
  uint32_t size;
} write_state_t;

enum {
  SUPPORTED_STORAGE_ID = 0x00010001u // physical = 1, logical = 1
};

static int32_t fs_get_device_info(tud_mtp_cb_data_t* cb_data);
static int32_t fs_open_close_session(tud_mtp_cb_data_t* cb_data);
static int32_t fs_get_storage_ids(tud_mtp_cb_data_t* cb_data);
static int32_t fs_get_storage_info(tud_mtp_cb_data_t* cb_data);
static int32_t fs_get_device_properties(tud_mtp_cb_data_t* cb_data);
static int32_t fs_get_object_handles(tud_mtp_cb_data_t* cb_data);
static int32_t fs_get_object_info(tud_mtp_cb_data_t* cb_data);
static int32_t fs_get_object(tud_mtp_cb_data_t* cb_data);
static int32_t fs_get_partial_object(tud_mtp_cb_data_t* cb_data);
static int32_t fs_delete_object(tud_mtp_cb_data_t* cb_data);
static int32_t fs_send_object_info(tud_mtp_cb_data_t* cb_data);
static int32_t fs_send_object(tud_mtp_cb_data_t* cb_data);
static int32_t fs_move_object(tud_mtp_cb_data_t* cb_data);
static int32_t fs_set_object_prop_value(tud_mtp_cb_data_t* cb_data);
static int32_t fs_get_object_props_supported(tud_mtp_cb_data_t* cb_data);
static int32_t fs_get_object_prop_value(tud_mtp_cb_data_t* cb_data);
static uint16_t fs_guess_object_format(const char* name, uint8_t status);

typedef int32_t (*fs_op_handler_t)(tud_mtp_cb_data_t* cb_data);
typedef struct {
  uint32_t op_code;
  fs_op_handler_t handler;
}fs_op_handler_dict_t;

fs_op_handler_dict_t fs_op_handler_dict[] = {
  { MTP_OP_GET_DEVICE_INFO,       fs_get_device_info    },
  { MTP_OP_OPEN_SESSION,          fs_open_close_session },
  { MTP_OP_CLOSE_SESSION,         fs_open_close_session },
  { MTP_OP_GET_STORAGE_IDS,       fs_get_storage_ids       },
  { MTP_OP_GET_STORAGE_INFO,      fs_get_storage_info      },
  { MTP_OP_GET_DEVICE_PROP_DESC,  fs_get_device_properties  },
  { MTP_OP_GET_DEVICE_PROP_VALUE, fs_get_device_properties },
  { MTP_OP_GET_OBJECT_HANDLES,    fs_get_object_handles    },
  { MTP_OP_GET_OBJECT_INFO,       fs_get_object_info       },
  { MTP_OP_GET_OBJECT,            fs_get_object            },
  { MTP_OP_GET_PARTIAL_OBJECT,    fs_get_partial_object    },
  { MTP_OP_DELETE_OBJECT,         fs_delete_object         },
  { MTP_OP_SEND_OBJECT_INFO,      fs_send_object_info      },
  { MTP_OP_SEND_OBJECT,           fs_send_object           },
  { MTP_OP_MOVE_OBJECT,           fs_move_object           },
  { MTP_OP_GET_OBJECT_PROPS_SUPPORTED, fs_get_object_props_supported },
  { MTP_OP_GET_OBJECT_PROP_VALUE, fs_get_object_prop_value },
  { MTP_OP_SET_OBJECT_PROP_VALUE, fs_set_object_prop_value },
};

static bool is_session_opened = false;
static uint32_t send_obj_handle = 0;
static uint32_t send_obj_parent = 0;
static read_state_t g_read_state;
static write_state_t g_write_state;
static fs_cache_t g_fs_cache;

//--------------------------------------------------------------------+
//
//--------------------------------------------------------------------+
static bool fs_ensure_ready(void) {
  return filesystemMount();
}

static void fs_ascii_to_utf16(const char* ascii, uint16_t utf16[], size_t max_chars) {
  size_t i = 0;
  if ((ascii == NULL) || (utf16 == NULL) || (max_chars == 0)) {
    return;
  }

  for (; (i + 1u) < max_chars && ascii[i] != '\0'; i++) {
    utf16[i] = (uint8_t) ascii[i];
  }
  utf16[i] = 0;
}

static void fs_utf16_to_ascii(const uint16_t utf16[], char ascii[], size_t max_chars) {
  size_t i = 0;
  if ((utf16 == NULL) || (ascii == NULL) || (max_chars == 0)) {
    return;
  }

  for (; (i + 1u) < max_chars && utf16[i] != 0; i++) {
    ascii[i] = (char) (utf16[i] & 0x00FFu);
  }
  ascii[i] = '\0';
}

static bool fs_make_path(const char* base, const char* name, char path[], size_t max_len) {
  if ((base == NULL) || (name == NULL) || (path == NULL) || (max_len == 0)) {
    return false;
  }

  int n;
  if (strcmp(base, "/") == 0) {
    n = snprintf(path, max_len, "/%s", name);
  } else {
    n = snprintf(path, max_len, "%s/%s", base, name);
  }

  if (n < 0 || (size_t)n >= max_len) {
    return false;
  }
  return true;
}

static uint32_t fs_handle_from_path(const char* path) {
  uint32_t hash = 2166136261u;
  const uint8_t* p = (const uint8_t*) path;

  while (*p != 0u) {
    hash ^= *p++;
    hash *= 16777619u;
  }

  if (hash == 0u || hash == 0xFFFFFFFFu) {
    hash ^= 0xA5A5A5A5u;
  }

  return hash;
}

static bool fs_walk_tree_recursive(const char* dir_path, uint32_t parent_handle,
                                   fs_walk_cb_t cb, void* user_data) {
  DIR dir;
  FILINFO fno;

  if (f_opendir(&dir, dir_path) != FR_OK) {
    return false;
  }

  while (true) {
    if (f_readdir(&dir, &fno) != FR_OK) {
      (void) f_closedir(&dir);
      return false;
    }

    if (fno.fname[0] == '\0') {
      break;
    }

    if ((strcmp(fno.fname, ".") == 0) || (strcmp(fno.fname, "..") == 0)) {
      continue;
    }

    fs_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.is_dir = (fno.fattrib & AM_DIR) != 0;
    entry.size = (uint32_t)fno.fsize;
    strlcpy(entry.name, fno.fname, sizeof(entry.name));
    if (!fs_make_path(dir_path, fno.fname, entry.path, sizeof(entry.path))) {
      continue;
    }
    entry.handle = fs_handle_from_path(entry.path);
    entry.parent = parent_handle;

    if (!cb(&entry, user_data)) {
      (void) f_closedir(&dir);
      return true;
    }

    if (entry.is_dir) {
      if (!fs_walk_tree_recursive(entry.path, entry.handle, cb, user_data)) {
        (void) f_closedir(&dir);
        return false;
      }
    }
  }

  (void) f_closedir(&dir);
  return true;
}

static bool fs_walk_tree(fs_walk_cb_t cb, void* user_data) {
  if (!fs_ensure_ready()) {
    return false;
  }
  return fs_walk_tree_recursive("/", 0, cb, user_data);
}

static const char* fs_basename(const char* path) {
  const char* p = strrchr(path, '/');
  return (p != NULL) ? (p + 1) : path;
}

static void fs_cache_clear(void) {
  if (g_fs_cache.entries != NULL) {
    for (uint32_t i = 0; i < g_fs_cache.count; i++) {
      free(g_fs_cache.entries[i].path);
    }
    free(g_fs_cache.entries);
  }

  memset(&g_fs_cache, 0, sizeof(g_fs_cache));
}

static void fs_cache_invalidate(void) {
  fs_cache_clear();
}

static bool fs_cache_add_entry(const fs_entry_t* entry) {
  if (g_fs_cache.count >= g_fs_cache.capacity) {
    uint32_t new_capacity = (g_fs_cache.capacity == 0u) ? 64u : (g_fs_cache.capacity * 2u);
    fs_cache_entry_t* new_entries = (fs_cache_entry_t*) realloc(g_fs_cache.entries, new_capacity * sizeof(fs_cache_entry_t));
    if (new_entries == NULL) {
      return false;
    }
    g_fs_cache.entries = new_entries;
    g_fs_cache.capacity = new_capacity;
  }

  fs_cache_entry_t* cache_entry = &g_fs_cache.entries[g_fs_cache.count];
  memset(cache_entry, 0, sizeof(*cache_entry));
  cache_entry->handle = entry->handle;
  cache_entry->parent = entry->parent;
  cache_entry->is_dir = entry->is_dir;
  cache_entry->size = entry->size;
  cache_entry->path = strdup(entry->path);
  if (cache_entry->path == NULL) {
    return false;
  }

  g_fs_cache.count++;
  return true;
}

typedef struct {
  bool ok;
} fs_cache_build_ctx_t;

static bool fs_cache_build_cb(const fs_entry_t* entry, void* user_data) {
  fs_cache_build_ctx_t* ctx = (fs_cache_build_ctx_t*) user_data;
  if (!fs_cache_add_entry(entry)) {
    ctx->ok = false;
    return false;
  }
  return true;
}

static bool fs_cache_ensure(void) {
  if (g_fs_cache.valid) {
    return true;
  }

  fs_cache_clear();

  fs_cache_build_ctx_t ctx = {
    .ok = true
  };

  if (!fs_walk_tree(fs_cache_build_cb, &ctx) || !ctx.ok) {
    fs_cache_clear();
    return false;
  }

  g_fs_cache.valid = true;
  return true;
}

static uint32_t fs_find_handle_by_parent_name(uint32_t parent, const char* name) {
  if ((name == NULL) || (name[0] == '\0')) {
    return 0;
  }

  if (!fs_cache_ensure()) {
    return 0;
  }

  for (uint32_t i = 0; i < g_fs_cache.count; i++) {
    fs_cache_entry_t* entry = &g_fs_cache.entries[i];
    if (entry->parent == parent && strcmp(fs_basename(entry->path), name) == 0) {
      return entry->handle;
    }
  }

  return 0;
}

static bool fs_get_entry_by_handle(uint32_t handle, fs_entry_t* out_entry) {
  if (handle == 0 || out_entry == NULL) {
    return false;
  }

  if (!fs_cache_ensure()) {
    return false;
  }

  for (uint32_t i = 0; i < g_fs_cache.count; i++) {
    const fs_cache_entry_t* cache_entry = &g_fs_cache.entries[i];
    if (cache_entry->handle == handle) {
      memset(out_entry, 0, sizeof(*out_entry));
      out_entry->handle = cache_entry->handle;
      out_entry->parent = cache_entry->parent;
      out_entry->is_dir = cache_entry->is_dir;
      out_entry->size = cache_entry->size;
      strlcpy(out_entry->path, cache_entry->path, sizeof(out_entry->path));
      strlcpy(out_entry->name, fs_basename(cache_entry->path), sizeof(out_entry->name));
      return true;
    }
  }

  return false;
}

static bool fs_append_handle(uint32_t** arr, uint32_t* count, uint32_t* cap, uint32_t handle) {
  if (*count >= *cap) {
    uint32_t new_cap = (*cap == 0) ? 32u : (*cap * 2u);
    uint32_t* new_arr = (uint32_t*) realloc(*arr, new_cap * sizeof(uint32_t));
    if (new_arr == NULL) {
      return false;
    }
    *arr = new_arr;
    *cap = new_cap;
  }

  (*arr)[*count] = handle;
  (*count)++;
  return true;
}

static bool fs_collect_handles(uint32_t parent_filter, uint32_t obj_format, uint32_t** out_arr, uint32_t* out_count) {
  uint32_t* arr = NULL;
  uint32_t count = 0;
  uint32_t cap = 0;

  if (!fs_cache_ensure()) {
    return false;
  }

  for (uint32_t i = 0; i < g_fs_cache.count; i++) {
    const fs_cache_entry_t* entry = &g_fs_cache.entries[i];

    bool parent_match;
    if (parent_filter == 0xFFFFFFFFu) {
      parent_match = (entry->parent == 0u);
    } else {
      parent_match = (entry->parent == parent_filter);
    }

    if (!parent_match) {
      continue;
    }

    uint16_t format = fs_guess_object_format(fs_basename(entry->path), entry->is_dir ? 2u : 1u);
    if (obj_format != 0u && obj_format != format) {
      continue;
    }

    if (!fs_append_handle(&arr, &count, &cap, entry->handle)) {
      free(arr);
      return false;
    }
  }

  *out_arr = arr;
  *out_count = count;
  return true;
}

static bool fs_get_parent_path(uint32_t parent_handle, char path[FS_PATH_MAX]) {
  if (parent_handle == 0u || parent_handle == 0xFFFFFFFFu) {
    strlcpy(path, "/", FS_PATH_MAX);
    return true;
  }

  fs_entry_t parent;
  if (!fs_get_entry_by_handle(parent_handle, &parent) || !parent.is_dir) {
    return false;
  }

  strlcpy(path, parent.path, FS_PATH_MAX);
  return true;
}

static uint16_t fs_guess_object_format(const char* name, uint8_t status) {
  if (status == 2u) {
    return MTP_OBJ_FORMAT_ASSOCIATION;
  }

  const char* ext = strrchr(name, '.');
  if (ext == NULL) {
    return MTP_OBJ_FORMAT_UNDEFINED;
  }
  ext++;

  char lower[8] = {0};
  size_t i;
  for (i = 0; i < sizeof(lower) - 1 && ext[i] != '\0'; i++) {
    lower[i] = (char) tolower((unsigned char) ext[i]);
  }
  lower[i] = '\0';

  if (strcmp(lower, "txt") == 0) return MTP_OBJ_FORMAT_TEXT;
  if (strcmp(lower, "png") == 0) return MTP_OBJ_FORMAT_PNG;
  if (strcmp(lower, "jpg") == 0 || strcmp(lower, "jpeg") == 0) return MTP_OBJ_FORMAT_EXIF_JPEG;
  if (strcmp(lower, "gif") == 0) return MTP_OBJ_FORMAT_GIF;
  if (strcmp(lower, "bmp") == 0) return MTP_OBJ_FORMAT_BMP;
  if (strcmp(lower, "mp3") == 0) return MTP_OBJ_FORMAT_MP3;
  if (strcmp(lower, "wav") == 0) return MTP_OBJ_FORMAT_WAV;
  if (strcmp(lower, "mp4") == 0) return MTP_OBJ_FORMAT_MP4;

  return MTP_OBJ_FORMAT_UNDEFINED;
}

static void fs_release_read_state(void) {
  if (g_read_state.file_open) {
    (void) f_close(&g_read_state.file);
  }
  if (g_read_state.data != NULL) {
    free(g_read_state.data);
  }
  memset(&g_read_state, 0, sizeof(g_read_state));
}

static void fs_release_write_state(void) {
  if (g_write_state.file_open) {
    (void) f_close(&g_write_state.file);
  }
  if (g_write_state.kernel_data != NULL) {
    free(g_write_state.kernel_data);
  }
  memset(&g_write_state, 0, sizeof(g_write_state));
}

static bool fs_kernel_alloc(uint32_t capacity) {
  uint8_t* new_data = (uint8_t*) malloc(capacity);
  if (new_data == NULL) {
    return false;
  }

  if (g_write_state.kernel_data != NULL) {
    free(g_write_state.kernel_data);
  }

  g_write_state.kernel_data = new_data;
  g_write_state.kernel_capacity = capacity;
  return true;
}

//--------------------------------------------------------------------+
// Control Request callback
//--------------------------------------------------------------------+
bool tud_mtp_request_cancel_cb(tud_mtp_request_cb_data_t* cb_data) {
  mtp_request_reset_cancel_data_t cancel_data;
  memcpy(&cancel_data, cb_data->buf, sizeof(cancel_data));
  (void) cancel_data.code;
  (void ) cancel_data.transaction_id;
  return true;
}

// Invoked when received Device Reset request
// return false to stall the request
bool tud_mtp_request_device_reset_cb(tud_mtp_request_cb_data_t* cb_data) {
  (void) cb_data;
  return true;
}

// Invoked when received Get Extended Event request. Application fill callback data's buffer for response
// return negative to stall the request
int32_t tud_mtp_request_get_extended_event_cb(tud_mtp_request_cb_data_t* cb_data) {
  (void) cb_data;
  return false; // not implemented yet
}

// Invoked when received Get DeviceStatus request. Application fill callback data's buffer for response
// return negative to stall the request
int32_t tud_mtp_request_get_device_status_cb(tud_mtp_request_cb_data_t* cb_data) {
  uint16_t* buf16 = (uint16_t*)(uintptr_t) cb_data->buf;
  buf16[0] = 4; // length
  buf16[1] = MTP_RESP_OK; // status
  return 4;
}

//--------------------------------------------------------------------+
// Bulk Only Protocol
//--------------------------------------------------------------------+
int32_t tud_mtp_command_received_cb(tud_mtp_cb_data_t* cb_data) {
  const mtp_container_command_t* command = cb_data->command_container;
  mtp_container_info_t* io_container = &cb_data->io_container;
  fs_op_handler_t handler = NULL;
  for (size_t i = 0; i < TU_ARRAY_SIZE(fs_op_handler_dict); i++) {
    if (fs_op_handler_dict[i].op_code == command->header.code) {
      handler = fs_op_handler_dict[i].handler;
      break;
    }
  }

  int32_t resp_code;
  if (handler == NULL) {
    resp_code = MTP_RESP_OPERATION_NOT_SUPPORTED;
  } else {
    resp_code = handler(cb_data);
    if (resp_code > MTP_RESP_UNDEFINED) {
      // send response if needed
      io_container->header->code = (uint16_t)resp_code;
      tud_mtp_response_send(io_container);
    }
  }

  return resp_code;
}

int32_t tud_mtp_data_xfer_cb(tud_mtp_cb_data_t* cb_data) {
  const mtp_container_command_t* command = cb_data->command_container;
  mtp_container_info_t* io_container = &cb_data->io_container;
  fs_op_handler_t handler = NULL;
  for (size_t i = 0; i < TU_ARRAY_SIZE(fs_op_handler_dict); i++) {
    if (fs_op_handler_dict[i].op_code == command->header.code) {
      handler = fs_op_handler_dict[i].handler;
      break;
    }
  }

  int32_t resp_code;
  if (handler == NULL) {
    resp_code = MTP_RESP_OPERATION_NOT_SUPPORTED;
  } else {
    resp_code = handler(cb_data);
    if (resp_code > MTP_RESP_UNDEFINED) {
      // send response if needed
      io_container->header->code = (uint16_t)resp_code;
      tud_mtp_response_send(io_container);
    }
  }

  return 0;
}

int32_t tud_mtp_data_complete_cb(tud_mtp_cb_data_t* cb_data) {
  const mtp_container_command_t* command = cb_data->command_container;
  mtp_container_info_t* resp = &cb_data->io_container;
  switch (command->header.code) {
    case MTP_OP_SEND_OBJECT_INFO: {
      // parameter is: storage id, parent handle, new handle
      (void) mtp_container_add_uint32(resp, SUPPORTED_STORAGE_ID);
      (void) mtp_container_add_uint32(resp, send_obj_parent);
      (void) mtp_container_add_uint32(resp, send_obj_handle);
      resp->header->code = MTP_RESP_OK;
      break;
    }

    case MTP_OP_SEND_OBJECT: {
      if (g_write_state.is_kernel_now) {
        uint32_t reboot_copy_len = (g_write_state.transferred + 63u) & ~63u;

        if (g_write_state.size_known && (g_write_state.transferred != g_write_state.size)) {
          resp->header->code = MTP_RESP_GENERAL_ERROR;
          fs_release_write_state();
          break;
        }

        if (g_write_state.kernel_data == NULL || g_write_state.transferred == 0u) {
          resp->header->code = MTP_RESP_GENERAL_ERROR;
          fs_release_write_state();
          break;
        }

        if (reboot_copy_len > g_write_state.kernel_capacity) {
          resp->header->code = MTP_RESP_STORE_FULL;
          fs_release_write_state();
          break;
        }

        if (reboot_copy_len > g_write_state.transferred) {
          memset(g_write_state.kernel_data + g_write_state.transferred, 0, reboot_copy_len - g_write_state.transferred);
        }
        _disable_interrupts();
        _copyandreboot(g_write_state.kernel_data, (int)reboot_copy_len);

        resp->header->code = MTP_RESP_GENERAL_ERROR;
        fs_release_write_state();
        break;
      }

      if (g_write_state.file_open) {
        (void) f_sync(&g_write_state.file);
        (void) f_close(&g_write_state.file);
        g_write_state.file_open = false;
      }

      if (!g_write_state.size_known || (g_write_state.transferred == g_write_state.size)) {
        FILINFO fno;
        if (f_stat(g_write_state.path, &fno) != FR_OK) {
          resp->header->code = MTP_RESP_GENERAL_ERROR;
          fs_release_write_state();
          break;
        }
        uint32_t handle = fs_find_handle_by_parent_name(send_obj_parent, g_write_state.name);
        send_obj_handle = (handle != 0u) ? handle : send_obj_handle;
        fs_cache_invalidate();
        resp->header->code = MTP_RESP_OK;
      } else {
        resp->header->code = MTP_RESP_GENERAL_ERROR;
      }
      fs_release_write_state();
      break;
    }

    case MTP_OP_GET_OBJECT:
      fs_release_read_state();
      resp->header->code = (cb_data->xfer_result == XFER_RESULT_SUCCESS) ? MTP_RESP_OK : MTP_RESP_GENERAL_ERROR;
      break;

    case MTP_OP_GET_PARTIAL_OBJECT:
      (void) mtp_container_add_uint32(resp, g_read_state.transferred);
      fs_release_read_state();
      resp->header->code = (cb_data->xfer_result == XFER_RESULT_SUCCESS) ? MTP_RESP_OK : MTP_RESP_GENERAL_ERROR;
      break;

    default:
      resp->header->code = (cb_data->xfer_result == XFER_RESULT_SUCCESS) ? MTP_RESP_OK : MTP_RESP_GENERAL_ERROR;
      break;
  }

  tud_mtp_response_send(resp);
  return 0;
}

int32_t tud_mtp_response_complete_cb(tud_mtp_cb_data_t* cb_data) {
  (void) cb_data;
  return 0; // nothing to do
}

//--------------------------------------------------------------------+
// File System Handlers
//--------------------------------------------------------------------+
static int32_t fs_get_device_info(tud_mtp_cb_data_t* cb_data) {
  // Device info is already prepared up to playback formats. Application only need to add string fields
  int32_t resp_code = 0;
  mtp_container_info_t* io_container = &cb_data->io_container;
  (void) mtp_container_add_cstring(io_container, DEV_INFO_MANUFACTURER);
  (void) mtp_container_add_cstring(io_container, DEV_INFO_MODEL);
  (void) mtp_container_add_cstring(io_container, DEV_INFO_VERSION);

  enum { MAX_SERIAL_NCHARS = 32 };
  uint16_t serial_utf16[MAX_SERIAL_NCHARS+1];
  size_t nchars = board_usb_get_serial(serial_utf16, MAX_SERIAL_NCHARS);
  serial_utf16[tu_min32(nchars, MAX_SERIAL_NCHARS)] = 0; // ensure null termination
  (void) mtp_container_add_string(io_container, serial_utf16);

  if (!tud_mtp_data_send(io_container)) {
    resp_code = MTP_RESP_DEVICE_BUSY;
  }
  return resp_code;
}

static int32_t fs_open_close_session(tud_mtp_cb_data_t* cb_data) {
  const mtp_container_command_t* command = cb_data->command_container;
  if (command->header.code == MTP_OP_OPEN_SESSION) {
    if (is_session_opened) {
      return MTP_RESP_SESSION_ALREADY_OPEN;
    }
    is_session_opened = true;
    fs_cache_invalidate();
    (void) fs_cache_ensure();
  } else { // close session
    if (!is_session_opened) {
      return MTP_RESP_SESSION_NOT_OPEN;
    }
    is_session_opened = false;
    fs_release_read_state();
    fs_release_write_state();
    fs_cache_clear();
  }
  return MTP_RESP_OK;
}

static int32_t fs_get_storage_ids(tud_mtp_cb_data_t* cb_data) {
  mtp_container_info_t* io_container = &cb_data->io_container;
  uint32_t storage_ids [] = { SUPPORTED_STORAGE_ID };
  (void) mtp_container_add_auint32(io_container, 1, storage_ids);
  tud_mtp_data_send(io_container);
  return 0;
}

static int32_t fs_get_storage_info(tud_mtp_cb_data_t* cb_data) {
  const mtp_container_command_t* command = cb_data->command_container;
  mtp_container_info_t* io_container = &cb_data->io_container;
  const uint32_t storage_id = command->params[0];
  TU_VERIFY(SUPPORTED_STORAGE_ID == storage_id, MTP_RESP_INVALID_STORAGE_ID);

  if (!fs_ensure_ready()) {
    return MTP_RESP_STORE_NOT_AVAILABLE;
  }

  // update storage info with current free space
  storage_info.max_capacity_in_bytes = UINT64_MAX;
  storage_info.free_space_in_bytes = UINT64_MAX;
  storage_info.free_space_in_objects = UINT32_MAX;
  (void) mtp_container_add_raw(io_container, &storage_info, sizeof(storage_info));
  tud_mtp_data_send(io_container);
  return 0;
}

static int32_t fs_get_device_properties(tud_mtp_cb_data_t* cb_data) {
  const mtp_container_command_t* command = cb_data->command_container;
  mtp_container_info_t* io_container = &cb_data->io_container;
  const uint16_t dev_prop_code = (uint16_t) command->params[0];

  if (command->header.code == MTP_OP_GET_DEVICE_PROP_DESC) {
    // get describing dataset
    mtp_device_prop_desc_header_t device_prop_header;
    device_prop_header.device_property_code = dev_prop_code;
    switch (dev_prop_code) {
      case MTP_DEV_PROP_DEVICE_FRIENDLY_NAME:
        device_prop_header.datatype = MTP_DATA_TYPE_STR;
        device_prop_header.get_set = MTP_MODE_GET;
        (void) mtp_container_add_raw(io_container, &device_prop_header, sizeof(device_prop_header));
        (void) mtp_container_add_cstring(io_container, DEV_PROP_FRIENDLY_NAME); // factory
        (void) mtp_container_add_cstring(io_container, DEV_PROP_FRIENDLY_NAME); // current
        (void) mtp_container_add_uint8(io_container, 0); // no form
        tud_mtp_data_send(io_container);
        break;

      default:
        return MTP_RESP_PARAMETER_NOT_SUPPORTED;
    }
  } else {
    // get value
    switch (dev_prop_code) {
      case MTP_DEV_PROP_DEVICE_FRIENDLY_NAME:
        (void) mtp_container_add_cstring(io_container, DEV_PROP_FRIENDLY_NAME);
        tud_mtp_data_send(io_container);
        break;

      default:
        return MTP_RESP_PARAMETER_NOT_SUPPORTED;
    }
  }
  return 0;
}

static int32_t fs_get_object_handles(tud_mtp_cb_data_t* cb_data) {
  const mtp_container_command_t* command = cb_data->command_container;
  mtp_container_info_t* io_container = &cb_data->io_container;

  const uint32_t storage_id = command->params[0];
  const uint32_t obj_format = command->params[1]; // optional
  const uint32_t parent_handle = command->params[2]; // folder handle, 0xFFFFFFFF is root
  if (storage_id != 0xFFFFFFFFu && storage_id != SUPPORTED_STORAGE_ID) {
    return MTP_RESP_INVALID_STORAGE_ID;
  }
  if (!fs_ensure_ready()) {
    return MTP_RESP_STORE_NOT_AVAILABLE;
  }

  if (parent_handle != 0u && parent_handle != 0xFFFFFFFFu) {
    fs_entry_t parent;
    if (!fs_get_entry_by_handle(parent_handle, &parent)) {
      return MTP_RESP_INVALID_PARENT_OBJECT;
    }
    if (!parent.is_dir) {
      return MTP_RESP_INVALID_PARENT_OBJECT;
    }
  }

  uint32_t* handles = NULL;
  uint32_t out_count = 0;
  if (!fs_collect_handles(parent_handle, obj_format, &handles, &out_count)) {
    return MTP_RESP_DEVICE_BUSY;
  }

  (void) mtp_container_add_auint32(io_container, out_count, handles);
  tud_mtp_data_send(io_container);
  free(handles);

  return 0;
}

static int32_t fs_get_object_info(tud_mtp_cb_data_t* cb_data) {
  const mtp_container_command_t* command = cb_data->command_container;
  mtp_container_info_t* io_container = &cb_data->io_container;
  const uint32_t obj_handle = command->params[0];

  fs_entry_t entry;
  if (!fs_get_entry_by_handle(obj_handle, &entry)) {
    return MTP_RESP_INVALID_OBJECT_HANDLE;
  }

  const uint16_t object_format = fs_guess_object_format(entry.name, entry.is_dir ? 2u : 1u);
  const uint32_t object_size = entry.is_dir ? 0u : entry.size;

  mtp_object_info_header_t obj_info_header = {
    .storage_id = SUPPORTED_STORAGE_ID,
    .object_format = object_format,
    .protection_status = MTP_PROTECTION_STATUS_NO_PROTECTION,
    .object_compressed_size = object_size,
    .thumb_format = MTP_OBJ_FORMAT_UNDEFINED,
    .thumb_compressed_size = 0,
    .thumb_pix_width = 0,
    .thumb_pix_height = 0,
    .image_pix_width = 0,
    .image_pix_height = 0,
    .image_bit_depth = 0,
    .parent_object = entry.parent,
    .association_type = entry.is_dir ? MTP_ASSOCIATION_GENERIC_FOLDER : MTP_ASSOCIATION_UNDEFINED,
    .association_desc = 0,
    .sequence_number = 0
  };
  uint16_t name_utf16[FS_NAME_MAX_LEN + 1];
  fs_ascii_to_utf16(entry.name, name_utf16, TU_ARRAY_SIZE(name_utf16));

  (void) mtp_container_add_raw(io_container, &obj_info_header, sizeof(obj_info_header));
  (void) mtp_container_add_string(io_container, name_utf16);
  (void) mtp_container_add_cstring(io_container, FS_FIXED_DATETIME);
  (void) mtp_container_add_cstring(io_container, FS_FIXED_DATETIME);
  (void) mtp_container_add_cstring(io_container, ""); // keywords, not used
  tud_mtp_data_send(io_container);

  return 0;
}

static int32_t fs_get_object(tud_mtp_cb_data_t* cb_data) {
  const mtp_container_command_t* command = cb_data->command_container;
  mtp_container_info_t* io_container = &cb_data->io_container;
  const uint32_t obj_handle = command->params[0];

  if (cb_data->phase == MTP_PHASE_COMMAND) {
    fs_entry_t entry;
    if (!fs_get_entry_by_handle(obj_handle, &entry)) {
      return MTP_RESP_INVALID_OBJECT_HANDLE;
    }
    if (entry.is_dir) {
      return MTP_RESP_INVALID_OBJECT_FORMAT_CODE;
    }

    fs_release_read_state();

    if (f_open(&g_read_state.file, entry.path, FA_READ) != FR_OK) {
      return MTP_RESP_GENERAL_ERROR;
    }
    g_read_state.file_open = true;

    const uint32_t actual_size = (uint32_t) f_size(&g_read_state.file);

    g_read_state.active = true;
    g_read_state.handle = obj_handle;
    g_read_state.transferred = 0;
    g_read_state.size = actual_size;

    io_container->header->len = sizeof(mtp_container_header_t) + actual_size;

    uint32_t xact_len = tu_min32(actual_size, io_container->payload_bytes);
    if (xact_len > 0u) {
      UINT bytes_read = 0;
      if (f_read(&g_read_state.file, io_container->payload, xact_len, &bytes_read) != FR_OK) {
        fs_release_read_state();
        return MTP_RESP_GENERAL_ERROR;
      }
      if (bytes_read != xact_len) {
        fs_release_read_state();
        return MTP_RESP_GENERAL_ERROR;
      }
      g_read_state.transferred = bytes_read;
    }

    tud_mtp_data_send(io_container);
  } else if (cb_data->phase == MTP_PHASE_DATA) {
    if (!g_read_state.active || g_read_state.handle != obj_handle) {
      return MTP_RESP_GENERAL_ERROR;
    }

    const uint32_t remain = (g_read_state.transferred < g_read_state.size) ? (g_read_state.size - g_read_state.transferred) : 0u;
    const uint32_t xact_len = tu_min32(remain, io_container->payload_bytes);
    if (xact_len > 0) {
      UINT bytes_read = 0;
      if (f_read(&g_read_state.file, io_container->payload, xact_len, &bytes_read) != FR_OK) {
        fs_release_read_state();
        return MTP_RESP_GENERAL_ERROR;
      }
      if (bytes_read != xact_len) {
        fs_release_read_state();
        return MTP_RESP_GENERAL_ERROR;
      }
      g_read_state.transferred += bytes_read;
      tud_mtp_data_send(io_container);
    }
  } else {
    // nothing to do
  }

  return 0;
}

static int32_t fs_get_partial_object(tud_mtp_cb_data_t* cb_data) {
  const mtp_container_command_t* command = cb_data->command_container;
  mtp_container_info_t* io_container = &cb_data->io_container;
  const uint32_t obj_handle = command->params[0];
  const uint32_t offset = command->params[1];
  const uint32_t max_bytes = command->params[2];

  if (cb_data->phase == MTP_PHASE_COMMAND) {
    fs_entry_t entry;
    if (!fs_get_entry_by_handle(obj_handle, &entry)) {
      return MTP_RESP_INVALID_OBJECT_HANDLE;
    }
    if (entry.is_dir) {
      return MTP_RESP_INVALID_OBJECT_FORMAT_CODE;
    }

    fs_release_read_state();

    if (f_open(&g_read_state.file, entry.path, FA_READ) != FR_OK) {
      return MTP_RESP_GENERAL_ERROR;
    }
    g_read_state.file_open = true;

    const uint32_t actual_size = (uint32_t) f_size(&g_read_state.file);

    const uint32_t start = tu_min32(offset, actual_size);
    const uint32_t remain = actual_size - start;
    const uint32_t req_size = (max_bytes == 0u) ? remain : tu_min32(max_bytes, remain);

    if (f_lseek(&g_read_state.file, start) != FR_OK) {
      fs_release_read_state();
      return MTP_RESP_GENERAL_ERROR;
    }

    g_read_state.active = true;
    g_read_state.handle = obj_handle;
    g_read_state.transferred = 0;
    g_read_state.size = req_size;

    io_container->header->len = sizeof(mtp_container_header_t) + req_size;

    uint32_t xact_len = tu_min32(req_size, io_container->payload_bytes);
    if (xact_len > 0u) {
      UINT bytes_read = 0;
      if (f_read(&g_read_state.file, io_container->payload, xact_len, &bytes_read) != FR_OK) {
        fs_release_read_state();
        return MTP_RESP_GENERAL_ERROR;
      }
      if (bytes_read != xact_len) {
        fs_release_read_state();
        return MTP_RESP_GENERAL_ERROR;
      }
      g_read_state.transferred = bytes_read;
    }

    tud_mtp_data_send(io_container);
  } else if (cb_data->phase == MTP_PHASE_DATA) {
    if (!g_read_state.active || g_read_state.handle != obj_handle) {
      return MTP_RESP_GENERAL_ERROR;
    }

    const uint32_t remain = (g_read_state.transferred < g_read_state.size) ? (g_read_state.size - g_read_state.transferred) : 0u;
    const uint32_t xact_len = tu_min32(remain, io_container->payload_bytes);
    if (xact_len > 0u) {
      UINT bytes_read = 0;
      if (f_read(&g_read_state.file, io_container->payload, xact_len, &bytes_read) != FR_OK) {
        fs_release_read_state();
        return MTP_RESP_GENERAL_ERROR;
      }
      if (bytes_read != xact_len) {
        fs_release_read_state();
        return MTP_RESP_GENERAL_ERROR;
      }
      g_read_state.transferred += bytes_read;
      tud_mtp_data_send(io_container);
    }
  }

  return 0;
}

static int32_t fs_send_object_info(tud_mtp_cb_data_t* cb_data) {
  const mtp_container_command_t* command = cb_data->command_container;
  mtp_container_info_t* io_container = &cb_data->io_container;
  const uint32_t storage_id = command->params[0];
  const uint32_t parent_handle = command->params[1]; // folder handle, 0xFFFFFFFF is root
  if (!is_session_opened) {
    return MTP_RESP_SESSION_NOT_OPEN;
  }
  if (storage_id != 0xFFFFFFFFu && storage_id != SUPPORTED_STORAGE_ID) {
    return MTP_RESP_INVALID_STORAGE_ID;
  }
  if (!fs_ensure_ready()) {
    return MTP_RESP_STORE_NOT_AVAILABLE;
  }

  if (cb_data->phase == MTP_PHASE_COMMAND) {
    (void) tud_mtp_data_receive(io_container);
  } else if (cb_data->phase == MTP_PHASE_DATA) {
    fs_release_write_state();

    mtp_object_info_header_t* obj_info = (mtp_object_info_header_t*) io_container->payload;
    if (obj_info->storage_id != 0 && obj_info->storage_id != SUPPORTED_STORAGE_ID) {
      return MTP_RESP_INVALID_STORAGE_ID;
    }

    const uint32_t cmd_parent = parent_handle;
    const uint32_t info_parent = obj_info->parent_object;

    uint32_t effective_parent;
    if (info_parent != 0u && info_parent != 0xFFFFFFFFu) {
      effective_parent = info_parent;
    } else if (cmd_parent != 0xFFFFFFFFu) {
      effective_parent = cmd_parent;
    } else {
      effective_parent = 0xFFFFFFFFu;
    }

    char parent_path[FS_PATH_MAX];
    if (!fs_get_parent_path(effective_parent, parent_path)) {
      return MTP_RESP_INVALID_PARENT_OBJECT;
    }

    uint16_t name_utf16[FS_NAME_MAX_LEN + 1];
    uint8_t* buf = io_container->payload + sizeof(mtp_object_info_header_t);
    (void) mtp_container_get_string(buf, name_utf16);
    fs_utf16_to_ascii(name_utf16, g_write_state.name, sizeof(g_write_state.name));
    if (g_write_state.name[0] == '\0') {
      return MTP_RESP_GENERAL_ERROR;
    }

    if (strcmp(g_write_state.name, "reboot.now") == 0) {
      reboot_now();
    }

    g_write_state.active = true;
    g_write_state.file_open = false;
    g_write_state.transferred = 0;
    g_write_state.object_format = obj_info->object_format;
    g_write_state.protection_status = obj_info->protection_status;
    g_write_state.parent = (effective_parent == 0xFFFFFFFFu) ? 0u : effective_parent;
    g_write_state.association_type = obj_info->association_type;
    g_write_state.is_dir = (obj_info->association_type == MTP_ASSOCIATION_GENERIC_FOLDER || obj_info->object_format == MTP_OBJ_FORMAT_ASSOCIATION);
    g_write_state.size_known = (obj_info->object_compressed_size != 0xFFFFFFFFu) && (obj_info->object_compressed_size != 0u);
    g_write_state.size = g_write_state.size_known ? obj_info->object_compressed_size : 0u;
    g_write_state.is_kernel_now = (strcmp(g_write_state.name, "kernel.now") == 0);

    if (g_write_state.is_kernel_now) {
      if (g_write_state.is_dir) {
        fs_release_write_state();
        return MTP_RESP_INVALID_OBJECT_FORMAT_CODE;
      }

      uint32_t kernel_capacity = g_write_state.size_known ? ((g_write_state.size + 63u) & ~63u) : FS_KERNEL_NOW_FALLBACK_CAPACITY;
      if (!fs_kernel_alloc(kernel_capacity)) {
        fs_release_write_state();
        return MTP_RESP_STORE_FULL;
      }

      send_obj_parent = 0u;
      send_obj_handle = fs_handle_from_path("/kernel.now");
      return 0;
    }

    if (!fs_make_path(parent_path, g_write_state.name, g_write_state.path, sizeof(g_write_state.path))) {
      return MTP_RESP_GENERAL_ERROR;
    }

    if (g_write_state.is_dir) {
      FRESULT mk_res = f_mkdir(g_write_state.path);
      if (mk_res != FR_OK && mk_res != FR_EXIST) {
        fs_release_write_state();
        return MTP_RESP_GENERAL_ERROR;
      }
      send_obj_parent = g_write_state.parent;
      send_obj_handle = fs_handle_from_path(g_write_state.path);
      fs_cache_invalidate();
      fs_release_write_state();
      return 0;
    }

    if (f_open(&g_write_state.file, g_write_state.path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
      fs_release_write_state();
      return MTP_RESP_GENERAL_ERROR;
    }
    g_write_state.file_open = true;
    fs_cache_invalidate();

    send_obj_parent = g_write_state.parent;
    send_obj_handle = fs_handle_from_path(g_write_state.path);
  } else {
    // nothing to do
  }

  return 0;
}

static int32_t fs_send_object(tud_mtp_cb_data_t* cb_data) {
  mtp_container_info_t* io_container = &cb_data->io_container;
  if (!g_write_state.active) {
    return MTP_RESP_GENERAL_ERROR;
  }

  if (g_write_state.is_dir) {
    return 0;
  }

  if (g_write_state.is_kernel_now) {
    if (cb_data->phase == MTP_PHASE_COMMAND) {
      if (g_write_state.size_known) {
        io_container->header->len = sizeof(mtp_container_header_t) + g_write_state.size;
      } else {
        io_container->header->len = UINT32_MAX;
      }
      tud_mtp_data_receive(io_container);
    } else {
      const uint32_t offset = cb_data->total_xferred_bytes - sizeof(mtp_container_header_t) - io_container->payload_bytes;
      const uint32_t xact_len = io_container->payload_bytes;
      uint32_t total_needed = offset + xact_len;

      if (total_needed > g_write_state.kernel_capacity) {
        return MTP_RESP_GENERAL_ERROR;
      }

      if (xact_len > 0u) {
        memcpy(g_write_state.kernel_data + offset, io_container->payload, xact_len);
        if (total_needed > g_write_state.transferred) {
          g_write_state.transferred = total_needed;
        }
      }

      if (g_write_state.size_known) {
        if (cb_data->total_xferred_bytes - sizeof(mtp_container_header_t) < g_write_state.size) {
          tud_mtp_data_receive(io_container);
        }
      } else {
        const uint32_t packet_bytes = (offset == 0u) ? (xact_len + sizeof(mtp_container_header_t)) : xact_len;
        if (packet_bytes == CFG_TUD_MTP_EP_BUFSIZE) {
          tud_mtp_data_receive(io_container);
        }
      }
    }
    return 0;
  }

  if (cb_data->phase == MTP_PHASE_COMMAND) {
    if (!g_write_state.file_open) {
      return MTP_RESP_GENERAL_ERROR;
    }
    if (g_write_state.size_known) {
      io_container->header->len = sizeof(mtp_container_header_t) + g_write_state.size;
    } else {
      io_container->header->len = UINT32_MAX;
    }
    tud_mtp_data_receive(io_container);
  } else {
    if (!g_write_state.file_open) {
      return MTP_RESP_GENERAL_ERROR;
    }
    // file contents offset is total xferred minus header size minus last received chunk
    const uint32_t offset = cb_data->total_xferred_bytes - sizeof(mtp_container_header_t) - io_container->payload_bytes;
    const uint32_t xact_len = io_container->payload_bytes;
    if (g_write_state.size_known && ((offset + xact_len) > g_write_state.size)) {
      return MTP_RESP_GENERAL_ERROR;
    }
    if (xact_len > 0u) {
      UINT bytes_written = 0;
      if (f_write(&g_write_state.file, io_container->payload, xact_len, &bytes_written) != FR_OK) {
        fs_release_write_state();
        return MTP_RESP_GENERAL_ERROR;
      }
      if (bytes_written != xact_len) {
        fs_release_write_state();
        return MTP_RESP_GENERAL_ERROR;
      }
      g_write_state.transferred += bytes_written;
    }
    if (g_write_state.size_known) {
      if (cb_data->total_xferred_bytes - sizeof(mtp_container_header_t) < g_write_state.size) {
        tud_mtp_data_receive(io_container);
      }
    } else {
      const uint32_t packet_bytes = (offset == 0u) ? (xact_len + sizeof(mtp_container_header_t)) : xact_len;
      if (packet_bytes == CFG_TUD_MTP_EP_BUFSIZE) {
        tud_mtp_data_receive(io_container);
      }
    }
  }

  return 0;
}

static int32_t fs_move_object(tud_mtp_cb_data_t* cb_data) {
  const mtp_container_command_t* command = cb_data->command_container;
  const uint32_t obj_handle = command->params[0];
  const uint32_t storage_id = command->params[1];
  const uint32_t parent_handle = command->params[2];

  if (!is_session_opened) {
    return MTP_RESP_SESSION_NOT_OPEN;
  }
  if (storage_id != 0xFFFFFFFFu && storage_id != SUPPORTED_STORAGE_ID) {
    return MTP_RESP_INVALID_STORAGE_ID;
  }

  fs_entry_t entry;
  if (!fs_get_entry_by_handle(obj_handle, &entry)) {
    return MTP_RESP_INVALID_OBJECT_HANDLE;
  }

  char parent_path[FS_PATH_MAX];
  if (!fs_get_parent_path(parent_handle, parent_path)) {
    return MTP_RESP_INVALID_PARENT_OBJECT;
  }

  char dst_path[FS_PATH_MAX];
  if (!fs_make_path(parent_path, entry.name, dst_path, sizeof(dst_path))) {
    return MTP_RESP_GENERAL_ERROR;
  }

  if (strcmp(dst_path, entry.path) == 0) {
    return MTP_RESP_OK;
  }

  FRESULT res = f_rename(entry.path, dst_path);
  if (res == FR_OK) {
    fs_cache_invalidate();
    return MTP_RESP_OK;
  }
  return MTP_RESP_GENERAL_ERROR;
}

static int32_t fs_get_object_props_supported(tud_mtp_cb_data_t* cb_data) {
  mtp_container_info_t* io_container = &cb_data->io_container;
  const uint16_t props[] = {
    MTP_OBJ_PROP_OBJECT_FILE_NAME,
    MTP_OBJ_PROP_PARENT_OBJECT
  };

  (void) mtp_container_add_auint16(io_container, TU_ARRAY_SIZE(props), props);
  tud_mtp_data_send(io_container);
  return 0;
}

static int32_t fs_get_object_prop_value(tud_mtp_cb_data_t* cb_data) {
  const mtp_container_command_t* command = cb_data->command_container;
  mtp_container_info_t* io_container = &cb_data->io_container;
  const uint32_t obj_handle = command->params[0];
  const uint16_t prop_code = (uint16_t) command->params[1];

  fs_entry_t entry;
  if (!fs_get_entry_by_handle(obj_handle, &entry)) {
    return MTP_RESP_INVALID_OBJECT_HANDLE;
  }

  switch (prop_code) {
    case MTP_OBJ_PROP_OBJECT_FILE_NAME: {
      uint16_t name_utf16[FS_NAME_MAX_LEN + 1];
      fs_ascii_to_utf16(entry.name, name_utf16, TU_ARRAY_SIZE(name_utf16));
      (void) mtp_container_add_string(io_container, name_utf16);
      tud_mtp_data_send(io_container);
      return 0;
    }

    case MTP_OBJ_PROP_PARENT_OBJECT:
      (void) mtp_container_add_uint32(io_container, entry.parent);
      tud_mtp_data_send(io_container);
      return 0;

    default:
      return MTP_RESP_OBJECT_PROP_NOT_SUPPORTED;
  }
}

static int32_t fs_set_object_prop_value(tud_mtp_cb_data_t* cb_data) {
  const mtp_container_command_t* command = cb_data->command_container;
  mtp_container_info_t* io_container = &cb_data->io_container;
  const uint32_t obj_handle = command->params[0];
  const uint16_t prop_code = (uint16_t) command->params[1];

  if (!is_session_opened) {
    return MTP_RESP_SESSION_NOT_OPEN;
  }

  if (cb_data->phase == MTP_PHASE_COMMAND) {
    (void) tud_mtp_data_receive(io_container);
    return 0;
  }

  if (cb_data->phase != MTP_PHASE_DATA) {
    return 0;
  }

  fs_entry_t entry;
  if (!fs_get_entry_by_handle(obj_handle, &entry)) {
    return MTP_RESP_INVALID_OBJECT_HANDLE;
  }

  if (prop_code == MTP_OBJ_PROP_OBJECT_FILE_NAME) {
    uint16_t name_utf16[FS_NAME_MAX_LEN + 1];
    char new_name[FS_NAME_MAX_LEN + 1];
    (void) mtp_container_get_string(io_container->payload, name_utf16);
    fs_utf16_to_ascii(name_utf16, new_name, sizeof(new_name));

    if (new_name[0] == '\0') {
      return MTP_RESP_INVALID_OBJECT_PROP_VALUE;
    }

    char parent_path[FS_PATH_MAX];
    if (!fs_get_parent_path(entry.parent, parent_path)) {
      return MTP_RESP_INVALID_PARENT_OBJECT;
    }

    char dst_path[FS_PATH_MAX];
    if (!fs_make_path(parent_path, new_name, dst_path, sizeof(dst_path))) {
      return MTP_RESP_GENERAL_ERROR;
    }

    if (strcmp(dst_path, entry.path) == 0) {
      return MTP_RESP_OK;
    }

    FRESULT res = f_rename(entry.path, dst_path);
    if (res == FR_OK) {
      fs_cache_invalidate();
      return MTP_RESP_OK;
    }
    return MTP_RESP_GENERAL_ERROR;
  }

  if (prop_code == MTP_OBJ_PROP_PARENT_OBJECT) {
    uint32_t new_parent = 0;
    if (io_container->payload_bytes < sizeof(uint32_t)) {
      return MTP_RESP_INVALID_OBJECT_PROP_VALUE;
    }

    memcpy(&new_parent, io_container->payload, sizeof(uint32_t));
    if (new_parent == 0xFFFFFFFFu) {
      new_parent = 0;
    }

    char parent_path[FS_PATH_MAX];
    if (!fs_get_parent_path(new_parent, parent_path)) {
      return MTP_RESP_INVALID_PARENT_OBJECT;
    }

    char dst_path[FS_PATH_MAX];
    if (!fs_make_path(parent_path, entry.name, dst_path, sizeof(dst_path))) {
      return MTP_RESP_GENERAL_ERROR;
    }

    if (strcmp(dst_path, entry.path) == 0) {
      return MTP_RESP_OK;
    }

    FRESULT res = f_rename(entry.path, dst_path);
    if (res == FR_OK) {
      fs_cache_invalidate();
      return MTP_RESP_OK;
    }
    return MTP_RESP_GENERAL_ERROR;
  }

  return MTP_RESP_OBJECT_PROP_NOT_SUPPORTED;
}

static int32_t fs_delete_object(tud_mtp_cb_data_t* cb_data) {
  const mtp_container_command_t* command = cb_data->command_container;
  const uint32_t obj_handle = command->params[0];

  if (!is_session_opened) {
    return MTP_RESP_SESSION_NOT_OPEN;
  }

  fs_entry_t entry;
  if (!fs_get_entry_by_handle(obj_handle, &entry)) {
    return MTP_RESP_INVALID_OBJECT_HANDLE;
  }

  FRESULT res = f_unlink(entry.path);
  if (res == FR_OK) {
    fs_cache_invalidate();
    return MTP_RESP_OK;
  }

  return MTP_RESP_GENERAL_ERROR;
}
