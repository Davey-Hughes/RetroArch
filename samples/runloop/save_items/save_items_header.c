/* Built as C89 with -pedantic -Werror: names every member and value of
 * the save interface once, so the header stays C89 and its numbers
 * stay put. */
#include <stddef.h>

#include <libretro.h>

#define HEADER_CHECK(name, cond) typedef char header_##name[(cond) ? 1 : -1]

HEADER_CHECK(get_number, (RETRO_ENVIRONMENT_GET_SAVE_FRONTEND_INTERFACE
      & ~RETRO_ENVIRONMENT_EXPERIMENTAL) == 104);
HEADER_CHECK(set_number, (RETRO_ENVIRONMENT_SET_SAVE_INTERFACE
      & ~RETRO_ENVIRONMENT_EXPERIMENTAL) == 105);
HEADER_CHECK(get_experimental, (RETRO_ENVIRONMENT_GET_SAVE_FRONTEND_INTERFACE
      & RETRO_ENVIRONMENT_EXPERIMENTAL) != 0);
HEADER_CHECK(set_experimental, (RETRO_ENVIRONMENT_SET_SAVE_INTERFACE
      & RETRO_ENVIRONMENT_EXPERIMENTAL) != 0);
HEADER_CHECK(enum_size, sizeof(enum retro_save_storage) == sizeof(int));

HEADER_CHECK(storage_core_file, RETRO_SAVE_STORAGE_CORE_FILE == 0);
HEADER_CHECK(storage_core_folder, RETRO_SAVE_STORAGE_CORE_FOLDER == 1);
HEADER_CHECK(storage_buffer, RETRO_SAVE_STORAGE_FRONTEND_BUFFER == 2);
HEADER_CHECK(storage_blob, RETRO_SAVE_STORAGE_FRONTEND_BLOB == 3);
HEADER_CHECK(storage_in_content, RETRO_SAVE_STORAGE_IN_CONTENT == 4);
HEADER_CHECK(storage_external, RETRO_SAVE_STORAGE_EXTERNAL == 5);
HEADER_CHECK(storage_not_persisted, RETRO_SAVE_STORAGE_NOT_PERSISTED == 6);
HEADER_CHECK(storage_dummy, RETRO_SAVE_STORAGE_DUMMY == INT_MAX);
HEADER_CHECK(storage_bit, RETRO_SAVE_STORAGE_BIT(
      RETRO_SAVE_STORAGE_FRONTEND_BUFFER) == 4);

HEADER_CHECK(scope_unknown, RETRO_SAVE_SCOPE_UNKNOWN == 0);
HEADER_CHECK(scope_content, RETRO_SAVE_SCOPE_CONTENT == 1);
HEADER_CHECK(scope_game, RETRO_SAVE_SCOPE_GAME == 2);
HEADER_CHECK(scope_shared, RETRO_SAVE_SCOPE_SHARED == 3);
HEADER_CHECK(scope_accessory, RETRO_SAVE_SCOPE_ACCESSORY == 4);
HEADER_CHECK(scope_console, RETRO_SAVE_SCOPE_CONSOLE == 5);
HEADER_CHECK(scope_other_content, RETRO_SAVE_SCOPE_OTHER_CONTENT == 6);
HEADER_CHECK(scope_dummy, RETRO_SAVE_SCOPE_DUMMY == INT_MAX);

HEADER_CHECK(root_frontend, RETRO_SAVE_ROOT_FRONTEND == 0);
HEADER_CHECK(root_save_dir, RETRO_SAVE_ROOT_SAVE_DIR == 1);
HEADER_CHECK(root_system_dir, RETRO_SAVE_ROOT_SYSTEM_DIR == 2);
HEADER_CHECK(root_content_dir, RETRO_SAVE_ROOT_CONTENT_DIR == 3);
HEADER_CHECK(root_content_file, RETRO_SAVE_ROOT_CONTENT_FILE == 4);
HEADER_CHECK(root_absolute, RETRO_SAVE_ROOT_ABSOLUTE == 5);
HEADER_CHECK(root_dummy, RETRO_SAVE_ROOT_DUMMY == INT_MAX);

HEADER_CHECK(role_unknown, RETRO_SAVE_ROLE_UNKNOWN == 0);
HEADER_CHECK(role_progress, RETRO_SAVE_ROLE_PROGRESS == 1);
HEADER_CHECK(role_settings, RETRO_SAVE_ROLE_SETTINGS == 2);
HEADER_CHECK(role_mixed, RETRO_SAVE_ROLE_MIXED == 3);
HEADER_CHECK(role_hiscore, RETRO_SAVE_ROLE_HISCORE == 4);
HEADER_CHECK(role_clock, RETRO_SAVE_ROLE_CLOCK == 5);
HEADER_CHECK(role_user_drive, RETRO_SAVE_ROLE_USER_DRIVE == 6);
HEADER_CHECK(role_cache, RETRO_SAVE_ROLE_CACHE == 7);
HEADER_CHECK(role_dummy, RETRO_SAVE_ROLE_DUMMY == INT_MAX);

HEADER_CHECK(state_unknown, RETRO_SAVE_STATE_UNKNOWN == 0);
HEADER_CHECK(state_not_in_state, RETRO_SAVE_STATE_NOT_IN_STATE == 1);
HEADER_CHECK(state_overwrites, RETRO_SAVE_STATE_LOAD_OVERWRITES == 2);
HEADER_CHECK(state_writes_file, RETRO_SAVE_STATE_LOAD_WRITES_FILE == 3);
HEADER_CHECK(state_stored_ignored, RETRO_SAVE_STATE_STORED_IGNORED == 4);
HEADER_CHECK(state_dummy, RETRO_SAVE_STATE_DUMMY == INT_MAX);

HEADER_CHECK(write_unknown, RETRO_SAVE_WRITE_UNKNOWN == 0);
HEADER_CHECK(write_through, RETRO_SAVE_WRITE_THROUGH == 1);
HEADER_CHECK(write_debounced, RETRO_SAVE_WRITE_DEBOUNCED == 2);
HEADER_CHECK(write_periodic, RETRO_SAVE_WRITE_PERIODIC == 3);
HEADER_CHECK(write_at_unload, RETRO_SAVE_WRITE_AT_UNLOAD == 4);
HEADER_CHECK(write_frontend, RETRO_SAVE_WRITE_FRONTEND == 5);
HEADER_CHECK(write_dummy, RETRO_SAVE_WRITE_DUMMY == INT_MAX);

HEADER_CHECK(flag_contains_content, RETRO_SAVE_FLAG_CONTAINS_CONTENT == 1);
HEADER_CHECK(flag_diff_of_content, RETRO_SAVE_FLAG_DIFF_OF_CONTENT == 2);
HEADER_CHECK(flag_reload_reset, RETRO_SAVE_FLAG_RELOAD_NEEDS_RESET == 4);
HEADER_CHECK(flag_core_rewrites, RETRO_SAVE_FLAG_CORE_REWRITES == 8);
HEADER_CHECK(flag_live_elsewhere, RETRO_SAVE_FLAG_LIVE_ELSEWHERE == 16);
HEADER_CHECK(flag_host_clock, RETRO_SAVE_FLAG_HOST_CLOCK == 32);
HEADER_CHECK(flag_sensitive, RETRO_SAVE_FLAG_SENSITIVE == 64);
HEADER_CHECK(flag_may_transform,
      RETRO_SAVE_FLAG_FRONTEND_MAY_TRANSFORM == 128);
HEADER_CHECK(flag_hardware_shared, RETRO_SAVE_FLAG_HARDWARE_SHARED == 256);
HEADER_CHECK(flag_exact_size, RETRO_SAVE_FLAG_EXACT_SIZE == 512);

HEADER_CHECK(no_legacy, RETRO_SAVE_NO_LEGACY_MEMORY == 0xffffffffu);
HEADER_CHECK(legacy_none, RETRO_SAVE_LEGACY_NONE == 0xffffffffu);

HEADER_CHECK(op_flush, RETRO_SAVE_OP_FLUSH == 0);
HEADER_CHECK(op_reload, RETRO_SAVE_OP_RELOAD == 1);
HEADER_CHECK(op_dummy, RETRO_SAVE_OP_DUMMY == INT_MAX);

HEADER_CHECK(result_done, RETRO_SAVE_RESULT_DONE == 0);
HEADER_CHECK(result_pending, RETRO_SAVE_RESULT_PENDING == 1);
HEADER_CHECK(result_failed, RETRO_SAVE_RESULT_FAILED == 2);
HEADER_CHECK(result_refused, RETRO_SAVE_RESULT_REFUSED == 3);
HEADER_CHECK(result_no_space, RETRO_SAVE_RESULT_NO_SPACE == 4);
HEADER_CHECK(result_name_exists, RETRO_SAVE_RESULT_NAME_EXISTS == 5);
HEADER_CHECK(result_protected, RETRO_SAVE_RESULT_PROTECTED == 6);
HEADER_CHECK(result_bad_format, RETRO_SAVE_RESULT_BAD_FORMAT == 7);
HEADER_CHECK(result_unsupported, RETRO_SAVE_RESULT_UNSUPPORTED == 8);
HEADER_CHECK(result_dummy, RETRO_SAVE_RESULT_DUMMY == INT_MAX);

HEADER_CHECK(event_contents, RETRO_SAVE_EVENT_CONTENTS_CHANGED == 0);
HEADER_CHECK(event_entries, RETRO_SAVE_EVENT_ENTRIES_CHANGED == 1);
HEADER_CHECK(event_dummy, RETRO_SAVE_EVENT_DUMMY == INT_MAX);

HEADER_CHECK(instance_primary, RETRO_SAVE_INSTANCE_PRIMARY == 0);
HEADER_CHECK(instance_secondary, RETRO_SAVE_INSTANCE_SECONDARY == 1);
HEADER_CHECK(instance_dummy, RETRO_SAVE_INSTANCE_DUMMY == INT_MAX);

HEADER_CHECK(issue_refused, RETRO_SAVE_VERDICT_REFUSED == 0);
HEADER_CHECK(issue_ignored, RETRO_SAVE_VERDICT_IGNORED == 2);
HEADER_CHECK(issue_verdict_dummy, RETRO_SAVE_VERDICT_DUMMY == INT_MAX);
HEADER_CHECK(field_entries, RETRO_SAVE_FIELD_ENTRIES == 15);
HEADER_CHECK(field_flags, RETRO_SAVE_FIELD_FLAGS == 16);
HEADER_CHECK(field_num_segments, RETRO_SAVE_FIELD_NUM_SEGMENTS == 41);
HEADER_CHECK(field_dummy, RETRO_SAVE_FIELD_DUMMY == INT_MAX);
HEADER_CHECK(issue_no_item, RETRO_SAVE_ISSUE_NO_ITEM == 0xffffffffu);

HEADER_CHECK(feature_requests, RETRO_SAVE_FEATURE_REQUESTS == 1);
HEADER_CHECK(feature_events, RETRO_SAVE_FEATURE_EVENTS == 2);
HEADER_CHECK(feature_entries, RETRO_SAVE_FEATURE_ENTRIES == 4);
HEADER_CHECK(feature_foreign, RETRO_SAVE_FEATURE_FOREIGN == 8);
HEADER_CHECK(feature_legacy_load, RETRO_SAVE_FEATURE_LEGACY_LOAD == 16);

HEADER_CHECK(kind_unknown, RETRO_SAVE_ENTRY_KIND_UNKNOWN == 0);
HEADER_CHECK(kind_manual, RETRO_SAVE_ENTRY_KIND_MANUAL == 1);
HEADER_CHECK(kind_auto, RETRO_SAVE_ENTRY_KIND_AUTO == 2);
HEADER_CHECK(kind_quick, RETRO_SAVE_ENTRY_KIND_QUICK == 3);
HEADER_CHECK(kind_system, RETRO_SAVE_ENTRY_KIND_SYSTEM == 4);
HEADER_CHECK(kind_dummy, RETRO_SAVE_ENTRY_KIND_DUMMY == INT_MAX);

HEADER_CHECK(verdict_unknown, RETRO_SAVE_ENTRY_VERDICT_UNKNOWN == 0);
HEADER_CHECK(verdict_loadable, RETRO_SAVE_ENTRY_VERDICT_LOADABLE == 1);
HEADER_CHECK(verdict_needs_files, RETRO_SAVE_ENTRY_VERDICT_NEEDS_FILES == 2);
HEADER_CHECK(verdict_too_old, RETRO_SAVE_ENTRY_VERDICT_TOO_OLD == 3);
HEADER_CHECK(verdict_other_game, RETRO_SAVE_ENTRY_VERDICT_OTHER_GAME == 4);
HEADER_CHECK(verdict_dummy, RETRO_SAVE_ENTRY_VERDICT_DUMMY == INT_MAX);

HEADER_CHECK(entry_current, RETRO_SAVE_ENTRY_CURRENT_CONTENT == 1);
HEADER_CHECK(entry_protected, RETRO_SAVE_ENTRY_PROTECTED == 2);
HEADER_CHECK(entry_mounted, RETRO_SAVE_ENTRY_MOUNTED == 4);

static const unsigned header_values[] = {
   RETRO_SAVE_STORAGE_CORE_FILE, RETRO_SAVE_STORAGE_CORE_FOLDER,
   RETRO_SAVE_STORAGE_FRONTEND_BUFFER, RETRO_SAVE_STORAGE_FRONTEND_BLOB,
   RETRO_SAVE_STORAGE_IN_CONTENT, RETRO_SAVE_STORAGE_EXTERNAL,
   RETRO_SAVE_STORAGE_NOT_PERSISTED,
   RETRO_SAVE_SCOPE_UNKNOWN, RETRO_SAVE_SCOPE_CONTENT,
   RETRO_SAVE_SCOPE_GAME, RETRO_SAVE_SCOPE_SHARED,
   RETRO_SAVE_SCOPE_ACCESSORY, RETRO_SAVE_SCOPE_CONSOLE,
   RETRO_SAVE_SCOPE_OTHER_CONTENT,
   RETRO_SAVE_ROOT_FRONTEND, RETRO_SAVE_ROOT_SAVE_DIR,
   RETRO_SAVE_ROOT_SYSTEM_DIR, RETRO_SAVE_ROOT_CONTENT_DIR,
   RETRO_SAVE_ROOT_CONTENT_FILE, RETRO_SAVE_ROOT_ABSOLUTE,
   RETRO_SAVE_ROLE_UNKNOWN, RETRO_SAVE_ROLE_PROGRESS,
   RETRO_SAVE_ROLE_SETTINGS, RETRO_SAVE_ROLE_MIXED,
   RETRO_SAVE_ROLE_HISCORE, RETRO_SAVE_ROLE_CLOCK,
   RETRO_SAVE_ROLE_USER_DRIVE, RETRO_SAVE_ROLE_CACHE,
   RETRO_SAVE_STATE_UNKNOWN, RETRO_SAVE_STATE_NOT_IN_STATE,
   RETRO_SAVE_STATE_LOAD_OVERWRITES, RETRO_SAVE_STATE_LOAD_WRITES_FILE,
   RETRO_SAVE_STATE_STORED_IGNORED,
   RETRO_SAVE_WRITE_UNKNOWN, RETRO_SAVE_WRITE_THROUGH,
   RETRO_SAVE_WRITE_DEBOUNCED, RETRO_SAVE_WRITE_PERIODIC,
   RETRO_SAVE_WRITE_AT_UNLOAD, RETRO_SAVE_WRITE_FRONTEND,
   RETRO_SAVE_OP_FLUSH, RETRO_SAVE_OP_RELOAD,
   RETRO_SAVE_RESULT_DONE, RETRO_SAVE_RESULT_PENDING,
   RETRO_SAVE_RESULT_FAILED, RETRO_SAVE_RESULT_REFUSED,
   RETRO_SAVE_RESULT_NO_SPACE, RETRO_SAVE_RESULT_NAME_EXISTS,
   RETRO_SAVE_RESULT_PROTECTED, RETRO_SAVE_RESULT_BAD_FORMAT,
   RETRO_SAVE_RESULT_UNSUPPORTED,
   RETRO_SAVE_EVENT_CONTENTS_CHANGED, RETRO_SAVE_EVENT_ENTRIES_CHANGED,
   RETRO_SAVE_INSTANCE_PRIMARY, RETRO_SAVE_INSTANCE_SECONDARY,
   RETRO_SAVE_VERDICT_REFUSED, RETRO_SAVE_VERDICT_LEFT_OUT,
   RETRO_SAVE_VERDICT_IGNORED,
   RETRO_SAVE_FIELD_NONE, RETRO_SAVE_FIELD_INTERFACE_VERSION,
   RETRO_SAVE_FIELD_NUM_ITEMS, RETRO_SAVE_FIELD_ITEM_SIZE,
   RETRO_SAVE_FIELD_ITEMS, RETRO_SAVE_FIELD_REQUEST,
   RETRO_SAVE_FIELD_GET_RESULT, RETRO_SAVE_FIELD_GET_GENERATION,
   RETRO_SAVE_FIELD_CAN_SNAPSHOT, RETRO_SAVE_FIELD_LOADED,
   RETRO_SAVE_FIELD_LEGACY_LOAD, RETRO_SAVE_FIELD_REDIRECT,
   RETRO_SAVE_FIELD_BLOB_SIZE, RETRO_SAVE_FIELD_BLOB_SAVE,
   RETRO_SAVE_FIELD_BLOB_LOAD, RETRO_SAVE_FIELD_ENTRIES,
   RETRO_SAVE_FIELD_FLAGS, RETRO_SAVE_FIELD_ID, RETRO_SAVE_FIELD_LABEL,
   RETRO_SAVE_FIELD_GROUP, RETRO_SAVE_FIELD_SCOPE_KEY,
   RETRO_SAVE_FIELD_GAME_KEY, RETRO_SAVE_FIELD_PATH,
   RETRO_SAVE_FIELD_LEGACY_PATHS, RETRO_SAVE_FIELD_INCLUDE,
   RETRO_SAVE_FIELD_EXCLUDE, RETRO_SAVE_FIELD_FORMAT,
   RETRO_SAVE_FIELD_ACCEPTS, RETRO_SAVE_FIELD_BASE_HASH,
   RETRO_SAVE_FIELD_SEGMENTS, RETRO_SAVE_FIELD_MAX_SIZE,
   RETRO_SAVE_FIELD_STORAGE, RETRO_SAVE_FIELD_SCOPE,
   RETRO_SAVE_FIELD_CONTENT_INDEX, RETRO_SAVE_FIELD_PORT,
   RETRO_SAVE_FIELD_ROOT, RETRO_SAVE_FIELD_LEGACY_MEMORY,
   RETRO_SAVE_FIELD_FORMAT_VERSION, RETRO_SAVE_FIELD_ROLE,
   RETRO_SAVE_FIELD_STATE_RELATION, RETRO_SAVE_FIELD_WRITE_TIMING,
   RETRO_SAVE_FIELD_NUM_SEGMENTS,
   RETRO_SAVE_ENTRY_KIND_UNKNOWN, RETRO_SAVE_ENTRY_KIND_MANUAL,
   RETRO_SAVE_ENTRY_KIND_AUTO, RETRO_SAVE_ENTRY_KIND_QUICK,
   RETRO_SAVE_ENTRY_KIND_SYSTEM,
   RETRO_SAVE_ENTRY_VERDICT_UNKNOWN, RETRO_SAVE_ENTRY_VERDICT_LOADABLE,
   RETRO_SAVE_ENTRY_VERDICT_NEEDS_FILES, RETRO_SAVE_ENTRY_VERDICT_TOO_OLD,
   RETRO_SAVE_ENTRY_VERDICT_OTHER_GAME
};

static const char *const header_legacy[]  = { "old", "older", NULL };
static const char *const header_include[] = { "slot1", NULL };
static const char *const header_exclude[] = { "cache", NULL };
static const char *const header_accepts[] = { "nes:wram", NULL };

static size_t RETRO_CALLCONV header_get_path(unsigned item, char *buf,
      size_t len)
{
   (void)item;
   (void)buf;
   (void)len;
   return 0;
}

static void RETRO_CALLCONV header_notify(unsigned event, unsigned item)
{
   (void)event;
   (void)item;
}

static unsigned RETRO_CALLCONV header_request(unsigned item, unsigned op)
{
   (void)item;
   (void)op;
   return RETRO_SAVE_RESULT_UNSUPPORTED;
}

static uint64_t RETRO_CALLCONV header_generation(unsigned item)
{
   (void)item;
   return 0;
}

static bool RETRO_CALLCONV header_can_snapshot(unsigned item)
{
   (void)item;
   return true;
}

static void RETRO_CALLCONV header_loaded(unsigned item)
{
   (void)item;
}

static bool RETRO_CALLCONV header_legacy_load(unsigned item, unsigned index,
      const char *format, const void *buf, size_t len)
{
   (void)item;
   (void)index;
   (void)format;
   (void)buf;
   (void)len;
   return false;
}

static unsigned RETRO_CALLCONV header_redirect(unsigned item,
      const char *path)
{
   (void)item;
   (void)path;
   return RETRO_SAVE_RESULT_REFUSED;
}

static size_t RETRO_CALLCONV header_blob_size(unsigned item)
{
   (void)item;
   return 0;
}

static bool RETRO_CALLCONV header_blob_save(unsigned item, void *buf,
      size_t len)
{
   (void)item;
   (void)buf;
   (void)len;
   return false;
}

static bool RETRO_CALLCONV header_blob_load(unsigned item,
      const void *buf, size_t len)
{
   (void)item;
   (void)buf;
   (void)len;
   return false;
}

static unsigned RETRO_CALLCONV header_list(unsigned item,
      const struct retro_save_entry **entries, unsigned *count)
{
   (void)item;
   *entries = NULL;
   *count   = 0;
   return RETRO_SAVE_RESULT_DONE;
}

static bool RETRO_CALLCONV header_get_icon(unsigned item,
      const char *handle, struct retro_save_icon *icon)
{
   (void)item;
   (void)handle;
   (void)icon;
   return false;
}

static void *RETRO_CALLCONV header_export_begin(unsigned item,
      const char *handle, const char *format, unsigned *result)
{
   (void)item;
   (void)handle;
   (void)format;
   *result = RETRO_SAVE_RESULT_UNSUPPORTED;
   return NULL;
}

static bool RETRO_CALLCONV header_export_file(void *op, unsigned file,
      char *name, size_t name_len)
{
   (void)op;
   (void)file;
   (void)name;
   (void)name_len;
   return false;
}

static unsigned RETRO_CALLCONV header_export_step(void *op, void *buf,
      size_t max_bytes, size_t *written)
{
   (void)op;
   (void)buf;
   (void)max_bytes;
   *written = 0;
   return RETRO_SAVE_RESULT_DONE;
}

static void *RETRO_CALLCONV header_import_begin(unsigned item,
      const char *format, const char *name_hint, unsigned *result)
{
   (void)item;
   (void)format;
   (void)name_hint;
   *result = RETRO_SAVE_RESULT_UNSUPPORTED;
   return NULL;
}

static unsigned RETRO_CALLCONV header_import_file(void *op,
      unsigned file, const char *name)
{
   (void)op;
   (void)file;
   (void)name;
   return RETRO_SAVE_RESULT_UNSUPPORTED;
}

static unsigned RETRO_CALLCONV header_import_step(void *op,
      const void *buf, size_t len)
{
   (void)op;
   (void)buf;
   (void)len;
   return RETRO_SAVE_RESULT_UNSUPPORTED;
}

static unsigned RETRO_CALLCONV header_transfer_end(void *op, bool commit)
{
   (void)op;
   (void)commit;
   return RETRO_SAVE_RESULT_DONE;
}

static unsigned RETRO_CALLCONV header_remove(unsigned item,
      const char *handle)
{
   (void)item;
   (void)handle;
   return RETRO_SAVE_RESULT_UNSUPPORTED;
}

static struct retro_save_entry_interface header_entries;

/* Sums every value above; the HEADER_CHECKs pin each one. */
unsigned header_sum_values(void)
{
   unsigned i;
   unsigned sum = 0;
   for (i = 0; i < sizeof(header_values) / sizeof(header_values[0]); i++)
      sum += header_values[i];
   return sum;
}

void header_fill_frontend(struct retro_save_frontend_interface *fi)
{
   fi->interface_version = 1;
   fi->instance          = RETRO_SAVE_INSTANCE_PRIMARY;
   fi->storage_kinds     = ~(uint64_t)0;
   fi->item_flags        = ~(uint64_t)0;
   fi->features          = ~(uint64_t)0;
   fi->get_path          = NULL;
   fi->notify            = NULL;
   fi->get_issues        = NULL;
}

void header_fill_item(struct retro_save_item *item,
      const struct retro_save_segment *segments, unsigned num_segments)
{
   item->flags          = RETRO_SAVE_FLAG_CONTAINS_CONTENT
                        | RETRO_SAVE_FLAG_DIFF_OF_CONTENT
                        | RETRO_SAVE_FLAG_RELOAD_NEEDS_RESET
                        | RETRO_SAVE_FLAG_CORE_REWRITES
                        | RETRO_SAVE_FLAG_LIVE_ELSEWHERE
                        | RETRO_SAVE_FLAG_HOST_CLOCK
                        | RETRO_SAVE_FLAG_SENSITIVE
                        | RETRO_SAVE_FLAG_FRONTEND_MAY_TRANSFORM
                        | RETRO_SAVE_FLAG_HARDWARE_SHARED;
   item->id             = "wram";
   item->label          = "Battery RAM";
   item->group          = "cart";
   item->scope_key      = "us";
   item->game_key       = "serial:SLUS-00594";
   item->path           = "srm";
   item->legacy_paths   = header_legacy;
   item->include        = header_include;
   item->exclude        = header_exclude;
   item->format         = "nes:wram+n163";
   item->accepts        = header_accepts;
   item->base_hash      = "sha1:00";
   item->segments       = segments;
   item->max_size       = 0;
   item->storage        = RETRO_SAVE_STORAGE_FRONTEND_BUFFER;
   item->scope          = RETRO_SAVE_SCOPE_CONTENT;
   item->content_index  = 0;
   item->port           = 0;
   item->root           = RETRO_SAVE_ROOT_FRONTEND;
   item->legacy_memory  = RETRO_MEMORY_SAVE_RAM;
   item->format_version = 0;
   item->role           = RETRO_SAVE_ROLE_PROGRESS;
   item->state_relation = RETRO_SAVE_STATE_LOAD_OVERWRITES;
   item->write_timing   = RETRO_SAVE_WRITE_FRONTEND;
   item->num_segments   = num_segments;
}

void header_fill_interface(struct retro_save_interface *iface,
      const struct retro_save_item *items, unsigned num_items)
{
   header_entries.interface_version = 1;
   header_entries.entry_size        = sizeof(struct retro_save_entry);
   header_entries.list              = header_list;
   header_entries.get_icon          = header_get_icon;
   header_entries.export_begin      = header_export_begin;
   header_entries.export_file       = header_export_file;
   header_entries.export_step       = header_export_step;
   header_entries.import_begin      = header_import_begin;
   header_entries.import_file       = header_import_file;
   header_entries.import_step       = header_import_step;
   header_entries.transfer_end      = header_transfer_end;
   header_entries.remove            = header_remove;

   iface->interface_version = 1;
   iface->num_items         = num_items;
   iface->item_size         = sizeof(struct retro_save_item);
   iface->items             = items;
   iface->request           = header_request;
   iface->get_result        = header_request;
   iface->get_generation    = header_generation;
   iface->can_snapshot      = header_can_snapshot;
   iface->loaded            = header_loaded;
   iface->legacy_load       = NULL;
   iface->redirect          = header_redirect;
   iface->blob_size         = header_blob_size;
   iface->blob_save         = header_blob_save;
   iface->blob_load         = header_blob_load;
   iface->entries           = &header_entries;
}

void header_fill_entry(struct retro_save_entry *entry,
      struct retro_save_icon *icon)
{
   entry->size      = 8192;
   entry->flags     = RETRO_SAVE_ENTRY_CURRENT_CONTENT
                    | RETRO_SAVE_ENTRY_PROTECTED
                    | RETRO_SAVE_ENTRY_MOUNTED;
   entry->mtime     = -1;
   entry->handle    = "0";
   entry->title     = "Slot 1";
   entry->detail    = NULL;
   entry->game_id   = "serial:SLUS-00594";
   entry->version   = NULL;
   entry->kind      = RETRO_SAVE_ENTRY_KIND_MANUAL;
   entry->verdict   = RETRO_SAVE_ENTRY_VERDICT_LOADABLE;
   entry->num_files = 1;

   icon->pixels     = NULL;
   icon->width      = 32;
   icon->height     = 32;
   icon->frames     = 1;
   icon->frame_ms   = 0;
}

/* Never called: stores what the fill helpers leave out or leave NULL. */
void header_store_rest(struct retro_save_frontend_interface *fi,
      struct retro_save_segment *segment, struct retro_save_interface *iface)
{
   fi->get_path       = header_get_path;
   fi->notify         = header_notify;
   segment->data      = NULL;
   segment->size      = 0;
   iface->legacy_load = header_legacy_load;
}
