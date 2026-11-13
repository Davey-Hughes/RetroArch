/* Harness for the Google Drive driver's uploads under non-destructive
 * sync.
 *
 * With destructive sync off, a Google Drive delete keeps the server's
 * copy, renamed in place to <name>-<yymmdd-hhmmss>, but an upload
 * replaced the file's content outright: the setting protected against
 * deletes and not against overwrites.  An upload to an existing file
 * now first asks Drive to copy it (files.copy) to that name in the
 * oldest of its same-named folders, then PATCHes the new content.  The
 * test plays the Drive API and checks, request by request:
 *
 * - the copy comes before the PATCH, is made from the right file, into
 *   the oldest of its same-named folders, under <name>-<yymmdd-hhmmss>;
 * - a failed copy stops the upload, reported once, with no PATCH;
 * - a 401 on the copy refreshes the token and goes round again, and the
 *   PATCH after a successful copy still has its own 401 retry;
 * - destructive sync, a file that does not exist yet (created, nothing
 *   to keep) and the server manifest skip the copy.
 *
 * It also checks lookups when a folder name appears twice (#19244).
 *
 * Includes the driver's translation unit so it is tested as shipped.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

#include <boolean.h>
#include <streams/file_stream.h>

#include "../../../network/cloud_sync/google_drive.c"

#include "stubs_retroarch.h"

#define MAX_STEPS 24

static unsigned failures = 0;

#define CHECK(cond, ...) \
   do { \
      if (!(cond)) \
      { \
         printf("FAIL %s:%d: ", __FILE__, __LINE__); \
         printf(__VA_ARGS__); \
         printf("\n"); \
         failures++; \
      } \
   } while (0)

typedef enum { REQ_OTHER, REQ_SEARCH, REQ_COPY, REQ_PATCH, REQ_CREATE, REQ_TOKEN } req_kind_t;

typedef struct
{
   bool exists;          /* search finds the file */
   int  copy_status[4];  /* consumed in order; 0 = 200 */
   int  patch_status[4];
} script_t;

typedef struct
{
   stub_request_t req[MAX_STEPS];
   req_kind_t     kind[MAX_STEPS];
   unsigned       steps;
   unsigned       calls;
   bool           success;
} run_t;

static void on_done(void *user_data, const char *path, bool success,
      RFILE *file)
{
   run_t *run = (run_t*)user_data;
   (void)path; (void)file;
   run->calls++;
   run->success = success;
}

static req_kind_t classify(const stub_request_t *req)
{
   if (strstr(req->url, "oauth2.googleapis.com/token"))
      return REQ_TOKEN;
   if (strstr(req->url, "/files?q="))
      return REQ_SEARCH;
   if (strstr(req->url, "/copy"))
      return REQ_COPY;
   if (!strcmp(req->method, "PATCH") && strstr(req->url, "uploadType=media"))
      return REQ_PATCH;
   if (!strcmp(req->method, "POST") && !strcmp(req->url, GDRIVE_API_BASE "/files"))
      return REQ_CREATE;
   return REQ_OTHER;
}

static void run_update(const char *path, bool destructive,
      const script_t *script, run_t *run, const char *tmp)
{
   static char found[]   = "{\"files\":[{\"id\":\"FILE1\"}]}";
   static char none[]    = "{\"files\":[]}";
   static char copied[]  = "{\"id\":\"COPY1\"}";
   static char created[] = "{\"id\":\"NEW1\"}";
   static char token[]   = "{\"access_token\":\"token-2\",\"expires_in\":3599}";
   static char empty[]   = "{}";
   unsigned ncopy = 0, npatch = 0;
   RFILE *rfile;

   memset(run, 0, sizeof(*run));
   config_get_ptr()->bools.cloud_sync_destructive = destructive;
   strlcpy(gdrive_st.access_token, "token-1", sizeof(gdrive_st.access_token));
   stub_has_pending        = false;
   stub_pending.overlapped = false;

   rfile = filestream_open(tmp, RETRO_VFS_FILE_ACCESS_READ,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);
   CHECK(gdrive_update(path, rfile, on_done, run), "%s: update must start", path);

   while (stub_has_pending && run->steps < MAX_STEPS)
   {
      http_transfer_data_t resp;
      stub_request_t req = stub_pending;
      req_kind_t kind    = classify(&req);

      stub_has_pending = false;
      run->kind[run->steps]  = kind;
      run->req[run->steps++] = req;

      memset(&resp, 0, sizeof(resp));
      resp.status = 200;
      resp.data   = empty;
      switch (kind)
      {
         case REQ_SEARCH: resp.data = script->exists ? found : none; break;
         case REQ_COPY:
            if (ncopy < 4 && script->copy_status[ncopy])
               resp.status = script->copy_status[ncopy];
            ncopy++;
            resp.data = copied;
            break;
         case REQ_PATCH:
            if (npatch < 4 && script->patch_status[npatch])
               resp.status = script->patch_status[npatch];
            npatch++;
            break;
         case REQ_CREATE: resp.data = created; break;
         case REQ_TOKEN:  resp.data = token;   break;
         default: break;
      }
      resp.len = strlen(resp.data);
      req.cb(NULL, &resp, req.user_data, NULL);
   }

   CHECK(!stub_pending.overlapped, "%s: requests overlapped", path);
   filestream_close(rfile);
}

static int find(const run_t *run, req_kind_t kind, unsigned from)
{
   unsigned i;
   for (i = from; i < run->steps; i++)
      if (run->kind[i] == kind)
         return (int)i;
   return -1;
}

static unsigned count(const run_t *run, req_kind_t kind)
{
   unsigned i, n = 0;
   for (i = 0; i < run->steps; i++)
      n += (run->kind[i] == kind);
   return n;
}

/* {"name":"a.srm-yymmdd-hhmmss","parents":["F_SAVES"]} */
static bool copy_body_ok(const char *body)
{
   static const char want[] = "{\"name\":\"a.srm-";
   const char *p = body + sizeof(want) - 1;
   int i;
   if (strncmp(body, want, sizeof(want) - 1))
      return false;
   for (i = 0; i < 13; i++)
      if (i == 6 ? p[i] != '-' : !isdigit((unsigned char)p[i]))
         return false;
   return !strcmp(p + 13, "\",\"parents\":[\"F_SAVES\"]}");
}

static void test_copy_then_patch(const char *tmp)
{
   script_t script = {true, {0}, {0}};
   run_t    run;
   int      copy, patch;

   run_update("saves/a.srm", false, &script, &run, tmp);
   copy  = find(&run, REQ_COPY, 0);
   patch = find(&run, REQ_PATCH, 0);

   CHECK(copy >= 0, "non-destructive upload must copy the existing file first");
   CHECK(patch >= 0, "non-destructive upload must still upload");
   CHECK(copy < 0 || patch < 0 || copy < patch, "the copy must come before the PATCH");
   if (copy >= 0)
   {
      CHECK(!strcmp(run.req[copy].url, GDRIVE_API_BASE "/files/FILE1/copy"),
            "the copy must be made from the file being replaced, got %s",
            run.req[copy].url);
      CHECK(copy_body_ok(run.req[copy].body),
            "the copy must be named a.srm-<yymmdd-hhmmss> in the same folder, got %s",
            run.req[copy].body);
   }
   CHECK(run.calls == 1 && run.success, "non-destructive upload must succeed once");
}

static void test_copy_fails(const char *tmp)
{
   script_t script = {true, {500}, {0}};
   run_t    run;

   run_update("saves/a.srm", false, &script, &run, tmp);
   CHECK(!count(&run, REQ_PATCH), "a failed copy must stop the upload");
   CHECK(run.calls == 1 && !run.success, "a failed copy must report failure once");
}

static void test_token_refresh(const char *tmp)
{
   script_t script = {true, {401}, {401}};
   run_t    run;

   run_update("saves/a.srm", false, &script, &run, tmp);
   CHECK(count(&run, REQ_TOKEN) == 2,
         "a 401 on the copy and a 401 on the PATCH must each refresh the token, saw %u",
         count(&run, REQ_TOKEN));
   CHECK(count(&run, REQ_COPY) == 2 && count(&run, REQ_PATCH) == 2,
         "copy and PATCH must each be retried once after the refresh");
   CHECK(run.calls == 1 && run.success, "one 401 each must still succeed, once");
}

static void test_no_backup_wanted(const char *tmp)
{
   script_t exists  = {true, {0}, {0}};
   script_t missing = {false, {0}, {0}};
   run_t    run;

   run_update("saves/a.srm", true, &exists, &run, tmp);
   CHECK(!count(&run, REQ_COPY) && count(&run, REQ_PATCH) == 1
         && run.calls == 1 && run.success,
         "destructive upload must PATCH without a copy");

   run_update("saves/a.srm", false, &missing, &run, tmp);
   CHECK(!count(&run, REQ_COPY) && count(&run, REQ_CREATE) == 1
         && count(&run, REQ_PATCH) == 1 && run.calls == 1 && run.success,
         "a file that does not exist yet has nothing to keep");

   run_update(CLOUD_SYNC_SERVER_MANIFEST, false, &exists, &run, tmp);
   CHECK(!count(&run, REQ_COPY) && count(&run, REQ_PATCH) == 1
         && run.calls == 1 && run.success,
         "the server manifest must be replaced without a copy");
}

/* ---- OAuth device flow ---- */

static unsigned oauth_calls;
static bool     oauth_success;

static void oauth_done(void *user_data, const char *path, bool success,
      RFILE *file)
{
   (void)user_data; (void)path; (void)file;
   oauth_calls++;
   oauth_success = success;
}

static void oauth_reply(const char *json)
{
   static char body[256];
   http_transfer_data_t resp;
   stub_request_t req = stub_pending;

   stub_has_pending = false;
   strlcpy(body, json, sizeof(body));
   memset(&resp, 0, sizeof(resp));
   resp.status = 200;
   resp.data   = body;
   resp.len    = strlen(body);
   req.cb(NULL, &resp, req.user_data, NULL);
}

/* While a token poll is in flight the polling task parks, and does not
 * end - cancelled or not - since the poll's callback writes into its
 * context.  A pending reply leaves it to its interval; an outcome wakes
 * it, and a cancel with nothing in flight ends it. */
static void test_oauth_poll(void)
{
   retro_task_t        task;
   gdrive_oauth_ctx_t *ctx = (gdrive_oauth_ctx_t*)calloc(1, sizeof(*ctx));
   unsigned            had = failures;

   printf("[oauth poll]\n");
   memset(&task, 0, sizeof(task));
   ctx->cb         = oauth_done;
   ctx->interval   = 5;
   ctx->max_polls  = 10;
   ctx->task_ident = 42;
   retro_atomic_int_init(&ctx->poll_pending, 0);
   retro_atomic_int_init(&ctx->auth_done, 0);
   task.user_data  = ctx;
   task.ident      = 42;
   oauth_calls     = 0;
   stub_parks      = 0;
   stub_wakes      = 0;
   stub_has_pending = false;

   gdrive_oauth_task_handler(&task);
   CHECK(stub_has_pending && classify(&stub_pending) == REQ_TOKEN,
         "the first pass posts a token poll");
   CHECK(task.when > 0, "the next poll is an interval away");

   gdrive_oauth_task_handler(&task);
   CHECK(stub_parks == 1, "a pass with the poll in flight parks");
   CHECK(!(task.flags & RETRO_TASK_FLG_FINISHED), "and does not finish");

   task.flags |= RETRO_TASK_FLG_CANCELLED;
   gdrive_oauth_task_handler(&task);
   CHECK(!(task.flags & RETRO_TASK_FLG_FINISHED) && oauth_calls == 0,
         "a cancel waits for the poll in flight");

   oauth_reply("{\"error\":\"authorization_pending\"}");
   CHECK(stub_wakes == 0, "a pending reply does not wake the task");

   gdrive_oauth_task_handler(&task);
   CHECK((task.flags & RETRO_TASK_FLG_FINISHED) && oauth_calls == 1
         && !oauth_success, "the cancel ends it once the poll is in");
   CHECK(!stub_has_pending, "and no further poll is posted");

   /* An outcome wakes it. */
   memset(&task, 0, sizeof(task));
   retro_atomic_int_init(&ctx->poll_pending, 0);
   retro_atomic_int_init(&ctx->auth_done, 0);
   ctx->polls     = 0;
   task.user_data = ctx;
   task.ident     = 42;
   oauth_calls    = 0;
   gdrive_oauth_task_handler(&task);
   oauth_reply("{\"error\":\"access_denied\"}");
   CHECK(stub_wakes == 1 && stub_woken_ident == 42,
         "a final reply wakes the polling task");
   gdrive_oauth_task_handler(&task);
   CHECK((task.flags & RETRO_TASK_FLG_FINISHED) && oauth_calls == 1
         && !oauth_success, "which ends with the failure");

   free(ctx);
   if (failures == had)
      printf("  ok   oauth poll\n");
}

/* ---- Duplicate folders (#19244) ----
 *
 * Two clients creating the same folder at once leave two folders of
 * that name, each with the files its creator uploaded.  Lookups must
 * search every same-named folder, and new files and folders go in the
 * oldest, so no file hides and racing clients converge.  A name in
 * several of them is the copy modified last.  The fake Drive lists
 * newest first unless asked to order by createdTime or modifiedTime
 * desc, and honours pageSize. */

typedef struct
{
   const char *id;
   const char *name;
   const char *parent; /* NULL: top level */
   unsigned    created;
   unsigned    modified;
   bool        folder;
} drive_item_t;

/* Newest first.  s.srm is in three duplicates: the newest copy
 * (F_S2), the oldest (F_S1) and the one modified last (F_S3) are
 * three different files. */
static const drive_item_t drive_items[] = {
   { "F_S2",  "s.srm",     "D_R",   15, 17, false },
   { "F_S3",  "s.srm",     "D_NEW", 14, 21, false },
   { "F_S1",  "s.srm",     "D_OLD", 13, 16, false },
   { "F_C",   "c.srm",     "S_SUB", 12, 12, false },
   { "S_SUB", "sub",       "D_NEW", 11, 11, true  },
   { "F_B",   "b.srm",     "D_NEW", 10, 10, false },
   { "F_A",   "a.srm",     "D_OLD",  9,  9, false },
   { "F_R",   "r.srm",     "R_NEW",  8,  8, false },
   { "F_O",   "o.srm",     "R_OLD",  7,  7, false },
   { "D_NEW", "dup",       "R_OLD",  6,  6, true  },
   { "D_R",   "dup",       "R_NEW",  5,  5, true  },
   { "D_OLD", "dup",       "R_OLD",  4,  4, true  },
   { "R_NEW", "RetroArch", NULL,     2,  2, true  },
   { "R_OLD", "RetroArch", NULL,     1,  1, true  }
};

#define DRIVE_ITEMS (sizeof(drive_items) / sizeof(drive_items[0]))

static void drive_list(const char *url, char *out, size_t out_size)
{
   const drive_item_t *hits[DRIVE_ITEMS];
   const char *p     = strstr(url, "name='");
   bool folders_only = strstr(url, "mimeType=") != NULL;
   bool any_parent   = strstr(url, "+in+parents") == NULL;
   unsigned page     = 100;
   unsigned n        = 0;
   unsigned i, j;
   size_t _len;
   char name[64];

   name[0] = '\0';
   if (p)
   {
      p += 6;
      for (i = 0; p[i] && p[i] != '\'' && i + 1 < sizeof(name); i++)
         name[i] = p[i];
      name[i] = '\0';
   }
   if ((p = strstr(url, "pageSize=")))
      page = (unsigned)atoi(p + 9);

   for (i = 0; i < DRIVE_ITEMS; i++)
   {
      char clause[64];
      const drive_item_t *it = &drive_items[i];
      if (strcmp(it->name, name) || (folders_only && !it->folder))
         continue;
      if (!any_parent)
      {
         if (!it->parent)
            continue;
         snprintf(clause, sizeof(clause), "'%s'+in+parents", it->parent);
         if (!strstr(url, clause))
            continue;
      }
      hits[n++] = it;
   }

   if (strstr(url, "orderBy=createdTime"))
      for (i = 1; i < n; i++)
         for (j = i; j > 0 && hits[j - 1]->created > hits[j]->created; j--)
         {
            const drive_item_t *t = hits[j];
            hits[j]     = hits[j - 1];
            hits[j - 1] = t;
         }
   else if (strstr(url, "orderBy=modifiedTime+desc"))
      for (i = 1; i < n; i++)
         for (j = i; j > 0 && hits[j - 1]->modified < hits[j]->modified; j--)
         {
            const drive_item_t *t = hits[j];
            hits[j]     = hits[j - 1];
            hits[j - 1] = t;
         }

   _len = strlcpy(out, "{\"files\":[", out_size);
   for (i = 0; i < n && i < page && _len < out_size; i++)
      _len += snprintf(out + _len, out_size - _len, "%s{\"id\":\"%s\"}",
            i ? "," : "", hits[i]->id);
   strlcat(out, "]}", out_size);
}

static stub_request_t dup_reqs[16];
static unsigned       dup_steps;

static void drive_serve(void)
{
   static char listing[512];
   static char created[] = "{\"id\":\"NEW_ID\"}";
   static char empty[]   = "{}";

   dup_steps = 0;
   while (stub_has_pending && dup_steps < 16)
   {
      http_transfer_data_t resp;
      stub_request_t req = stub_pending;

      stub_has_pending      = false;
      dup_reqs[dup_steps++] = req;
      memset(&resp, 0, sizeof(resp));
      resp.status = 200;
      resp.data   = empty;
      if (strstr(req.url, "/files?q="))
      {
         drive_list(req.url, listing, sizeof(listing));
         resp.data = listing;
      }
      /* The lanes check only which file a read fetches. */
      else if (strstr(req.url, "?alt=media"))
         resp.status = 404;
      else if (!strcmp(req.method, "POST")
            && !strcmp(req.url, GDRIVE_API_BASE "/files"))
         resp.data = created;
      resp.len = strlen(resp.data);
      req.cb(NULL, &resp, req.user_data, NULL);
   }
   CHECK(!stub_has_pending, "the driver kept asking");
}

/* The ID a read of path downloads; "" when it fetches nothing. */
static const char *dup_fetched(const char *path, const char *tmp)
{
   static char id[64];
   run_t    run;
   unsigned i;

   memset(&run, 0, sizeof(run));
   id[0] = '\0';
   CHECK(gdrive_read(path, tmp, on_done, &run), "%s: read must start", path);
   drive_serve();
   CHECK(run.calls == 1, "%s: read must finish once", path);
   for (i = 0; i < dup_steps; i++)
   {
      const char *p = strstr(dup_reqs[i].url, "/files/");
      if (p && strstr(p, "?alt=media"))
      {
         size_t _len = strcspn(p + 7, "?");
         strlcpy(id, p + 7, _len + 1 < sizeof(id) ? _len + 1 : sizeof(id));
      }
   }
   return id;
}

/* The last request with that method whose URL holds part; n counts
 * them. */
static const stub_request_t *dup_req(const char *method,
      const char *part, unsigned *n)
{
   const stub_request_t *req = NULL;
   unsigned i;

   *n = 0;
   for (i = 0; i < dup_steps; i++)
      if (     !strcmp(dup_reqs[i].method, method)
            && strstr(dup_reqs[i].url, part))
      {
         req = &dup_reqs[i];
         (*n)++;
      }
   return req;
}

/* The body of the request an upload of path makes to create name. */
static const char *dup_created(const char *path, const char *name,
      const char *tmp)
{
   static char body[1024];
   char     prefix[128];
   run_t    run;
   unsigned i;
   RFILE   *rfile = filestream_open(tmp, RETRO_VFS_FILE_ACCESS_READ,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);

   memset(&run, 0, sizeof(run));
   body[0] = '\0';
   snprintf(prefix, sizeof(prefix), "{\"name\":\"%s\"", name);
   CHECK(gdrive_update(path, rfile, on_done, &run), "%s: upload must start", path);
   drive_serve();
   CHECK(run.calls == 1 && run.success, "%s: upload must succeed once", path);
   for (i = 0; i < dup_steps; i++)
      if (     !strcmp(dup_reqs[i].url, GDRIVE_API_BASE "/files")
            && !strncmp(dup_reqs[i].body, prefix, strlen(prefix)))
         strlcpy(body, dup_reqs[i].body, sizeof(body));
   filestream_close(rfile);
   return body;
}

/* Eight 33-character IDs overflow a 256-byte list: the eighth is left
 * out whole and the oldest stays first. */
static void test_id_list(void)
{
   char     listing[512];
   char     ids[256];
   unsigned had  = failures;
   unsigned i;
   size_t   _len = strlcpy(listing, "{\"files\":[", sizeof(listing));

   printf("[folder ID list]\n");
   for (i = 0; i < 8; i++)
      _len += snprintf(listing + _len, sizeof(listing) - _len,
            "%s{\"id\":\"%c%032u\"}", i ? "," : "", "ABCDEFGH"[i], i);
   strlcat(listing, "]}", sizeof(listing));
   gdrive_extract_ids(listing, strlen(listing), ids, sizeof(ids), true);
   CHECK(strlen(ids) == 7 * 34 - 1 && ids[0] == 'A' && !strchr(ids, 'H'),
         "seven whole IDs, oldest first, and no part of the eighth, got %s",
         ids);

   if (failures == had)
      printf("  ok   folder ID list\n");
}

static void test_duplicates(const char *tmp)
{
   run_t    run;
   unsigned had = failures;

   printf("[duplicate folders]\n");
   config_get_ptr()->bools.cloud_sync_destructive = true;
   strlcpy(gdrive_st.access_token, "token-1", sizeof(gdrive_st.access_token));
   gdrive_st.folder_id[0]  = '\0';
   gdrive_st.folder_count  = 0;
   stub_has_pending        = false;
   stub_pending.overlapped = false;
   memset(&run, 0, sizeof(run));

   gdrive_begin_with_token(on_done, &run);
   drive_serve();
   CHECK(run.calls == 1 && run.success, "the sync must begin");
   /* The fake Drive matches clauses loosely; pin the queries. */
   CHECK(!strcmp(dup_reqs[0].url, GDRIVE_API_BASE "/files?q="
         "name='RetroArch'+and+mimeType='application/vnd.google-apps.folder'"
         "+and+trashed=false&fields=files(id,name)&orderBy=createdTime"
         "&pageSize=10"),
         "the RetroArch folders must be listed oldest first, got %s",
         dup_reqs[0].url);

   CHECK(!strcmp(dup_fetched("o.srm", tmp), "F_O"),
         "a file in the oldest RetroArch folder must be found");
   CHECK(!strcmp(dup_fetched("r.srm", tmp), "F_R"),
         "a file in a newer RetroArch folder must be found");
   CHECK(!strcmp(dup_fetched("dup/a.srm", tmp), "F_A"),
         "a file in the oldest of three duplicates must be found");
   CHECK(!strcmp(dup_reqs[0].url, GDRIVE_API_BASE "/files?q="
         "name='dup'+and+('R_OLD'+in+parents+or+'R_NEW'+in+parents)"
         "+and+mimeType='application/vnd.google-apps.folder'+and+trashed=false"
         "&fields=files(id)&orderBy=createdTime&pageSize=10"),
         "dup must be looked up in either RetroArch folder, oldest first, got %s",
         dup_reqs[0].url);
   CHECK(!strcmp(dup_reqs[1].url, GDRIVE_API_BASE "/files?q="
         "name='a.srm'+and+('D_OLD'+in+parents+or+'D_R'+in+parents"
         "+or+'D_NEW'+in+parents)+and+trashed=false&fields=files(id)"
         "&orderBy=modifiedTime+desc&pageSize=10"),
         "a.srm must be looked up in every duplicate, modified last first, got %s",
         dup_reqs[1].url);
   CHECK(!strcmp(dup_fetched("dup/b.srm", tmp), "F_B"),
         "a file in a newer duplicate must be found");
   CHECK(!strcmp(dup_fetched("dup/sub/c.srm", tmp), "F_C"),
         "a folder only in a newer duplicate must be found");
   CHECK(!strcmp(dup_created("n.srm", "n.srm", tmp),
         "{\"name\":\"n.srm\",\"parents\":[\"R_OLD\"]}"),
         "a new file must go in the oldest RetroArch folder");
   CHECK(!strcmp(dup_created("dup/n.srm", "n.srm", tmp),
         "{\"name\":\"n.srm\",\"parents\":[\"D_OLD\"]}"),
         "a new file must go in the oldest duplicate");
   CHECK(!strcmp(dup_created("dup/new/x.srm", "new", tmp),
         "{\"name\":\"new\","
         "\"mimeType\":\"application/vnd.google-apps.folder\","
         "\"parents\":[\"D_OLD\"]}"),
         "a new folder must go in the oldest duplicate");

   CHECK(!strcmp(dup_fetched("dup/s.srm", tmp), "F_S3"),
         "of a name in several duplicates, the copy modified last must be read");
   {
      const stub_request_t *req;
      unsigned n;
      RFILE   *rfile = filestream_open(tmp, RETRO_VFS_FILE_ACCESS_READ,
            RETRO_VFS_FILE_ACCESS_HINT_NONE);

      config_get_ptr()->bools.cloud_sync_destructive = false;
      memset(&run, 0, sizeof(run));
      CHECK(gdrive_update("dup/s.srm", rfile, on_done, &run),
            "dup/s.srm: upload must start");
      drive_serve();
      CHECK(run.calls == 1 && run.success, "dup/s.srm: upload must succeed once");
      req = dup_req("PATCH", "uploadType=media", &n);
      CHECK(n == 1 && strstr(req->url, "/files/F_S3?"),
            "the copy modified last must be the one replaced, got %s",
            req ? req->url : "nothing");
      req = dup_req("POST", "/copy", &n);
      CHECK(n == 1 && strstr(req->url, "/files/F_S3/copy")
            && strstr(req->body, "\"parents\":[\"D_OLD\"]"),
            "its backup must be copied from it into the oldest duplicate, got %s %s",
            req ? req->url : "nothing", req ? req->body : "");
      filestream_close(rfile);
      config_get_ptr()->bools.cloud_sync_destructive = true;

      memset(&run, 0, sizeof(run));
      CHECK(gdrive_delete("dup/s.srm", on_done, &run),
            "dup/s.srm: delete must start");
      drive_serve();
      CHECK(run.calls == 1 && run.success, "dup/s.srm: delete must succeed once");
      req = dup_req("DELETE", "/files/", &n);
      CHECK(n == 1 && !strcmp(req->url, GDRIVE_API_BASE "/files/F_S3"),
            "only the copy modified last must be deleted, got %u, %s",
            n, req ? req->url : "nothing");
   }
   CHECK(!stub_pending.overlapped, "requests overlapped");

   if (failures == had)
      printf("  ok   duplicate folders\n");
}

int main(void)
{
   char  tmp[64];
   FILE *f;

   snprintf(tmp, sizeof(tmp), "/tmp/gdrive_backup_%ld", (long)getpid());
   if ((f = fopen(tmp, "wb")))
   {
      fputs("SAVEDATA", f);
      fclose(f);
   }

   strlcpy(gdrive_st.folder_id, "ROOT", sizeof(gdrive_st.folder_id));
   gdrive_folder_cache_add("saves", "F_SAVES");

   test_copy_then_patch(tmp);
   test_copy_fails(tmp);
   test_token_refresh(tmp);
   test_no_backup_wanted(tmp);
   test_oauth_poll();
   test_id_list();
   test_duplicates(tmp);

   free(gdrive_st.folders);
   gdrive_st.folders = NULL;
   remove(tmp);

   if (failures)
   {
      printf("\n%u check(s) failed\n", failures);
      return 1;
   }
   printf("\nall Google Drive upload backup checks passed\n");
   return 0;
}
