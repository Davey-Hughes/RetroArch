/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2026 - The RetroArch team
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <compat/strl.h>
#include <file/file_path.h>
#include <retro_miscellaneous.h>
#include <string/stdstring.h>

#include "file_path_special.h"
#include "save_items.h"
#include "verbosity.h"

/* Main-thread data: every call arrives on the core's thread. */
static save_items_state_t save_items_st;

/* Lines one registration logs; the rest are counted. */
#define SAVE_ITEMS_MAX_LINES 32
/* Findings and told ids remembered per content load. */
#define SAVE_ITEMS_MAX_SEEN 256

/* By enum retro_save_field. */
static const char *const save_items_field_names[] = {
   "registration", "interface_version", "num_items", "item_size", "items",
   "request", "get_result", "get_generation", "can_snapshot", "loaded",
   "legacy_load", "redirect", "blob_size", "blob_save", "blob_load",
   "entries", "flags", "id", "label", "group", "scope_key", "game_key",
   "path", "legacy_paths", "include", "exclude", "format", "accepts",
   "base_hash", "segments", "max_size", "storage", "scope",
   "content_index", "port", "root", "legacy_memory", "format_version",
   "role", "state_relation", "write_timing", "num_segments"
};

/* One registration's findings. */
typedef struct save_items_check
{
   const char *core;
   struct retro_save_issue *issues;
   unsigned num_issues;
   /* Slots left after the findings. */
   unsigned room;
   unsigned lines;
   unsigned more;
   bool refused;
   /* The list may not move: findings only take the room left. */
   bool fixed;
   /* A finding past the lines logged is not IGNORED. */
   bool more_err;
   /* The item being checked was left out. */
   bool left;
} save_items_check_t;

save_items_state_t *save_items_state_get_ptr(void)
{
   return &save_items_st;
}

static char *save_items_strdup(const char *s, bool *ok)
{
   char *copy;
   if (!s)
      return NULL;
   if (!(copy = strdup(s)))
      *ok = false;
   return copy;
}

static void save_items_strv_free(const char *const *v)
{
   size_t i;
   if (!v)
      return;
   for (i = 0; v[i]; i++)
      free((void*)v[i]);
   free((void*)v);
}

static const char *const *save_items_strv_dup(const char *const *v,
      bool *ok)
{
   size_t i;
   size_t n = 0;
   char **copy;
   if (!v)
      return NULL;
   while (v[n])
      n++;
   if (!(copy = (char**)calloc(n + 1, sizeof(*copy))))
   {
      *ok = false;
      return NULL;
   }
   for (i = 0; i < n; i++)
      if (!(copy[i] = strdup(v[i])))
      {
         *ok = false;
         break;
      }
   return (const char *const *)copy;
}

static const struct retro_save_segment *save_items_segments_dup(
      const struct retro_save_segment *segments, unsigned num, bool *ok)
{
   struct retro_save_segment *copy;
   if (!segments || !num)
      return NULL;
   if (!(copy = (struct retro_save_segment*)
         malloc(num * sizeof(*copy))))
   {
      *ok = false;
      return NULL;
   }
   memcpy(copy, segments, num * sizeof(*copy));
   return copy;
}

/* Assigns every pointer member, so a failed copy still frees cleanly. */
static bool save_items_copy_item(struct retro_save_item *dst,
      const struct retro_save_item *src)
{
   bool ok             = true;
   *dst                = *src;
   dst->id             = save_items_strdup(src->id,        &ok);
   dst->label          = save_items_strdup(src->label,     &ok);
   dst->group          = save_items_strdup(src->group,     &ok);
   dst->scope_key      = save_items_strdup(src->scope_key, &ok);
   dst->game_key       = save_items_strdup(src->game_key,  &ok);
   dst->path           = save_items_strdup(src->path,      &ok);
   dst->format         = save_items_strdup(src->format,    &ok);
   dst->base_hash      = save_items_strdup(src->base_hash, &ok);
   dst->legacy_paths   = save_items_strv_dup(src->legacy_paths, &ok);
   dst->include        = save_items_strv_dup(src->include,      &ok);
   dst->exclude        = save_items_strv_dup(src->exclude,      &ok);
   dst->accepts        = save_items_strv_dup(src->accepts,      &ok);
   dst->segments       = save_items_segments_dup(src->segments,
         src->num_segments, &ok);
   if (!dst->segments)
      dst->num_segments = 0;
   return ok;
}

static void save_items_free_item(struct retro_save_item *item)
{
   free((void*)item->id);
   free((void*)item->label);
   free((void*)item->group);
   free((void*)item->scope_key);
   free((void*)item->game_key);
   free((void*)item->path);
   free((void*)item->format);
   free((void*)item->base_hash);
   save_items_strv_free(item->legacy_paths);
   save_items_strv_free(item->include);
   save_items_strv_free(item->exclude);
   save_items_strv_free(item->accepts);
   free((void*)item->segments);
}

static void save_items_set_free(save_items_set_t *set)
{
   unsigned i;
   if (!set)
      return;
   for (i = 0; i < set->num_items; i++)
   {
      save_items_free_item(&set->items[i].item);
      free(set->items[i].path);
      save_items_strv_free((const char *const *)set->items[i].legacy);
      free(set->items[i].tail);
   }
   free(set->items);
   free(set->legacy_names);
   free(set);
}

/* Item i of the core's array, read at the core's stride; members its
 * header lacks stay zero. */
static void save_items_read_item(struct retro_save_item *out,
      const struct retro_save_interface *iface, unsigned i)
{
   size_t size = iface->item_size < sizeof(*out)
      ? iface->item_size : sizeof(*out);
   memset(out, 0, sizeof(*out));
   memcpy(out, (const unsigned char*)iface->items
         + (size_t)i * iface->item_size, size);
}

static bool save_items_safe_name(const char *s);
static void save_items_tidy_path(char *s);

/* An id for a line; NULL only in a refused set. */
static const char *save_items_id(const struct retro_save_item *item)
{
   return item->id ? item->id : "";
}

/* Whether @key was said since the content loaded; remembered if not.
 * A key that cannot be remembered counts as said, so nothing repeats. */
static bool save_items_seen(char ***list, unsigned *num, const char *key)
{
   unsigned i;
   char **grown;
   for (i = 0; i < *num; i++)
      if (string_is_equal((*list)[i], key))
         return true;
   if (     *num >= SAVE_ITEMS_MAX_SEEN
         || !(grown = (char**)realloc(*list, (*num + 1) * sizeof(*grown))))
      return true;
   *list = grown;
   if (!(grown[*num] = strdup(key)))
      return true;
   (*num)++;
   return false;
}

static void save_items_strs_free(char ***list, unsigned *num)
{
   unsigned i;
   for (i = 0; i < *num; i++)
      free((*list)[i]);
   free(*list);
   *list = NULL;
   *num  = 0;
}

static void save_items_issues_free(void)
{
   unsigned i;
   for (i = 0; i < save_items_st.num_issues; i++)
      free((char*)save_items_st.issues[i].text);
   free(save_items_st.issues);
   save_items_st.issues      = NULL;
   save_items_st.num_issues  = 0;
   save_items_st.issues_room = 0;
}

/* The frontend, not the core, writes an item of @storage; an unknown
 * one counts, as the core meant the frontend to keep it. */
static bool save_items_frontend_writes(unsigned storage)
{
   return   storage == RETRO_SAVE_STORAGE_FRONTEND_BUFFER
         || storage == RETRO_SAVE_STORAGE_FRONTEND_BLOB
         || storage >  RETRO_SAVE_STORAGE_NOT_PERSISTED;
}

/* A finding about item @i (RETRO_SAVE_ISSUE_NO_ITEM: the call), as the
 * core gave it in @item: listed for get_issues, and logged once per
 * content load. @fmt gives the value, then the rule. @place NULL names
 * the member. */
static void save_items_find(save_items_check_t *c, unsigned i,
      const struct retro_save_item *item, unsigned field, unsigned verdict,
      const char *place, const char *fmt, ...)
{
   static const char *const actions[] = {
      "Registration refused.",
      "Item left out; it is not saved.",
      "Ignored.",
      "Item left out; the frontend makes no copies or backups of it."
   };
   unsigned action = verdict;
   char what[320];
   char who[160];
   char member[48];
   char line[768];
   va_list ap;
   struct retro_save_issue *grown;

   va_start(ap, fmt);
   vsnprintf(what, sizeof(what), fmt, ap);
   va_end(ap);
   if (i == RETRO_SAVE_ISSUE_NO_ITEM)
      strlcpy(who, "save registration", sizeof(who));
   else if (item->id && item->label)
      snprintf(who, sizeof(who), "save item %u \"%s\" (\"%s\")", i,
            item->id, item->label);
   else if (item->id)
      snprintf(who, sizeof(who), "save item %u \"%s\"", i, item->id);
   else
      snprintf(who, sizeof(who), "save item %u", i);
   if (     verdict == RETRO_SAVE_VERDICT_LEFT_OUT
         && !save_items_frontend_writes(item->storage))
      action = 3;
   if (!place)
   {
      snprintf(member, sizeof(member), "%s::%s",
            field >= RETRO_SAVE_FIELD_FLAGS
            ? "retro_save_item" : "retro_save_interface",
            save_items_field_names[field]);
      place = member;
   }
   snprintf(line, sizeof(line), "%s: %s: %s = %s. See libretro.h, %s. %s",
         string_is_empty(c->core) ? "Core" : c->core, who,
         save_items_field_names[field], what, place, actions[action]);

   if (verdict == RETRO_SAVE_VERDICT_REFUSED)
      c->refused = true;
   else if (verdict == RETRO_SAVE_VERDICT_LEFT_OUT)
      c->left    = true;
   if (     !c->room && !c->fixed
         && (grown = (struct retro_save_issue*)realloc(c->issues,
            (c->num_issues + 1) * sizeof(*grown))))
   {
      c->issues = grown;
      c->room   = 1;
   }
   if (c->room)
   {
      struct retro_save_issue *issue = &c->issues[c->num_issues];
      issue->item    = i;
      issue->field   = field;
      issue->verdict = verdict;
      if ((issue->text = strdup(line)))
      {
         c->num_issues++;
         c->room--;
      }
   }
   if (save_items_seen(&save_items_st.logged, &save_items_st.num_logged,
         line))
      return;
   if (c->lines >= SAVE_ITEMS_MAX_LINES)
   {
      c->more++;
      if (verdict != RETRO_SAVE_VERDICT_IGNORED)
         c->more_err = true;
      return;
   }
   c->lines++;
   if (verdict == RETRO_SAVE_VERDICT_IGNORED)
      RARCH_WARN("[SRAM] %s\n", line);
   else
      RARCH_ERR("[SRAM] %s\n", line);
}

/* By item, the call's own first; in finding order within one. */
static int save_items_issue_cmp(const void *a, const void *b)
{
   const struct retro_save_issue *x = *(const struct retro_save_issue**)a;
   const struct retro_save_issue *y = *(const struct retro_save_issue**)b;
   /* RETRO_SAVE_ISSUE_NO_ITEM wraps to 0. */
   unsigned kx = x->item + 1;
   unsigned ky = y->item + 1;
   if (kx != ky)
      return kx < ky ? -1 : 1;
   return x < y ? -1 : x > y ? 1 : 0;
}

/* The checks after the paths resolve add findings out of item order. */
static void save_items_issues_sort(save_items_check_t *c)
{
   unsigned i;
   const struct retro_save_issue **order;
   struct retro_save_issue *sorted;
   for (i = 1; i < c->num_issues; i++)
      if (c->issues[i].item + 1 < c->issues[i - 1].item + 1)
         break;
   if (i >= c->num_issues)
      return;
   if (!(order = (const struct retro_save_issue**)malloc(
         c->num_issues * sizeof(*order))))
      return;
   if (!(sorted = (struct retro_save_issue*)malloc(
         c->num_issues * sizeof(*sorted))))
   {
      free(order);
      return;
   }
   for (i = 0; i < c->num_issues; i++)
      order[i] = &c->issues[i];
   qsort((void*)order, c->num_issues, sizeof(*order),
         save_items_issue_cmp);
   for (i = 0; i < c->num_issues; i++)
      sorted[i] = *order[i];
   free(order);
   free(c->issues);
   c->issues = sorted;
}

/* The findings become the list get_issues returns. */
static void save_items_check_end(save_items_check_t *c)
{
   if (c->more && c->more_err)
      RARCH_ERR("[SRAM] %s: and %u more save item findings.\n",
            string_is_empty(c->core) ? "Core" : c->core, c->more);
   else if (c->more)
      RARCH_WARN("[SRAM] %s: and %u more save item findings.\n",
            string_is_empty(c->core) ? "Core" : c->core, c->more);
   save_items_issues_sort(c);
   save_items_issues_free();
   save_items_st.issues     = c->issues;
   save_items_st.num_issues = c->num_issues;
   c->issues                = NULL;
}

/* Room after the findings for those the first fill may add, one per
 * item it may leave out, so the list never moves after the call. */
static void save_items_issues_reserve(const save_items_set_t *set)
{
   unsigned i;
   unsigned room = 0;
   struct retro_save_issue *grown;
   for (i = 0; i < set->num_items; i++)
      if (!set->items[i].left_out && set->items[i].legacy_unreported)
         room++;
   if (     room
         && (grown = (struct retro_save_issue*)realloc(save_items_st.issues,
            (save_items_st.num_issues + room) * sizeof(*grown))))
   {
      save_items_st.issues      = grown;
      save_items_st.issues_room = room;
   }
}

/* False when the item array cannot be read: nothing after is checked. */
static bool save_items_check_call(save_items_check_t *c,
      const struct retro_save_interface *iface)
{
   if (!iface)
   {
      save_items_find(c, RETRO_SAVE_ISSUE_NO_ITEM, NULL,
            RETRO_SAVE_FIELD_NONE, RETRO_SAVE_VERDICT_REFUSED,
            "RETRO_ENVIRONMENT_SET_SAVE_INTERFACE",
            "NULL: data is the core's struct retro_save_interface");
      return false;
   }
   if (iface->num_items > SAVE_ITEMS_MAX_ITEMS)
      save_items_find(c, RETRO_SAVE_ISSUE_NO_ITEM, NULL,
            RETRO_SAVE_FIELD_NUM_ITEMS, RETRO_SAVE_VERDICT_REFUSED, NULL,
            "%u: more than %u items is taken as garbage",
            iface->num_items, SAVE_ITEMS_MAX_ITEMS);
   else if (iface->num_items && !iface->items)
      save_items_find(c, RETRO_SAVE_ISSUE_NO_ITEM, NULL,
            RETRO_SAVE_FIELD_ITEMS, RETRO_SAVE_VERDICT_REFUSED, NULL,
            "NULL: num_items is %u", iface->num_items);
   else if (     iface->num_items
            && (   iface->item_size < SAVE_ITEMS_ITEM_MIN_SIZE
                || iface->item_size > SAVE_ITEMS_ITEM_MAX_SIZE))
      save_items_find(c, RETRO_SAVE_ISSUE_NO_ITEM, NULL,
            RETRO_SAVE_FIELD_ITEM_SIZE, RETRO_SAVE_VERDICT_REFUSED, NULL,
            "%lu: it is sizeof(struct retro_save_item) in the core's "
            "build, %lu or more and at most %u",
            (unsigned long)iface->item_size,
            (unsigned long)SAVE_ITEMS_ITEM_MIN_SIZE,
            SAVE_ITEMS_ITEM_MAX_SIZE);
   if (c->refused)
      return false;
   if (!iface->interface_version)
      save_items_find(c, RETRO_SAVE_ISSUE_NO_ITEM, NULL,
            RETRO_SAVE_FIELD_INTERFACE_VERSION, RETRO_SAVE_VERDICT_REFUSED,
            NULL, "0: it is the version the core was written for, 1 or "
            "more");
   if (iface->request && !iface->get_result)
      save_items_find(c, RETRO_SAVE_ISSUE_NO_ITEM, NULL,
            RETRO_SAVE_FIELD_GET_RESULT, RETRO_SAVE_VERDICT_REFUSED, NULL,
            "NULL: it is required with request");
   return true;
}

/* Whether content @index is loaded. */
static bool save_items_content_known(const struct save_items_paths *paths,
      unsigned index)
{
   return paths->subsystem ? index < paths->subsystem->size : index == 0;
}

/* @path names its root directory itself, as "" and "." do. */
static bool save_items_names_root(const char *path)
{
   const char *s;
   if (!path || path_is_absolute(path))
      return false;
   for (s = path; *s; s++)
      if (     *s != '/' && *s != '\\'
            && !(   *s == '.'
                 && (s == path || s[-1] == '/' || s[-1] == '\\')
                 && (!s[1] || s[1] == '/' || s[1] == '\\')))
         return false;
   return true;
}

/* @pattern matches every name: '*'s, and at most one '?', as no name
 * is empty. */
static bool save_items_matches_all(const char *pattern)
{
   unsigned stars = 0;
   unsigned any   = 0;
   for (; *pattern; pattern++)
   {
      if (*pattern == '*')
         stars++;
      else if (*pattern == '?')
         any++;
      else
         return false;
   }
   return stars && any <= 1;
}

/* Folder item @i is its root directory itself, which holds files the
 * core does not write, so it names its own. */
static void save_items_check_root_folder(save_items_check_t *c, unsigned i,
      const struct retro_save_item *src)
{
   const char *const *k;
   if (!src->include || !src->include[0])
   {
      save_items_find(c, i, src, RETRO_SAVE_FIELD_INCLUDE,
            RETRO_SAVE_VERDICT_LEFT_OUT, NULL,
            "%s: a folder at the root directory itself names its own "
            "files, as that directory also holds files the core does not "
            "write", src->include ? "{ NULL }" : "NULL");
      return;
   }
   for (k = src->include; *k; k++)
      if (save_items_matches_all(*k))
      {
         save_items_find(c, i, src, RETRO_SAVE_FIELD_INCLUDE,
               RETRO_SAVE_VERDICT_IGNORED, NULL,
               "\"%s\": it selects every file of the root directory "
               "itself, which also holds files the core does not write",
               *k);
         return;
      }
}

/* Item @i's id against those of the items before it. */
static void save_items_check_id(save_items_check_t *c,
      const struct retro_save_interface *iface, unsigned i,
      const struct retro_save_item *src)
{
   unsigned j;
   if (!src->id)
      save_items_find(c, i, src, RETRO_SAVE_FIELD_ID,
            RETRO_SAVE_VERDICT_REFUSED, NULL,
            "NULL: every item needs one");
   for (j = 0; src->id && j < i; j++)
   {
      struct retro_save_item other;
      save_items_read_item(&other, iface, j);
      if (string_is_equal(other.id, src->id))
         save_items_find(c, i, src, RETRO_SAVE_FIELD_ID,
               RETRO_SAVE_VERDICT_REFUSED, NULL,
               "\"%s\": item %u has it too, and a frontend matches items "
               "by id", src->id, j);
      else if (other.id && string_is_equal_noncase(other.id, src->id))
         save_items_find(c, i, src, RETRO_SAVE_FIELD_ID,
               RETRO_SAVE_VERDICT_IGNORED, NULL,
               "\"%s\": item %u's \"%s\" differs only in case", src->id,
               j, other.id);
   }
}

/* Checks item @i as the core gave it in @src; the frontend's copy,
 * in @set, reads unknown descriptive values as UNKNOWN. */
static void save_items_check_item(save_items_check_t *c,
      const struct retro_save_interface *iface, save_items_set_t *set,
      unsigned i, const struct retro_save_item *src,
      const struct save_items_paths *paths)
{
   unsigned k;
   struct retro_save_item *it = &set->items[i].item;
   uint64_t unknown           = src->flags & ~(uint64_t)SAVE_ITEMS_ITEM_FLAGS;
   bool buffer = src->storage == RETRO_SAVE_STORAGE_FRONTEND_BUFFER;
   bool blob   = src->storage == RETRO_SAVE_STORAGE_FRONTEND_BLOB;
   bool core   =  src->storage == RETRO_SAVE_STORAGE_CORE_FILE
               || src->storage == RETRO_SAVE_STORAGE_CORE_FOLDER;
   bool needs_path = core || buffer || blob
               || src->storage == RETRO_SAVE_STORAGE_IN_CONTENT;
   /* What contradicts the storage says nothing more of an unknown one. */
   bool known  = src->storage <= RETRO_SAVE_STORAGE_NOT_PERSISTED;

   c->left = false;
   save_items_check_id(c, iface, i, src);
   if (!src->label)
      save_items_find(c, i, src, RETRO_SAVE_FIELD_LABEL,
            RETRO_SAVE_VERDICT_IGNORED, NULL,
            "NULL: it is the name the user sees");

   switch (src->storage)
   {
      case RETRO_SAVE_STORAGE_FRONTEND_BUFFER:
      {
         size_t total = 0;
         if (!src->segments || !src->num_segments)
         {
            save_items_find(c, i, src, src->segments
                  ? RETRO_SAVE_FIELD_NUM_SEGMENTS : RETRO_SAVE_FIELD_SEGMENTS,
                  RETRO_SAVE_VERDICT_LEFT_OUT, NULL,
                  "%s: a FRONTEND_BUFFER item needs at least one segment",
                  src->segments ? "0" : "NULL");
            break;
         }
         for (k = 0; k < src->num_segments; k++)
         {
            size_t size = src->segments[k].size;
            if (size && !src->segments[k].data)
               save_items_find(c, i, src, RETRO_SAVE_FIELD_SEGMENTS,
                     RETRO_SAVE_VERDICT_LEFT_OUT, NULL,
                     "segment %u data NULL: a segment of %lu bytes needs "
                     "its memory", k, (unsigned long)size);
            else if (size > (size_t)-1 - total)
               save_items_find(c, i, src, RETRO_SAVE_FIELD_SEGMENTS,
                     RETRO_SAVE_VERDICT_LEFT_OUT, NULL,
                     "segment %u size %lu: the sizes add up past the "
                     "largest size", k, (unsigned long)size);
            else
            {
               total += size;
               continue;
            }
            break;
         }
         break;
      }
      case RETRO_SAVE_STORAGE_FRONTEND_BLOB:
         if (!iface->blob_size || !iface->blob_save || !iface->blob_load)
            save_items_find(c, i, src, !iface->blob_size
                  ? RETRO_SAVE_FIELD_BLOB_SIZE : !iface->blob_save
                  ? RETRO_SAVE_FIELD_BLOB_SAVE : RETRO_SAVE_FIELD_BLOB_LOAD,
                  RETRO_SAVE_VERDICT_LEFT_OUT,
                  "struct retro_save_interface",
                  "NULL: a FRONTEND_BLOB item needs blob_size, blob_save "
                  "and blob_load");
         break;
      case RETRO_SAVE_STORAGE_CORE_FILE:
      case RETRO_SAVE_STORAGE_CORE_FOLDER:
      case RETRO_SAVE_STORAGE_IN_CONTENT:
      case RETRO_SAVE_STORAGE_EXTERNAL:
      case RETRO_SAVE_STORAGE_NOT_PERSISTED:
         break;
      default:
         save_items_find(c, i, src, RETRO_SAVE_FIELD_STORAGE,
               RETRO_SAVE_VERDICT_LEFT_OUT, NULL,
               "%u: not an enum retro_save_storage value", src->storage);
         needs_path = false;
         break;
   }
   if (known && !buffer && src->segments)
      save_items_find(c, i, src, RETRO_SAVE_FIELD_SEGMENTS,
            RETRO_SAVE_VERDICT_IGNORED, NULL,
            "set: only a FRONTEND_BUFFER item has segments");

   if (needs_path)
   {
      bool indexed = false;
      switch (src->root)
      {
         case RETRO_SAVE_ROOT_FRONTEND:
            indexed = true;
            if (!save_items_safe_name(src->path))
               save_items_find(c, i, src, RETRO_SAVE_FIELD_PATH,
                     RETRO_SAVE_VERDICT_LEFT_OUT, NULL,
                     src->path ? "\"%s\": under ROOT_FRONTEND it is an "
                     "extension of letters, digits, '_', '-' and '.'"
                     : "%s: under ROOT_FRONTEND it is the extension",
                     src->path ? src->path : "NULL");
            else if (src->path[0] == '.')
               save_items_find(c, i, src, RETRO_SAVE_FIELD_PATH,
                     RETRO_SAVE_VERDICT_LEFT_OUT, NULL,
                     "\"%s\": under ROOT_FRONTEND it is the extension "
                     "without a dot (\"%s\")", src->path, src->path + 1);
            break;
         case RETRO_SAVE_ROOT_CONTENT_DIR:
            indexed = true;
            /* fall through */
         case RETRO_SAVE_ROOT_SAVE_DIR:
         case RETRO_SAVE_ROOT_SYSTEM_DIR:
            /* A folder may be the directory itself. */
            if (     src->storage == RETRO_SAVE_STORAGE_CORE_FOLDER
                  && save_items_names_root(src->path))
            {
               save_items_check_root_folder(c, i, src);
               break;
            }
            /* fall through */
         case RETRO_SAVE_ROOT_ABSOLUTE:
            if (string_is_empty(src->path))
               save_items_find(c, i, src, RETRO_SAVE_FIELD_PATH,
                     RETRO_SAVE_VERDICT_LEFT_OUT, NULL,
                     "%s: this root needs a path",
                     src->path ? "\"\"" : "NULL");
            break;
         case RETRO_SAVE_ROOT_CONTENT_FILE:
            indexed = true;
            if (buffer || blob)
               save_items_find(c, i, src, RETRO_SAVE_FIELD_ROOT,
                     RETRO_SAVE_VERDICT_LEFT_OUT, NULL,
                     "ROOT_CONTENT_FILE: a frontend-owned item there would "
                     "be written over the content file");
            break;
         default:
            save_items_find(c, i, src, RETRO_SAVE_FIELD_ROOT,
                  RETRO_SAVE_VERDICT_LEFT_OUT, NULL,
                  "%u: not an enum retro_save_root value", src->root);
            break;
      }
      if (indexed && !save_items_content_known(paths, src->content_index))
         save_items_find(c, i, src, RETRO_SAVE_FIELD_CONTENT_INDEX,
               RETRO_SAVE_VERDICT_LEFT_OUT, NULL,
               "%u: no content is loaded at that index",
               src->content_index);
   }

   if (src->scope > RETRO_SAVE_SCOPE_OTHER_CONTENT)
   {
      save_items_find(c, i, src, RETRO_SAVE_FIELD_SCOPE,
            RETRO_SAVE_VERDICT_IGNORED, NULL,
            "%u: not an enum retro_save_scope value; read as UNKNOWN",
            src->scope);
      it->scope = RETRO_SAVE_SCOPE_UNKNOWN;
   }
   if (src->role > RETRO_SAVE_ROLE_CACHE)
   {
      save_items_find(c, i, src, RETRO_SAVE_FIELD_ROLE,
            RETRO_SAVE_VERDICT_IGNORED, NULL,
            "%u: not an enum retro_save_role value; read as UNKNOWN",
            src->role);
      it->role = RETRO_SAVE_ROLE_UNKNOWN;
   }
   if (src->state_relation > RETRO_SAVE_STATE_STORED_IGNORED)
   {
      save_items_find(c, i, src, RETRO_SAVE_FIELD_STATE_RELATION,
            RETRO_SAVE_VERDICT_IGNORED, NULL,
            "%u: not an enum retro_save_state_relation value; read as "
            "UNKNOWN", src->state_relation);
      it->state_relation = RETRO_SAVE_STATE_UNKNOWN;
   }
   else if ((buffer || blob)
         && src->state_relation == RETRO_SAVE_STATE_LOAD_WRITES_FILE)
      save_items_find(c, i, src, RETRO_SAVE_FIELD_STATE_RELATION,
            RETRO_SAVE_VERDICT_IGNORED, NULL,
            "LOAD_WRITES_FILE: only a core-written item writes its file "
            "when a state loads");
   if (src->write_timing > RETRO_SAVE_WRITE_FRONTEND)
   {
      save_items_find(c, i, src, RETRO_SAVE_FIELD_WRITE_TIMING,
            RETRO_SAVE_VERDICT_IGNORED, NULL,
            "%u: not an enum retro_save_write_timing value; read as "
            "UNKNOWN", src->write_timing);
      it->write_timing = RETRO_SAVE_WRITE_UNKNOWN;
   }
   else if ((buffer || blob)
         && src->write_timing != RETRO_SAVE_WRITE_FRONTEND
         && src->write_timing != RETRO_SAVE_WRITE_UNKNOWN)
      save_items_find(c, i, src, RETRO_SAVE_FIELD_WRITE_TIMING,
            RETRO_SAVE_VERDICT_IGNORED, NULL,
            "%u: a frontend-owned item's is FRONTEND", src->write_timing);
   else if (core && src->write_timing == RETRO_SAVE_WRITE_FRONTEND)
      save_items_find(c, i, src, RETRO_SAVE_FIELD_WRITE_TIMING,
            RETRO_SAVE_VERDICT_IGNORED, NULL,
            "FRONTEND: only a frontend-owned item's is FRONTEND");

   if (unknown)
   {
      save_items_find(c, i, src, RETRO_SAVE_FIELD_FLAGS,
            RETRO_SAVE_VERDICT_IGNORED, NULL,
            "0x%08lx%08lx: bits this frontend does not know are dropped",
            (unsigned long)(unknown >> 32),
            (unsigned long)(unknown & 0xffffffffu));
      it->flags &= SAVE_ITEMS_ITEM_FLAGS;
   }
   if (known && (src->flags & RETRO_SAVE_FLAG_EXACT_SIZE) && !buffer)
      save_items_find(c, i, src, RETRO_SAVE_FIELD_FLAGS,
            RETRO_SAVE_VERDICT_IGNORED, "RETRO_SAVE_FLAG_EXACT_SIZE",
            "EXACT_SIZE: it applies only to FRONTEND_BUFFER items");
   if (     known
         && (src->flags & RETRO_SAVE_FLAG_FRONTEND_MAY_TRANSFORM)
         && !buffer && !blob)
      save_items_find(c, i, src, RETRO_SAVE_FIELD_FLAGS,
            RETRO_SAVE_VERDICT_IGNORED,
            "RETRO_SAVE_FLAG_FRONTEND_MAY_TRANSFORM",
            "FRONTEND_MAY_TRANSFORM: it applies only to frontend-owned "
            "items");
   if ((src->flags & RETRO_SAVE_FLAG_DIFF_OF_CONTENT) && !src->base_hash)
      save_items_find(c, i, src, RETRO_SAVE_FIELD_BASE_HASH,
            RETRO_SAVE_VERDICT_IGNORED, NULL,
            "NULL: a DIFF_OF_CONTENT item names the content it applies to");
   else if (!(src->flags & RETRO_SAVE_FLAG_DIFF_OF_CONTENT) && src->base_hash)
      save_items_find(c, i, src, RETRO_SAVE_FIELD_BASE_HASH,
            RETRO_SAVE_VERDICT_IGNORED, NULL,
            "\"%s\": only a DIFF_OF_CONTENT item has one", src->base_hash);
   if (     known && (src->include || src->exclude)
         && src->storage != RETRO_SAVE_STORAGE_CORE_FOLDER)
      save_items_find(c, i, src, src->include
            ? RETRO_SAVE_FIELD_INCLUDE : RETRO_SAVE_FIELD_EXCLUDE,
            RETRO_SAVE_VERDICT_IGNORED, NULL,
            "set: only a CORE_FOLDER item has patterns");
   set->items[i].left_out = c->left;
}

static const char *save_items_content_path(
      const struct save_items_paths *paths, unsigned index)
{
   if (paths->subsystem)
      return index < paths->subsystem->size
         ? paths->subsystem->elems[index].data : NULL;
   return index == 0 ? paths->content : NULL;
}

/* A file name character no platform treats specially. */
static bool save_items_safe_char(char c)
{
   return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
      || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
}

/* Safe characters only, so no separator, and not "." or "..": joined
 * to a directory, it names something inside it. */
static bool save_items_safe_name(const char *s)
{
   const char *c;
   if (     string_is_empty(s)
         || string_is_equal(s, ".")
         || string_is_equal(s, ".."))
      return false;
   for (c = s; *c; c++)
      if (!save_items_safe_char(*c))
         return false;
   return true;
}

/* The name the frontend gives an extension, as save RAM gets its own:
 * beside content 0's .srm, or per content in a subsystem session.
 * @s holds two PATH_MAX_LENGTH halves; the second is scratch. */
static size_t save_items_name_frontend(char *s,
      const struct save_items_paths *paths, unsigned index,
      const char *ext)
{
   char *savename = s + PATH_MAX_LENGTH;
   const char *content;
   if (!paths->subsystem)
   {
      if (index != 0 || string_is_empty(paths->savefile))
         return 0;
      /* A --save override names save RAM whatever its extension. */
      if (string_is_equal(ext, FILE_PATH_SRM_EXTENSION))
         return strlcpy(s, paths->savefile, PATH_MAX_LENGTH);
      return fill_pathname(s, paths->savefile, ext, PATH_MAX_LENGTH);
   }
   if (string_is_empty(content = save_items_content_path(paths, index)))
      return 0;
   fill_pathname(savename, content, "", NAME_MAX_LENGTH);
   if (paths->savefile_dir_is_dir)
   {
      strlcpy(s, paths->savefile_dir, PATH_MAX_LENGTH);
      return fill_pathname_dir(s, savename, ext, PATH_MAX_LENGTH);
   }
   return fill_pathname(s, savename, ext, PATH_MAX_LENGTH);
}

static size_t save_items_join(char *s, const char *dir, const char *path)
{
   if (!path)
      return 0;
   if (path_is_absolute(path))
      return strlcpy(s, path, PATH_MAX_LENGTH);
   if (string_is_empty(dir))
      return 0;
   return fill_pathname_join(s, dir, path, PATH_MAX_LENGTH);
}

/* @s as for save_items_name_frontend(). Returns 0 when there is
 * no path. */
static size_t save_items_name(char *s, const struct retro_save_item *item,
      const struct save_items_paths *paths)
{
   const char *content;
   s[0] = '\0';
   switch (item->root)
   {
      case RETRO_SAVE_ROOT_FRONTEND:
         /* No name rather than one outside the save dir. */
         if (!save_items_safe_name(item->path))
            return 0;
         {
            char ext[NAME_MAX_LENGTH];
            ext[0] = '.';
            if (strlcpy(ext + 1, item->path, sizeof(ext) - 1)
                  >= sizeof(ext) - 1)
               return 0;
            return save_items_name_frontend(s, paths,
                  item->content_index, ext);
         }
      case RETRO_SAVE_ROOT_SAVE_DIR:
         return save_items_join(s, paths->savefile_dir, item->path);
      case RETRO_SAVE_ROOT_SYSTEM_DIR:
         return save_items_join(s, paths->system_dir, item->path);
      case RETRO_SAVE_ROOT_CONTENT_DIR:
         if (string_is_empty(content = save_items_content_path(paths,
               item->content_index)))
            return 0;
         {
            char *dir = s + PATH_MAX_LENGTH;
            fill_pathname_basedir(dir, content, PATH_MAX_LENGTH);
            return save_items_join(s, dir, item->path);
         }
      case RETRO_SAVE_ROOT_CONTENT_FILE:
         if (string_is_empty(content = save_items_content_path(paths,
               item->content_index)))
            return 0;
         return strlcpy(s, content, PATH_MAX_LENGTH);
      case RETRO_SAVE_ROOT_ABSOLUTE:
         if (string_is_empty(item->path))
            return 0;
         return strlcpy(s, item->path, PATH_MAX_LENGTH);
      default:
         break;
   }
   return 0;
}

/* The earlier names an item is read from when its own is missing,
 * under the same root. @buf as for save_items_name(). */
static char **save_items_resolve_legacy(const struct retro_save_item *item,
      const struct save_items_paths *paths, char *buf)
{
   size_t n = 0;
   size_t k = 0;
   char **out;
   struct retro_save_item named;

   if (     !item->legacy_paths
         || !item->legacy_paths[0]
         || item->root == RETRO_SAVE_ROOT_CONTENT_FILE)
      return NULL;
   while (item->legacy_paths[n])
      n++;
   if (!(out = (char**)calloc(n + 1, sizeof(*out))))
      return NULL;
   named = *item;
   for (n = 0; item->legacy_paths[n]; n++)
   {
      named.path = item->legacy_paths[n];
      if (save_items_name(buf, &named, paths) <= 0)
         continue;
      save_items_tidy_path(buf);
      if ((out[k] = strdup(buf)))
         k++;
   }
   return out;
}

/* Whether item @i of @set held its file at the previous call. */
static bool save_items_held(const save_items_set_t *set, unsigned i,
      const save_items_set_t *cur)
{
   unsigned j;
   for (j = 0; cur && j < cur->num_items; j++)
      if (     string_is_equal(cur->items[j].item.id, set->items[i].item.id)
            && cur->items[j].path
            && string_is_equal_noncase(cur->items[j].path,
               set->items[i].path))
         return true;
   return false;
}

/* @path is inside folder @dir. */
static bool save_items_inside(const char *path, const char *dir)
{
   size_t len = strlen(dir);
   return   len
         && string_starts_with_case_insensitive(path, dir)
         && (path[len] == '/' || path[len] == '\\');
}

/* The length of pattern token @p when it matches character @c, else
 * 0: a list or an escape left open matches nothing. */
static size_t save_items_token(const char *p, char c)
{
   const char *q;
   bool listed = false;
   switch (*p)
   {
      case '\0':
         return 0;
      case '?':
         return 1;
      case '\\':
         return p[1] == c ? 2 : 0;
      case '[':
         for (q = p + 1; *q != ']'; q++)
         {
            if (*q == '\\' && q[1])
               q++;
            if (!*q)
               return 0;
            if (*q == c)
               listed = true;
         }
         return listed ? (size_t)(q - p) + 1 : 0;
      default:
         break;
   }
   return *p == c ? 1 : 0;
}

/* Whether @p matches the whole of @s. A mismatch goes back only to the
 * last '*', which takes one more character, so the cost stays within
 * pattern length times name length. */
static bool save_items_glob(const char *p, const char *s)
{
   const char *star = NULL;
   const char *from = NULL;
   for (;;)
   {
      size_t len;
      if (*p == '*')
      {
         while (*p == '*')
            p++;
         if (!*p)
            return true;
         star = p;
         from = s;
         continue;
      }
      if (!*s && !*p)
         return true;
      if (*s && (len = save_items_token(p, *s)))
      {
         p += len;
         s++;
         continue;
      }
      if (!star || !*from)
         return false;
      p = star;
      s = ++from;
   }
}

/* Whether a pattern of @list matches @name, a path inside a folder,
 * compared as paths are here: case aside, and '\\' read as '/'.
 * @buf holds two PATH_MAX_LENGTH halves. */
static bool save_items_match(const char *const *list, const char *name,
      char *buf)
{
   char *pattern = buf + PATH_MAX_LENGTH;
   char *s;
   strlcpy(buf, name, PATH_MAX_LENGTH);
   for (s = buf; *s; s++)
      *s = *s == '\\' ? '/' : (char)tolower((unsigned char)*s);
   for (; *list; list++)
   {
      strlcpy(pattern, *list, PATH_MAX_LENGTH);
      for (s = pattern; *s; s++)
         *s = (char)tolower((unsigned char)*s);
      if (save_items_glob(pattern, buf))
         return true;
   }
   return false;
}

/* Folder item @f's patterns select @name, a path inside it: an include
 * pattern (no or an empty list: everything) matches and no exclude one.
 * @buf as for save_items_match(); without it, everything is selected. */
static bool save_items_selects(const save_item_t *f, const char *name,
      char *buf)
{
   bool include = f->item.include && f->item.include[0];
   if (!buf || (!include && !f->item.exclude))
      return true;
   if (include && !save_items_match(f->item.include, name, buf))
      return false;
   return !f->item.exclude || !save_items_match(f->item.exclude, name, buf);
}

/* Folder item @f holds @path: it is inside, and selected. */
static bool save_items_holds(const save_item_t *f, const char *path,
      char *buf)
{
   return   f->path && save_items_inside(path, f->path)
         && save_items_selects(f, path + strlen(f->path) + 1, buf);
}

/* The temps the frontend writes a file through, as save.c names them:
 * its own writes, then the autosave worker's. */
static const char *const save_items_temps[] = { ".tmp", ".auto.tmp", NULL };

/* Whether folder item @f holds @name, of @len characters, with one of
 * @ends added: then @name names that file. @buf as for
 * save_items_match(). */
static bool save_items_holds_ends(const save_item_t *f, char *name,
      size_t len, const char *const *ends, char *buf)
{
   for (; *ends; ends++)
      if (     strlcpy(name + len, *ends, PATH_MAX_LENGTH - len)
               < PATH_MAX_LENGTH - len
            && save_items_holds(f, name, buf))
         return true;
   return false;
}

/* Leaves out item @i for its file, @why naming item @j. */
static void save_items_leave_file(save_items_check_t *c,
      save_items_set_t *set, unsigned i, unsigned j, unsigned field,
      const char *name, const char *why)
{
   save_items_find(c, i, &set->items[i].item, field,
         RETRO_SAVE_VERDICT_LEFT_OUT, "SET_SAVE_INTERFACE, \"Ownership\"",
         "\"%s\": %s item %u \"%s\", and a file has one writer", name, why,
         j, save_items_id(&set->items[j].item));
   set->items[i].left_out = true;
}

/* Checks the resolved files: a frontend-owned item's is not the
 * content, nor another item's or inside its folder, and no item lists
 * another item's file among its earlier names. Of two items at one
 * file, the one that held it at the previous call, or a core-written
 * one, or else the first, keeps it. */
static void save_items_check_files(save_items_check_t *c,
      save_items_set_t *set, const save_items_set_t *cur,
      const struct save_items_paths *paths)
{
   unsigned i, j, k;
   char *buf  = (char*)malloc(3 * PATH_MAX_LENGTH);
   char *temp = buf ? buf + 2 * PATH_MAX_LENGTH : NULL;
   for (i = 0; i < set->num_items; i++)
   {
      save_item_t *a = &set->items[i];
      bool content   = false;
      if (a->left_out || !save_items_frontend_owned(a))
         continue;
      if (paths->subsystem)
      {
         for (k = 0; k < paths->subsystem->size; k++)
            if (string_is_equal_noncase(paths->subsystem->elems[k].data,
                  a->path))
               content = true;
      }
      else if (paths->content)
         content = string_is_equal_noncase(paths->content, a->path);
      if (content)
      {
         save_items_find(c, i, &a->item, RETRO_SAVE_FIELD_PATH,
               RETRO_SAVE_VERDICT_LEFT_OUT, "SET_SAVE_INTERFACE, \"In content\"",
               "\"%s\": it is the content file, which a frontend never "
               "writes", a->path);
         a->left_out = true;
      }
   }
   for (i = 0; i < set->num_items; i++)
   {
      save_item_t *a = &set->items[i];
      for (j = 0; !a->left_out && a->path && j < set->num_items; j++)
      {
         save_item_t *b = &set->items[j];
         bool fa        = save_items_frontend_owned(a);
         bool fb        = save_items_frontend_owned(b);
         if (j == i || b->left_out || !b->path)
            continue;
         if ((fa || fb) && string_is_equal_noncase(a->path, b->path))
         {
            bool a_loses;
            if (fa != fb)
               a_loses = fa;
            else if (save_items_held(set, i, cur)
                  != save_items_held(set, j, cur))
               a_loses = save_items_held(set, j, cur);
            else
               a_loses = i > j;
            if (a_loses)
               save_items_leave_file(c, set, i, j, RETRO_SAVE_FIELD_PATH,
                     a->path, "the file of");
            else
               save_items_leave_file(c, set, j, i, RETRO_SAVE_FIELD_PATH,
                     b->path, "the file of");
            continue;
         }
         if (     fa
               && b->item.storage == RETRO_SAVE_STORAGE_CORE_FOLDER
               && save_items_holds(b, a->path, buf))
         {
            save_items_leave_file(c, set, i, j, RETRO_SAVE_FIELD_PATH,
                  a->path, b->item.include || b->item.exclude
                  ? "inside the folder and patterns of"
                  : "inside the folder of");
            continue;
         }
         if (     fa && temp
               && b->item.storage == RETRO_SAVE_STORAGE_CORE_FOLDER
               && save_items_holds_ends(b, temp,
                  strlcpy(temp, a->path, PATH_MAX_LENGTH),
                  save_items_temps, buf))
         {
            save_items_find(c, i, &a->item, RETRO_SAVE_FIELD_PATH,
                  RETRO_SAVE_VERDICT_LEFT_OUT,
                  "SET_SAVE_INTERFACE, \"Ownership\"",
                  "\"%s\": the frontend writes it through \"%s\", inside "
                  "the folder and patterns of item %u \"%s\", and a file "
                  "has one writer", a->path, temp, j,
                  save_items_id(&b->item));
            a->left_out = true;
            continue;
         }
         for (k = 0; a->legacy && a->legacy[k]; k++)
            if (string_is_equal_noncase(a->legacy[k], b->path))
            {
               save_items_leave_file(c, set, i, j,
                     RETRO_SAVE_FIELD_LEGACY_PATHS, a->legacy[k],
                     "an earlier name is the file of");
               break;
            }
      }
   }
   free(buf);
}

/* Where the frontend writes save RAM and RTC itself, in two
 * PATH_MAX_LENGTH halves; NULL in a subsystem session, whose names
 * are not known yet. */
static char *save_items_legacy_names(const struct save_items_paths *paths)
{
   char *names;
   if (     paths->subsystem || string_is_empty(paths->savefile)
         || !(names = (char*)malloc(2 * PATH_MAX_LENGTH)))
      return NULL;
   strlcpy(names, paths->savefile, PATH_MAX_LENGTH);
   fill_pathname(names + PATH_MAX_LENGTH, paths->savefile, ".rtc",
         PATH_MAX_LENGTH);
   save_items_tidy_path(names);
   save_items_tidy_path(names + PATH_MAX_LENGTH);
   return names;
}

/* A kept item of @set, or an earlier set, retired legacy memory @mem. */
static bool save_items_memory_retired(const save_items_set_t *set,
      unsigned mem)
{
   unsigned k;
   for (k = 0; k < set->num_items; k++)
      if (!set->items[k].left_out && set->items[k].item.legacy_memory == mem)
         return true;
   for (k = 0; k < save_items_st.num_retired; k++)
      if (save_items_st.retired[k] == mem)
         return true;
   return false;
}

/* Leaves out item @i, at legacy memory @mem's file @name or a folder
 * holding it, while the core reports that memory. */
static void save_items_leave_legacy(save_items_check_t *c,
      save_items_set_t *set, unsigned i, unsigned mem, const char *name)
{
   char value[32];
   save_item_t *it   = &set->items[i];
   const char *which = mem ? "RETRO_MEMORY_RTC" : "RETRO_MEMORY_SAVE_RAM";
   if (it->item.storage == RETRO_SAVE_STORAGE_CORE_FOLDER)
      save_items_find(c, i, &it->item, RETRO_SAVE_FIELD_PATH,
            RETRO_SAVE_VERDICT_LEFT_OUT,
            "SET_SAVE_INTERFACE, \"Ownership\"",
            "\"%s\": the folder holds \"%s\", where the frontend "
            "writes %s, which the core reports and no item replaces, "
            "and a file has one writer; exclude it, or name that memory "
            "in legacy_memory", it->item.path, name, which);
   else
   {
      if (it->item.legacy_memory == RETRO_SAVE_NO_LEGACY_MEMORY)
         strlcpy(value, "RETRO_SAVE_NO_LEGACY_MEMORY", sizeof(value));
      else
         snprintf(value, sizeof(value), "%u", it->item.legacy_memory);
      save_items_find(c, i, &it->item, RETRO_SAVE_FIELD_LEGACY_MEMORY,
            RETRO_SAVE_VERDICT_LEFT_OUT,
            "SET_SAVE_INTERFACE, \"Ownership\"",
            "%s: the file \"%s\" is where the frontend writes %s, "
            "which the core reports and no item replaces, and a file "
            "has one writer", value, name, which);
   }
   it->left_out = true;
}

/* The legacy memories the kept items retire: known ones, one item
 * each, and each written by its item. */
static void save_items_check_legacy(save_items_check_t *c,
      save_items_set_t *set, const struct save_items_paths *paths)
{
   unsigned i, j;
   char *names = save_items_legacy_names(paths);
   for (i = 0; names && i < set->num_items; i++)
   {
      unsigned t;
      save_item_t *it = &set->items[i];
      if (it->left_out || !it->path)
         continue;
      for (t = 0; t < 2; t++)
      {
         unsigned mem     = t ? RETRO_MEMORY_RTC : RETRO_MEMORY_SAVE_RAM;
         const char *name = names + t * PATH_MAX_LENGTH;
         if (!string_is_equal(it->path, name))
            continue;
         if (it->item.storage == RETRO_SAVE_STORAGE_FRONTEND_BLOB)
            save_items_find(c, i, &it->item, RETRO_SAVE_FIELD_PATH,
                  RETRO_SAVE_VERDICT_IGNORED,
                  "SET_SAVE_INTERFACE, \"Cross-core compatibility\"",
                  "\"%s\": the frontend writes %s raw there, and a core "
                  "never moves an existing raw file to a blob", name,
                  t ? "RETRO_MEMORY_RTC" : "RETRO_MEMORY_SAVE_RAM");
         if (     it->item.legacy_memory == mem
               || save_items_memory_retired(set, mem))
            continue;
         /* Without that memory the frontend writes no file there. */
         if (!paths->legacy_live[mem])
            it->legacy_unreported |= 1 << mem;
         else
            save_items_leave_legacy(c, set, i, mem, name);
      }
   }
   for (i = 0; i < set->num_items; i++)
   {
      const save_item_t *it = &set->items[i];
      unsigned mem          = it->item.legacy_memory;
      if (it->left_out || mem == RETRO_SAVE_NO_LEGACY_MEMORY)
         continue;
      for (j = 0; j < i; j++)
         if (     !set->items[j].left_out
               && set->items[j].item.legacy_memory == mem)
         {
            save_items_find(c, i, &it->item,
                  RETRO_SAVE_FIELD_LEGACY_MEMORY,
                  RETRO_SAVE_VERDICT_IGNORED, NULL,
                  "%u: item %u \"%s\" replaces it too, and one item "
                  "replaces each memory%s", mem, j,
                  save_items_id(&set->items[j].item),
                  mem == RETRO_MEMORY_SAVE_RAM
                  ? "; 0 is RETRO_MEMORY_SAVE_RAM" : "");
            break;
         }
      /* Its old file is read only through its own name or legacy_paths. */
      if (     j == i && names && it->path
            && (mem == RETRO_MEMORY_SAVE_RAM || mem == RETRO_MEMORY_RTC))
      {
         unsigned n;
         const char *old = names
            + (mem == RETRO_MEMORY_RTC ? PATH_MAX_LENGTH : 0);
         bool listed     = string_is_equal(it->path, old);
         for (n = 0; !listed && it->legacy && it->legacy[n]; n++)
            listed = string_is_equal(it->legacy[n], old);
         if (!listed)
            save_items_find(c, i, &it->item, RETRO_SAVE_FIELD_LEGACY_PATHS,
                  RETRO_SAVE_VERDICT_IGNORED,
                  "SET_SAVE_INTERFACE, \"Cross-core compatibility\"",
                  "%s: \"%s\", where the frontend saved this memory "
                  "before, is not among them, so a save there is not "
                  "loaded%s", it->legacy ? "set" : "NULL", old,
                  mem == RETRO_MEMORY_SAVE_RAM
                  ? "; 0 is RETRO_MEMORY_SAVE_RAM" : "");
      }
      if (     mem != RETRO_MEMORY_SAVE_RAM
            && mem != RETRO_MEMORY_RTC
            && mem < 0x100)
         save_items_find(c, i, &it->item, RETRO_SAVE_FIELD_LEGACY_MEMORY,
               RETRO_SAVE_VERDICT_IGNORED, NULL,
               "%u: not RETRO_MEMORY_SAVE_RAM, RETRO_MEMORY_RTC or a "
               "subsystem memory type", mem);
      if (     it->item.storage == RETRO_SAVE_STORAGE_EXTERNAL
            || it->item.storage == RETRO_SAVE_STORAGE_NOT_PERSISTED
            || (   (   it->item.storage == RETRO_SAVE_STORAGE_FRONTEND_BUFFER
                    || it->item.storage == RETRO_SAVE_STORAGE_FRONTEND_BLOB)
                && !it->path))
         save_items_find(c, i, &it->item, RETRO_SAVE_FIELD_LEGACY_MEMORY,
               RETRO_SAVE_VERDICT_IGNORED, NULL,
               "%u: this item keeps no file, so that memory is no longer "
               "saved%s", mem, mem == RETRO_MEMORY_SAVE_RAM
               ? "; 0 is RETRO_MEMORY_SAVE_RAM" : "");
   }
   free(names);
}

/* Whether folder item @f holds legacy file @name or its write temp:
 * then @buf, of three PATH_MAX_LENGTH parts, names it. */
static bool save_items_holds_legacy(const save_item_t *f, const char *name,
      char *buf)
{
   static const char *const ends[] = { "", ".tmp", NULL };
   return save_items_holds_ends(f, buf, strlcpy(buf, name, PATH_MAX_LENGTH),
         ends, buf + PATH_MAX_LENGTH);
}

/* Whether folder item @f holds @base plus a slot and one of @ends:
 * each of @forms, then the numbers @from to 999, where the slot
 * hotkeys wrap. Then @buf, of three PATH_MAX_LENGTH parts, names it. */
static bool save_items_holds_slot(const save_item_t *f, const char *base,
      const char *const *forms, unsigned from, const char *const *ends,
      char *buf)
{
   unsigned k;
   size_t len;
   strlcpy(buf, base, PATH_MAX_LENGTH);
   save_items_tidy_path(buf);
   len = strlen(buf);
   if (len + 16 >= PATH_MAX_LENGTH || !save_items_inside(buf, f->path))
      return false;
   for (; *forms; forms++)
      if (save_items_holds_ends(f, buf,
            len + strlcpy(buf + len, *forms, PATH_MAX_LENGTH - len), ends,
            buf + PATH_MAX_LENGTH))
         return true;
   for (k = from; k <= 999; k++)
      if (save_items_holds_ends(f, buf, len + (size_t)snprintf(buf + len,
            PATH_MAX_LENGTH - len, "%u", k), ends, buf + PATH_MAX_LENGTH))
         return true;
   return false;
}

/* Whether folder item @f holds a file the frontend writes or reads for
 * this content beside its .srm and .rtc: then @buf, as for
 * save_items_holds_slot(), names it, and the result says what for. */
static const char *save_items_holds_own(const save_item_t *f,
      const struct save_items_paths *paths, char *buf)
{
   static const char *const state_forms[] = { "", ".auto", NULL };
   static const char *const state_ends[]  = { "", FILE_PATH_PNG_EXTENSION,
      ".tmp", NULL };
   static const char *const entry_ends[]  = { ".entry", NULL };
   static const char *const bare[]        = { "", NULL };
   static const char *const none[]        = { NULL };
   static const char *const disk_ends[]   = { "", ".tmp", NULL };
   if (!string_is_empty(paths->savestate))
   {
      if (save_items_holds_slot(f, paths->savestate, state_forms, 1,
            state_ends, buf))
         return "writes for savestates";
      if (save_items_holds_slot(f, paths->savestate, none, 0, entry_ends,
            buf))
         return "loads as an entry state";
   }
   if (     !string_is_empty(paths->replay)
         && save_items_holds_slot(f, paths->replay, bare, 0, bare, buf))
      return "writes for replays";
   if (     !string_is_empty(paths->disk_index)
         && save_items_holds_slot(f, paths->disk_index, bare, 1000,
            disk_ends, buf))
      return "writes for the disk index";
   return NULL;
}

/* Leaves out a folder item that holds a file the frontend writes
 * itself, as that writer is not an item: the live .srm and .rtc, this
 * content's savestates, thumbnails, entry states, replays and disk
 * index, and the write temps of these. */
static void save_items_check_frontend_files(save_items_check_t *c,
      save_items_set_t *set, const struct save_items_paths *paths)
{
   unsigned i, t;
   char *names = save_items_legacy_names(paths);
   char *buf   = (char*)malloc(3 * PATH_MAX_LENGTH);
   for (i = 0; buf && i < set->num_items; i++)
   {
      const char *what;
      save_item_t *it = &set->items[i];
      if (     it->left_out || !it->path
            || it->item.storage != RETRO_SAVE_STORAGE_CORE_FOLDER)
         continue;
      for (t = 0; names && t < 2; t++)
      {
         unsigned mem = t ? RETRO_MEMORY_RTC : RETRO_MEMORY_SAVE_RAM;
         if (     save_items_memory_retired(set, mem)
               || !save_items_holds_legacy(it,
                  names + t * PATH_MAX_LENGTH, buf))
            continue;
         if (!paths->legacy_live[mem])
            it->legacy_unreported |= 1 << mem;
         else
         {
            save_items_leave_legacy(c, set, i, mem, buf);
            break;
         }
      }
      if (it->left_out || !(what = save_items_holds_own(it, paths, buf)))
         continue;
      save_items_find(c, i, &it->item, RETRO_SAVE_FIELD_PATH,
            RETRO_SAVE_VERDICT_LEFT_OUT,
            "SET_SAVE_INTERFACE, \"Ownership\"",
            "\"%s\": the folder holds \"%s\", which the frontend %s, "
            "and a file has one writer; exclude it", it->item.path, buf,
            what);
      it->left_out = true;
   }
   free(names);
   free(buf);
}

/* A frontend-owned item that resolved to no file: left out when its
 * name is too long; otherwise its directory or content is not known,
 * the user's setup rather than the core's mistake, so only logged. */
static void save_items_check_unnamed(save_items_check_t *c,
      save_items_set_t *set, const struct save_items_paths *paths)
{
   unsigned i;
   char *buf = NULL;
   for (i = 0; i < set->num_items; i++)
   {
      size_t len;
      save_item_t *it = &set->items[i];
      if (     it->left_out || it->path
            || (   it->item.storage != RETRO_SAVE_STORAGE_FRONTEND_BUFFER
                && it->item.storage != RETRO_SAVE_STORAGE_FRONTEND_BLOB))
         continue;
      if (!buf && !(buf = (char*)malloc(2 * PATH_MAX_LENGTH)))
         return;
      if ((len = save_items_name(buf, &it->item, paths)) >= PATH_MAX_LENGTH)
      {
         save_items_find(c, i, &it->item, RETRO_SAVE_FIELD_PATH,
               RETRO_SAVE_VERDICT_LEFT_OUT, NULL,
               "\"%s\": the file name it gives is %lu characters, over "
               "the frontend's limit of %u", it->item.path
               ? it->item.path : "", (unsigned long)len,
               (unsigned)PATH_MAX_LENGTH - 1);
         it->left_out = true;
         continue;
      }
      snprintf(buf, PATH_MAX_LENGTH, "%s: save item %u \"%s\" has no "
            "file: its directory or content is not known. It is not "
            "saved.", string_is_empty(c->core) ? "Core" : c->core, i,
            save_items_id(&it->item));
      if (!save_items_seen(&save_items_st.logged,
            &save_items_st.num_logged, buf))
         RARCH_WARN("[SRAM] %s\n", buf);
   }
   free(buf);
}

/* Drops "." components and doubled separators past a leading "//",
 * and a trailing separator, so one file has one spelling. */
static void save_items_tidy_path(char *s)
{
   char *in  = s;
   char *out = s;
   if (*in == '/' || *in == '\\')
   {
      *out++ = *in++;
      if (*in == '/' || *in == '\\')
         *out++ = *in++;
   }
   while (*in)
   {
      bool sep = out > s && (out[-1] == '/' || out[-1] == '\\');
      if (sep && (*in == '/' || *in == '\\'))
         in++;
      else if (sep && in[0] == '.' && (in[1] == '/' || in[1] == '\\'))
         in += 2;
      else if (sep && in[0] == '.' && !in[1])
         in++;
      else
         *out++ = *in++;
   }
   if (out > s + 1 && (out[-1] == '/' || out[-1] == '\\'))
      out--;
   *out = '\0';
}

static void save_items_resolve_set(save_items_set_t *set,
      const struct save_items_paths *paths)
{
   unsigned i;
   char *buf = (char*)malloc(2 * PATH_MAX_LENGTH);
   if (!buf)
      RARCH_ERR("[SRAM] No memory to name save items.\n");
   for (i = 0; i < set->num_items; i++)
   {
      size_t len;
      save_item_t *it = &set->items[i];
      free(it->path);
      it->path        = NULL;
      if (it->left_out)
         continue;
      /* A cut name is another file. */
      if (     buf
            && (len = save_items_name(buf, &it->item, paths)) > 0
            && len < PATH_MAX_LENGTH)
      {
         save_items_tidy_path(buf);
         if (!(it->path = strdup(buf)))
            RARCH_ERR("[SRAM] No memory for the path of \"%s\".\n",
                  it->item.id);
      }
      save_items_strv_free((const char *const *)it->legacy);
      it->legacy      = buf
         ? save_items_resolve_legacy(&it->item, paths, buf) : NULL;
   }
   free(buf);
}

void save_items_resolve(const struct save_items_paths *paths)
{
   if (save_items_st.current)
      save_items_resolve_set(save_items_st.current, paths);
}

static size_t RETRO_CALLCONV save_items_get_path(unsigned item,
      char *buf, size_t len)
{
   const save_items_set_t *set = save_items_st.current;
   const char *path;
   if (!set || item >= set->num_items || !(path = set->items[item].path))
      return 0;
   if (!buf || !len)
      return strlen(path);
   return strlcpy(buf, path, len);
}

static void RETRO_CALLCONV save_items_notify(unsigned event, unsigned item)
{
   (void)event;
   (void)item;
}

static unsigned RETRO_CALLCONV save_items_get_issues(
      const struct retro_save_issue **issues)
{
   if (issues)
      *issues = save_items_st.issues;
   return save_items_st.num_issues;
}

/* The secondary instance's findings are not listed. */
static unsigned RETRO_CALLCONV save_items_get_no_issues(
      const struct retro_save_issue **issues)
{
   if (issues)
      *issues = NULL;
   return 0;
}

bool save_items_get_frontend_interface(
      struct retro_save_frontend_interface *fi, unsigned instance)
{
   if (!fi)
      return true;
   if (fi->interface_version > SAVE_ITEMS_INTERFACE_VERSION)
      return false;
   fi->interface_version = SAVE_ITEMS_INTERFACE_VERSION;
   fi->instance          = instance;
   fi->storage_kinds    &= SAVE_ITEMS_STORAGE_KINDS;
   fi->item_flags       &= SAVE_ITEMS_ITEM_FLAGS;
   fi->features         &= SAVE_ITEMS_FEATURES;
   if (instance == RETRO_SAVE_INSTANCE_PRIMARY)
   {
      save_items_st.features = fi->features;
      save_items_st.asked    = true;
   }
   fi->get_path          = save_items_get_path;
   fi->notify            = save_items_notify;
   fi->get_issues        = instance == RETRO_SAVE_INSTANCE_PRIMARY
      ? save_items_get_issues : save_items_get_no_issues;
   return true;
}

static size_t save_items_buffer_size(const struct retro_save_item *item)
{
   unsigned k;
   size_t len = 0;
   for (k = 0; k < item->num_segments; k++)
      len += item->segments[k].size;
   return len;
}

/* Whether two items keep one file the same way: the same storage, the
 * same format when both are tagged and, for a buffer, the same size.
 * Blobs have no size here: blob_load() refuses what it cannot read. */
static bool save_items_same_type(const struct retro_save_item *a,
      const struct retro_save_item *b)
{
   if (a->storage != b->storage)
      return false;
   if (a->format && b->format && !string_is_equal(a->format, b->format))
      return false;
   return   a->storage != RETRO_SAVE_STORAGE_FRONTEND_BUFFER
         || save_items_buffer_size(a) == save_items_buffer_size(b);
}

/* A new id at the file of a replaced item whose id is gone, kept the
 * same way, is that save under another name (a Transfer Pak cart
 * moving port): linked as a kept id is, so its record carries over. */
static void save_items_match_moves(save_items_set_t *set,
      save_items_set_t *cur)
{
   unsigned i, j;
   for (i = 0; i < set->num_items; i++)
   {
      save_item_t *it = &set->items[i];
      if (it->prev != SAVE_ITEMS_NONE || !save_items_frontend_owned(it))
         continue;
      for (j = 0; j < cur->num_items; j++)
         if (     cur->items[j].next == SAVE_ITEMS_NONE
               && save_items_frontend_owned(&cur->items[j])
               && string_is_equal(cur->items[j].path, it->path)
               && save_items_same_type(&cur->items[j].item, &it->item))
         {
            it->prev                = j;
            cur->items[j].next      = i;
            it->tail                = cur->items[j].tail;
            it->tail_size           = cur->items[j].tail_size;
            cur->items[j].tail      = NULL;
            cur->items[j].tail_size = 0;
            break;
         }
   }
}

/* Neither format tag is the other with "+<memory>" parts appended. */
static bool save_items_tags_unrelated(const char *a, const char *b)
{
   size_t len;
   if (!a || !b)
      return false;
   if (strlen(a) > strlen(b))
   {
      const char *t = a;
      a             = b;
      b             = t;
   }
   len = strlen(a);
   return strncmp(a, b, len) || (b[len] && b[len] != '+');
}

/* Marks each frontend-owned item of @set, new or kept, whose file an
 * item of @cur other than its own kept another way, or whose own kept
 * id there changed its format other than by appending or cutting. */
static void save_items_mark_type_changes(save_items_check_t *c,
      save_items_set_t *set, const save_items_set_t *cur)
{
   unsigned i, j;
   for (i = 0; i < set->num_items; i++)
   {
      save_item_t *it = &set->items[i];
      if (!save_items_frontend_owned(it))
         continue;
      if (     it->prev != SAVE_ITEMS_NONE
            && string_is_equal(cur->items[it->prev].path, it->path)
            && save_items_tags_unrelated(cur->items[it->prev].item.format,
                  it->item.format))
      {
         it->type_changed = true;
         save_items_find(c, i, &it->item, RETRO_SAVE_FIELD_FORMAT,
               RETRO_SAVE_VERDICT_IGNORED,
               "SET_SAVE_INTERFACE, \"Layout changes\"",
               "\"%s\": it was \"%s\" at this file, and a layout changes "
               "only by a trailing \"+<memory>\"; a copy of the file is "
               "kept", it->item.format, cur->items[it->prev].item.format);
      }
      for (j = 0; j < cur->num_items; j++)
         if (     j != it->prev
               && cur->items[j].path
               && string_is_equal_case_insensitive(cur->items[j].path,
                     it->path)
               && !save_items_same_type(&cur->items[j].item, &it->item))
            it->type_changed = true;
   }
}

static void save_items_retire(unsigned memory_type)
{
   unsigned i;
   unsigned *list;
   if (memory_type == RETRO_SAVE_NO_LEGACY_MEMORY)
      return;
   for (i = 0; i < save_items_st.num_retired; i++)
      if (save_items_st.retired[i] == memory_type)
         return;
   if (!(list = (unsigned*)realloc(save_items_st.retired,
               (save_items_st.num_retired + 1) * sizeof(*list))))
   {
      RARCH_ERR("[SRAM] No memory to keep a save memory retired.\n");
      return;
   }
   list[save_items_st.num_retired++] = memory_type;
   save_items_st.retired             = list;
}

/* A refusal ends the current set as a replacement would: it becomes
 * previous, read during this call only, and its legacy memories stay
 * retired, so the legacy path does not write them again. Frees @set. */
static bool save_items_refuse(save_items_set_t *set)
{
   unsigned i;
   save_items_set_t *cur = save_items_st.current;
   save_items_set_free(set);
   for (i = 0; cur && i < cur->num_items; i++)
      save_items_retire(cur->items[i].item.legacy_memory);
   save_items_set_free(save_items_st.previous);
   save_items_st.previous = cur;
   save_items_st.current  = NULL;
   save_items_st.refused  = true;
   save_items_st.dropped  = cur != NULL;
   return false;
}

static size_t save_items_head_size(const struct retro_save_item *item)
{
   unsigned k;
   size_t len = 0;
   for (k = 0; k < item->num_segments; k++)
      len += item->segments[k].size;
   return len;
}

/* A re-registration that cuts a buffer keeps the cut bytes in front of
 * its tail. @old's segments stand only during this SET call. False
 * when there is no memory for them. */
static bool save_items_keep_cut(save_item_t *it, const save_item_t *old)
{
   unsigned k;
   unsigned char *tail;
   size_t off = 0;
   size_t own = save_items_head_size(&it->item);
   size_t had = save_items_head_size(&old->item);

   if (     it->item.storage  != RETRO_SAVE_STORAGE_FRONTEND_BUFFER
         || old->item.storage != RETRO_SAVE_STORAGE_FRONTEND_BUFFER
         || had <= own)
      return true;
   if (!(tail = (unsigned char*)malloc(had - own + it->tail_size)))
   {
      RARCH_ERR("[SRAM] No memory to keep what \"%s\" was cut by.\n",
            it->item.id);
      return false;
   }
   for (k = 0; k < old->item.num_segments; k++)
   {
      size_t end = off + old->item.segments[k].size;
      if (end > own)
      {
         size_t from = off > own ? off : own;
         memcpy(tail + (from - own),
               (const unsigned char*)old->item.segments[k].data
               + (from - off), end - from);
      }
      off = end;
   }
   if (it->tail_size)
      memcpy(tail + (had - own), it->tail, it->tail_size);
   free(it->tail);
   it->tail       = tail;
   it->tail_size += had - own;
   return true;
}

static const char *save_items_storage_name(unsigned storage)
{
   static const char *const names[] = {
      "core file", "core folder", "buffer", "blob", "in content",
      "external", "not persisted"
   };
   return storage <= RETRO_SAVE_STORAGE_NOT_PERSISTED
      ? names[storage] : "unknown storage";
}

/* What registered, for a new item or one whose storage, file or size
 * changed. */
static void save_items_summary(const char *core, const save_items_set_t *set,
      const save_items_set_t *cur)
{
   unsigned i;
   bool buffers = false;
   if (string_is_empty(core))
      core = "Core";
   for (i = 0; i < set->num_items; i++)
   {
      char size[64];
      char memory[32];
      char names[32];
      unsigned n                    = 0;
      const save_item_t *it         = &set->items[i];
      const struct retro_save_item *item = &it->item;
      const save_item_t *was        = (cur && it->prev != SAVE_ITEMS_NONE)
         ? &cur->items[it->prev] : NULL;
      if (it->left_out)
         continue;
      if (     was
            && was->item.storage == item->storage
            && string_is_equal(was->path, it->path)
            && save_items_buffer_size(&was->item)
               == save_items_buffer_size(item))
         continue;
      size[0] = memory[0] = names[0] = '\0';
      if (item->storage == RETRO_SAVE_STORAGE_FRONTEND_BUFFER)
         buffers = true;
      if (item->storage == RETRO_SAVE_STORAGE_FRONTEND_BUFFER)
         snprintf(size, sizeof(size), ", %lu bytes in %u segment%s",
               (unsigned long)save_items_buffer_size(item),
               item->num_segments, item->num_segments == 1 ? "" : "s");
      else if (item->storage == RETRO_SAVE_STORAGE_FRONTEND_BLOB)
      {
         if (item->max_size)
            snprintf(size, sizeof(size), ", up to %lu bytes",
                  (unsigned long)item->max_size);
         else
            strlcpy(size, ", size unknown", sizeof(size));
      }
      if (item->legacy_memory == RETRO_MEMORY_SAVE_RAM)
         strlcpy(memory, ", replaces SAVE_RAM", sizeof(memory));
      else if (item->legacy_memory == RETRO_MEMORY_RTC)
         strlcpy(memory, ", replaces RTC", sizeof(memory));
      else if (item->legacy_memory != RETRO_SAVE_NO_LEGACY_MEMORY)
         snprintf(memory, sizeof(memory), ", replaces memory %u",
               item->legacy_memory);
      while (it->legacy && it->legacy[n])
         n++;
      if (n)
         snprintf(names, sizeof(names), ", earlier names %u", n);
      RARCH_LOG("[SRAM] %s: save item %u \"%s\" (\"%s\"): %s%s, %s%s%s%s%s."
            "\n", core, i, item->id,
            item->label ? item->label : "", save_items_storage_name(
               item->storage), size, it->path ? "file \"" : "no file",
            it->path ? it->path : "", it->path ? "\"" : "", memory, names);
   }
   if (buffers)
   {
      char line[256];
      snprintf(line, sizeof(line), "%s: buffers are read after "
            "retro_unload_game returns, so a core frees them in "
            "retro_deinit. See libretro.h, SET_SAVE_INTERFACE, \"Ordering "
            "at unload\".", core);
      if (!save_items_seen(&save_items_st.logged,
            &save_items_st.num_logged, line))
         RARCH_LOG("[SRAM] %s\n", line);
   }
}

bool save_items_register(const struct retro_save_interface *iface,
      const struct save_items_paths *paths)
{
   unsigned i, j;
   save_items_check_t c;
   save_items_set_t *set;
   save_items_set_t *cur = save_items_st.current;

   memset(&c, 0, sizeof(c));
   c.core                = paths->core;
   save_items_st.refused = false;
   if (     !string_is_empty(paths->core)
         && !string_is_equal(paths->core, save_items_st.core))
   {
      free(save_items_st.core);
      save_items_st.core = strdup(paths->core);
   }
   save_items_st.dropped = false;
   if (!save_items_check_call(&c, iface))
   {
      save_items_check_end(&c);
      return save_items_refuse(NULL);
   }
   if (!(set = (save_items_set_t*)calloc(1, sizeof(*set))))
   {
      RARCH_ERR("[SRAM] No memory for save items.\n");
      save_items_check_end(&c);
      return save_items_refuse(NULL);
   }
   set->iface       = *iface;
   set->iface.items = NULL;
   if (iface->entries)
   {
      set->entries       = *iface->entries;
      set->iface.entries = &set->entries;
   }
   if (     iface->num_items
         && !(set->items = (save_item_t*)calloc(iface->num_items,
               sizeof(*set->items))))
   {
      RARCH_ERR("[SRAM] No memory for save items.\n");
      free(set);
      save_items_check_end(&c);
      return save_items_refuse(NULL);
   }
   set->num_items = iface->num_items;

   for (i = 0; i < set->num_items; i++)
   {
      struct retro_save_item src;
      save_items_read_item(&src, iface, i);
      set->items[i].prev = SAVE_ITEMS_NONE;
      set->items[i].next = SAVE_ITEMS_NONE;
      if (!save_items_copy_item(&set->items[i].item, &src))
      {
         RARCH_ERR("[SRAM] No memory for save items.\n");
         save_items_check_end(&c);
         return save_items_refuse(set);
      }
      save_items_check_item(&c, iface, set, i, &src, paths);
   }

   save_items_resolve_set(set, paths);
   save_items_check_unnamed(&c, set, paths);
   save_items_check_files(&c, set, cur, paths);
   save_items_check_legacy(&c, set, paths);
   save_items_check_frontend_files(&c, set, paths);
   for (i = 0; i < set->num_items; i++)
      if (!set->items[i].left_out && set->items[i].legacy_unreported)
      {
         set->legacy_names = save_items_legacy_names(paths);
         break;
      }
   if (c.refused)
   {
      save_items_check_end(&c);
      return save_items_refuse(set);
   }

   /* Described only: no file, and it retires nothing. */
   for (i = 0; i < set->num_items; i++)
      if (set->items[i].left_out)
      {
         save_item_t *it = &set->items[i];
         free(it->path);
         it->path               = NULL;
         save_items_strv_free((const char *const *)it->legacy);
         it->legacy             = NULL;
         it->item.legacy_memory = RETRO_SAVE_NO_LEGACY_MEMORY;
      }

   if (cur)
      for (i = 0; i < set->num_items; i++)
         for (j = 0; j < cur->num_items; j++)
            if (string_is_equal(set->items[i].item.id,
                     cur->items[j].item.id))
            {
               /* Closed as an item that left, and what it retired
                * stays retired. */
               if (set->items[i].left_out)
               {
                  save_items_retire(cur->items[j].item.legacy_memory);
                  break;
               }
               set->items[i].prev = j;
               cur->items[j].next = i;
               /* Only a buffer keeps a tail: a blob leaves it in the
                * replaced item's last image. */
               if (     string_is_equal(set->items[i].path,
                           cur->items[j].path)
                     && set->items[i].item.storage
                        == RETRO_SAVE_STORAGE_FRONTEND_BUFFER)
               {
                  set->items[i].tail      = cur->items[j].tail;
                  set->items[i].tail_size = cur->items[j].tail_size;
                  cur->items[j].tail      = NULL;
                  cur->items[j].tail_size = 0;
                  /* Without memory for the cut bytes the end stays
                   * with the replaced item: its last image is whole. */
                  if (!save_items_keep_cut(&set->items[i],
                           &cur->items[j]))
                  {
                     cur->items[j].tail      = set->items[i].tail;
                     cur->items[j].tail_size = set->items[i].tail_size;
                     set->items[i].tail      = NULL;
                     set->items[i].tail_size = 0;
                  }
               }
               break;
            }

   if (cur)
   {
      save_items_match_moves(set, cur);
      save_items_mark_type_changes(&c, set, cur);
   }

   save_items_check_end(&c);
   save_items_issues_reserve(set);
   save_items_summary(paths->core, set, cur);
   save_items_set_free(save_items_st.previous);
   save_items_st.previous = cur;
   save_items_st.current  = set;
   return true;
}

bool save_items_check_reported(unsigned i, const bool *live)
{
   unsigned mem;
   char *buf;
   save_items_check_t c;
   save_item_t *it;
   save_items_set_t *set = save_items_st.current;
   if (!set || i >= set->num_items)
      return false;
   it = &set->items[i];
   if (it->left_out || !it->legacy_unreported || !set->legacy_names)
   {
      it->legacy_unreported = 0;
      return false;
   }
   /* The finding takes room kept after the call's, so the list a core
    * holds does not move. */
   memset(&c, 0, sizeof(c));
   c.core       = save_items_st.core;
   c.issues     = save_items_st.issues;
   c.num_issues = save_items_st.num_issues;
   c.room       = save_items_st.issues_room;
   c.fixed      = true;
   buf          = (char*)malloc(3 * PATH_MAX_LENGTH);
   for (mem = 0; mem < 2 && !it->left_out; mem++)
      if (     (it->legacy_unreported & (1 << mem)) && live[mem]
            && !save_items_memory_retired(set, mem))
      {
         const char *name = set->legacy_names + mem * PATH_MAX_LENGTH;
         if (     it->item.storage == RETRO_SAVE_STORAGE_CORE_FOLDER && buf
               && save_items_holds_legacy(it, name, buf))
            name = buf;
         save_items_leave_legacy(&c, set, i, mem, name);
      }
   free(buf);
   it->legacy_unreported     = 0;
   save_items_st.num_issues  = c.num_issues;
   save_items_st.issues_room = c.room;
   if (!it->left_out)
      return false;
   free(it->path);
   it->path               = NULL;
   save_items_strv_free((const char *const *)it->legacy);
   it->legacy             = NULL;
   it->item.legacy_memory = RETRO_SAVE_NO_LEGACY_MEMORY;
   return true;
}

bool save_items_check(const struct retro_save_interface *iface,
      const char *core)
{
   unsigned i;
   save_items_check_t c;
   memset(&c, 0, sizeof(c));
   c.core = core;
   if (save_items_check_call(&c, iface))
      for (i = 0; i < iface->num_items; i++)
      {
         struct retro_save_item src;
         save_items_read_item(&src, iface, i);
         save_items_check_id(&c, iface, i, &src);
      }
   for (i = 0; i < c.num_issues; i++)
      free((char*)c.issues[i].text);
   free(c.issues);
   return !c.refused;
}

void save_items_drop_previous(void)
{
   save_items_set_free(save_items_st.previous);
   save_items_st.previous = NULL;
   save_items_st.dropped  = false;
}

bool save_items_legacy_retired(unsigned memory_type)
{
   unsigned i;
   const save_items_set_t *set = save_items_st.current;
   if (memory_type == RETRO_SAVE_NO_LEGACY_MEMORY)
      return false;
   for (i = 0; set && i < set->num_items; i++)
      if (set->items[i].item.legacy_memory == memory_type)
         return true;
   for (i = 0; i < save_items_st.num_retired; i++)
      if (save_items_st.retired[i] == memory_type)
         return true;
   return false;
}

bool save_items_frontend_owned(const save_item_t *it)
{
   return it->path
      && (   it->item.storage == RETRO_SAVE_STORAGE_FRONTEND_BUFFER
          || it->item.storage == RETRO_SAVE_STORAGE_FRONTEND_BLOB);
}

bool save_items_can_legacy_load(void)
{
   return save_items_st.current
      && save_items_st.current->iface.legacy_load
      && (save_items_st.features & RETRO_SAVE_FEATURE_LEGACY_LOAD) != 0;
}

bool save_items_take_refusal(void)
{
   if (!save_items_st.refused || save_items_st.refusal_shown)
      return false;
   save_items_st.refusal_shown = true;
   return true;
}

void save_items_check_caller(const char *core, bool other_thread)
{
   if (string_is_empty(core))
      core = "Core";
   if (!save_items_st.asked && !save_items_st.unasked_told)
   {
      save_items_st.unasked_told = true;
      RARCH_WARN("[SRAM] %s: SET_SAVE_INTERFACE came before "
            "GET_SAVE_FRONTEND_INTERFACE, which a core calls first to "
            "learn what this frontend takes. See libretro.h, "
            "GET_SAVE_FRONTEND_INTERFACE.\n", core);
   }
   if (other_thread && !save_items_st.thread_told)
   {
      save_items_st.thread_told = true;
      RARCH_WARN("[SRAM] %s: SET_SAVE_INTERFACE came from a thread other "
            "than the core's. See libretro.h, SET_SAVE_INTERFACE, "
            "\"Threads\".\n", core);
   }
}

void save_items_who(char *s, size_t len, const save_items_set_t *set,
      unsigned i)
{
   const struct retro_save_item *item = &set->items[i].item;
   const char *core = string_is_empty(save_items_st.core)
      ? "Core" : save_items_st.core;
   if (item->label)
      snprintf(s, len, "%s: save item %u \"%s\" (\"%s\")", core, i,
            save_items_id(item), item->label);
   else
      snprintf(s, len, "%s: save item %u \"%s\"", core, i,
            save_items_id(item));
}

const char *save_items_take_left_out(void)
{
   unsigned i;
   const save_items_set_t *set = save_items_st.current;
   for (i = 0; set && i < set->num_items; i++)
   {
      const struct retro_save_item *item = &set->items[i].item;
      if (     set->items[i].left_out
            && save_items_frontend_writes(item->storage)
            && !save_items_seen(&save_items_st.told, &save_items_st.num_told,
               item->id))
         return item->label ? item->label : item->id;
   }
   return NULL;
}

bool save_items_can_register(void)
{
   save_items_check_t c;
   unsigned room;
   struct retro_save_issue *grown;
   if (!save_items_st.callback)
      return true;
   /* The set registered stays: so does the room its first fill may
    * take. */
   room   = save_items_st.issues_room;
   memset(&c, 0, sizeof(c));
   c.core = save_items_st.core;
   save_items_find(&c, RETRO_SAVE_ISSUE_NO_ITEM, NULL,
         RETRO_SAVE_FIELD_NONE, RETRO_SAVE_VERDICT_REFUSED,
         "SET_SAVE_INTERFACE", "made from inside the core's %s(), which "
         "reads the current set; a core makes it from retro_run()",
         save_items_st.callback);
   save_items_check_end(&c);
   if (     room
         && (grown = (struct retro_save_issue*)realloc(save_items_st.issues,
            (save_items_st.num_issues + room) * sizeof(*grown))))
   {
      save_items_st.issues      = grown;
      save_items_st.issues_room = room;
   }
   return false;
}

void save_items_clear(void)
{
   save_items_drop_previous();
   save_items_set_free(save_items_st.current);
   save_items_st.current       = NULL;
   free(save_items_st.retired);
   save_items_st.retired       = NULL;
   save_items_st.num_retired   = 0;
   save_items_st.features      = 0;
   save_items_st.refused       = false;
   save_items_st.dropped       = false;
   save_items_st.refusal_shown = false;
   save_items_st.asked         = false;
   save_items_st.unasked_told  = false;
   save_items_st.thread_told   = false;
   free(save_items_st.core);
   save_items_st.core          = NULL;
   save_items_issues_free();
   save_items_strs_free(&save_items_st.logged, &save_items_st.num_logged);
   save_items_strs_free(&save_items_st.told, &save_items_st.num_told);
}

size_t save_items_blob_size(const save_items_set_t *set, unsigned i)
{
   size_t len;
   const char *was        = save_items_st.callback;
   save_items_st.callback = "blob_size";
   len                    = set->iface.blob_size(i);
   save_items_st.callback = was;
   return len;
}

static bool save_items_blob_save(const save_items_set_t *set, unsigned i,
      void *buf, size_t len)
{
   bool ok;
   const char *was        = save_items_st.callback;
   save_items_st.callback = "blob_save";
   ok                     = set->iface.blob_save(i, buf, len);
   save_items_st.callback = was;
   return ok;
}

uint64_t save_items_generation(const save_items_set_t *set, unsigned i)
{
   uint64_t gen;
   const char *was;
   if (!set->iface.get_generation)
      return 0;
   was                    = save_items_st.callback;
   save_items_st.callback = "get_generation";
   gen                    = set->iface.get_generation(i);
   save_items_st.callback = was;
   return gen;
}

bool save_items_can_snapshot(const save_items_set_t *set, unsigned i)
{
   bool ready;
   const char *was;
   if (!set->iface.can_snapshot)
      return true;
   was                    = save_items_st.callback;
   save_items_st.callback = "can_snapshot";
   ready                  = set->iface.can_snapshot(i);
   save_items_st.callback = was;
   return ready;
}

size_t save_items_size(const save_items_set_t *set, unsigned i)
{
   size_t len;
   const save_item_t *it = &set->items[i];
   if (it->item.storage == RETRO_SAVE_STORAGE_FRONTEND_BLOB)
   {
      len = save_items_blob_size(set, i);
      return (it->item.max_size && len > it->item.max_size) ? 0 : len;
   }
   return save_items_head_size(&it->item) + it->tail_size;
}

bool save_items_copy_out_head(const save_items_set_t *set, unsigned i,
      void *buf, size_t len)
{
   unsigned k;
   size_t off                         = 0;
   const struct retro_save_item *item = &set->items[i].item;
   if (item->storage == RETRO_SAVE_STORAGE_FRONTEND_BLOB)
      return save_items_blob_save(set, i, buf, len);
   for (k = 0; k < item->num_segments; k++)
   {
      size_t n = item->segments[k].size;
      if (n > len - off)
         return false;
      if (n)
         memcpy((unsigned char*)buf + off, item->segments[k].data, n);
      off += n;
   }
   return off == len;
}

bool save_items_copy_out(const save_items_set_t *set, unsigned i,
      void *buf, size_t len)
{
   const save_item_t *it = &set->items[i];
   if (it->item.storage == RETRO_SAVE_STORAGE_FRONTEND_BLOB)
      return save_items_blob_save(set, i, buf, len);
   if (     len < it->tail_size
         || !save_items_copy_out_head(set, i, buf, len - it->tail_size))
      return false;
   if (it->tail_size)
      memcpy((unsigned char*)buf + len - it->tail_size, it->tail,
            it->tail_size);
   return true;
}

bool save_items_copy_in(const save_items_set_t *set, unsigned i,
      const void *buf, size_t len)
{
   unsigned k;
   size_t off                         = 0;
   const struct retro_save_item *item = &set->items[i].item;
   if (item->storage == RETRO_SAVE_STORAGE_FRONTEND_BLOB)
      return set->iface.blob_load(i, buf, len);
   for (k = 0; k < item->num_segments && off < len; k++)
   {
      size_t n = item->segments[k].size;
      if (n > len - off)
         n = len - off;
      if (n)
         memcpy(item->segments[k].data,
               (const unsigned char*)buf + off, n);
      off += n;
   }
   return true;
}

void save_items_keep_tail(unsigned i, const void *buf, size_t len)
{
   size_t own;
   save_item_t *it;
   save_items_set_t *set = save_items_st.current;
   if (!set || i >= set->num_items)
      return;
   it = &set->items[i];
   save_items_drop_tail(i);
   if (it->item.storage != RETRO_SAVE_STORAGE_FRONTEND_BUFFER)
      return;
   own = save_items_head_size(&it->item);
   if (len <= own)
      return;
   if (!(it->tail = malloc(len - own)))
   {
      RARCH_ERR("[SRAM] No memory to keep the end of \"%s\".\n", it->path);
      return;
   }
   memcpy(it->tail, (const unsigned char*)buf + own, len - own);
   it->tail_size = len - own;
}

void save_items_drop_tail(unsigned i)
{
   save_items_set_t *set = save_items_st.current;
   if (!set || i >= set->num_items)
      return;
   free(set->items[i].tail);
   set->items[i].tail      = NULL;
   set->items[i].tail_size = 0;
}

void save_items_cut_tail(unsigned i, size_t at, size_t len)
{
   save_item_t *it;
   save_items_set_t *set = save_items_st.current;
   if (!set || i >= set->num_items)
      return;
   it = &set->items[i];
   if (at >= it->tail_size)
      return;
   if (len > it->tail_size - at)
      len = it->tail_size - at;
   memmove((unsigned char*)it->tail + at,
         (const unsigned char*)it->tail + at + len,
         it->tail_size - at - len);
   if (!(it->tail_size -= len))
      save_items_drop_tail(i);
}

size_t save_items_grow_from_tail(unsigned i, size_t from)
{
   unsigned k;
   size_t n;
   size_t off            = 0;
   save_items_set_t *set = save_items_st.current;
   save_item_t *it;
   if (!set || i >= set->num_items || !set->items[i].tail_size)
      return 0;
   it = &set->items[i];
   n  = save_items_head_size(&it->item);
   if (n <= from)
      return 0;
   n -= from;
   if (n > it->tail_size)
      n = it->tail_size;
   for (k = 0; k < it->item.num_segments; k++)
   {
      size_t end = off + it->item.segments[k].size;
      size_t s   = off > from ? off : from;
      size_t e   = end < from + n ? end : from + n;
      if (s < e)
         memcpy((unsigned char*)it->item.segments[k].data + (s - off),
               (const unsigned char*)it->tail + (s - from), e - s);
      off = end;
   }
   if (n == it->tail_size)
      save_items_drop_tail(i);
   else
   {
      memmove(it->tail, (const unsigned char*)it->tail + n,
            it->tail_size - n);
      it->tail_size -= n;
   }
   return n;
}

bool save_items_path_is_live(const char *path)
{
   unsigned i;
   char *buf                   = NULL;
   bool live                   = false;
   const save_items_set_t *set = save_items_st.current;
   if (!set || string_is_empty(path))
      return false;
   for (i = 0; !live && i < set->num_items; i++)
   {
      size_t len;
      const save_item_t *it = &set->items[i];
      if (!it->path)
         continue;
      switch (it->item.storage)
      {
         case RETRO_SAVE_STORAGE_FRONTEND_BUFFER:
         case RETRO_SAVE_STORAGE_FRONTEND_BLOB:
         case RETRO_SAVE_STORAGE_CORE_FILE:
            live = string_is_equal_case_insensitive(it->path, path);
            break;
         case RETRO_SAVE_STORAGE_CORE_FOLDER:
            /* The folder, and the files in it its patterns select;
             * without memory to match, every file. */
            len = strlen(it->path);
            if (     !len
                  || !string_starts_with_case_insensitive(path, it->path))
               break;
            if (!path[len])
               live = true;
            else if (PATH_CHAR_IS_SLASH(path[len]))
            {
               if (!buf)
                  buf = (char*)malloc(2 * PATH_MAX_LENGTH);
               live = save_items_selects(it, path + len + 1, buf);
            }
            else if (PATH_CHAR_IS_SLASH(it->path[len - 1]))
            {
               if (!buf)
                  buf = (char*)malloc(2 * PATH_MAX_LENGTH);
               live = save_items_selects(it, path + len, buf);
            }
            break;
         default:
            break;
      }
   }
   free(buf);
   return live;
}

size_t save_items_state_undo_dir(char *s, size_t len,
      const char *core_assets, const char *library_name,
      const char *content)
{
   if (     string_is_empty(core_assets)
         || string_is_empty(library_name)
         || string_is_empty(content)
         || !*path_basename(content))
      return 0;
   fill_pathname_join_special(s, core_assets, "state_undo", len);
   fill_pathname_join(s, s, library_name, len);
   return fill_pathname_join(s, s, path_basename(content), len);
}
