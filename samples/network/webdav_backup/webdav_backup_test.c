/* Harness for the WebDAV upload backup.
 *
 * With destructive sync off, cloud sync promises not to throw away the
 * server's copy of a file.  A delete kept that promise - WebDAV moved
 * the file into deleted/ - but an upload did not: it overwrote the
 * server's copy in place, so the one change most likely to lose a save
 * (an older or broken save from another device replacing a good one)
 * left nothing behind.
 *
 * An upload now copies the server's file to deleted/<path>-<time>
 * first, and only then PUTs.  COPY rather than MOVE, so the server
 * keeps its file until the PUT replaces it.  The test plays the
 * server and checks, request by request:
 *
 * - the backup is taken before the PUT, into deleted/, with the same
 *   naming a delete uses;
 * - a 404 (nothing on the server yet) is not an error;
 * - a backup that fails stops the upload, reported once, with no PUT;
 * - destructive sync, and the server manifest, which is rewritten
 *   every sync, are not backed up;
 * - the COPY and the PUT each get their own Digest retry.
 *
 * Deletes: a file already gone counts as deleted.  A DELETE or MOVE
 * answered 404 is done; a MOVE refused otherwise (Go's x/net/webdav
 * answers a missing source with 403) is done only if a PROPFIND of
 * the source answers 404.
 *
 * Includes the driver's translation unit so the paths are tested as
 * shipped rather than as a copy that can drift from them.
 */

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

#include <boolean.h>
#include <lists/string_list.h>
#include <streams/file_stream.h>

#include "../../../network/cloud_sync/webdav.c"

#include "stubs_retroarch.h"

#define BASE       "http://127.0.0.1/dav/"
#define MAX_STEPS  32

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

/* What the scripted server answers.  Each array is consumed in order,
 * one per request of its method; everything else gets 201. */
typedef struct
{
   int copy[4];
   int put[4];
   int move[4];
   int propfind[4];
   int del[4];
} script_t;

typedef struct
{
   char     trace[MAX_STEPS][600];
   unsigned steps;
   unsigned calls;
   bool     success;
} run_t;

/* header block: the line, its NUL, the closing NUL */
static char challenge_headers[] =
      "WWW-Authenticate: Digest realm=\"dav\", nonce=\"6f2a\", "
      "qop=\"auth\", algorithm=MD5\0";

static void on_done(void *user_data, const char *path, bool success,
      RFILE *file)
{
   run_t *run = (run_t*)user_data;
   (void)path; (void)file;
   run->calls++;
   run->success = success;
}

static const char *rel(const char *url)
{
   return strncmp(url, BASE, sizeof(BASE) - 1) ? url : url + sizeof(BASE) - 1;
}

static void run_update(const char *path, bool destructive,
      const script_t *script, run_t *run, const char *tmp)
{
   http_transfer_data_t resp;
   webdav_state_t *webdav_st = webdav_state_get_ptr();
   unsigned ncopy = 0, nput = 0;
   RFILE *rfile;

   memset(run, 0, sizeof(*run));
   config_get_ptr()->bools.cloud_sync_destructive = destructive;

   /* A fresh sync, as webdav_sync_begin() leaves it: basic auth until
    * a Digest challenge says otherwise, no collections known yet. */
   webdav_cleanup_digest();
   webdav_st->basic = true;

   if (webdav_st->dirs)
      string_list_free(webdav_st->dirs);
   webdav_st->dirs = NULL;

   rfile = filestream_open(tmp, RETRO_VFS_FILE_ACCESS_READ,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);

   stub_has_pending       = false;
   stub_pending.overlapped = false;
   webdav_update(path, rfile, on_done, run);

   while (stub_has_pending && run->steps < MAX_STEPS)
   {
      stub_request_t req = stub_pending;
      int status         = 201;

      stub_has_pending = false;
      if (req.dest[0])
         snprintf(run->trace[run->steps], sizeof(run->trace[0]),
               "%s %s -> %s", req.method, rel(req.url), rel(req.dest));
      else
         snprintf(run->trace[run->steps], sizeof(run->trace[0]),
               "%s %s", req.method, rel(req.url));
      run->steps++;

      if (!strcmp(req.method, "COPY") && ncopy < 4 && script->copy[ncopy])
         status = script->copy[ncopy++];
      else if (!strcmp(req.method, "PUT") && nput < 4 && script->put[nput])
         status = script->put[nput++];

      memset(&resp, 0, sizeof(resp));
      resp.status  = status;
      resp.headers = (status == 401) ? challenge_headers : NULL;
      req.cb(NULL, &resp, req.user_data, NULL);
   }

   CHECK(!stub_pending.overlapped, "%s: requests overlapped", path);
   filestream_close(rfile);
}

static void run_delete(const char *path, bool destructive,
      const script_t *script, run_t *run)
{
   http_transfer_data_t resp;
   webdav_state_t *webdav_st = webdav_state_get_ptr();
   unsigned nmove = 0, nprop = 0, ndel = 0;

   memset(run, 0, sizeof(*run));
   config_get_ptr()->bools.cloud_sync_destructive = destructive;

   webdav_cleanup_digest();
   webdav_st->basic = true;

   if (webdav_st->dirs)
      string_list_free(webdav_st->dirs);
   webdav_st->dirs = NULL;

   stub_has_pending        = false;
   stub_pending.overlapped = false;
   webdav_delete(path, on_done, run);

   while (stub_has_pending && run->steps < MAX_STEPS)
   {
      stub_request_t req = stub_pending;
      int status         = 201;

      stub_has_pending = false;
      if (req.dest[0])
         snprintf(run->trace[run->steps], sizeof(run->trace[0]),
               "%s %s -> %s", req.method, rel(req.url), rel(req.dest));
      else
         snprintf(run->trace[run->steps], sizeof(run->trace[0]),
               "%s %s%s", req.method, rel(req.url),
               req.had_digest ? " (digest)" : "");
      run->steps++;

      if (!strcmp(req.method, "MOVE") && nmove < 4 && script->move[nmove])
         status = script->move[nmove++];
      else if (!strcmp(req.method, "PROPFIND") && nprop < 4
            && script->propfind[nprop])
         status = script->propfind[nprop++];
      else if (!strcmp(req.method, "DELETE") && ndel < 4 && script->del[ndel])
         status = script->del[ndel++];

      memset(&resp, 0, sizeof(resp));
      resp.status  = status;
      resp.headers = (status == 401) ? challenge_headers : NULL;
      req.cb(NULL, &resp, req.user_data, NULL);
   }

   CHECK(!stub_pending.overlapped, "%s: requests overlapped", path);
}

static int find_step(const run_t *run, const char *prefix, unsigned from)
{
   unsigned i;
   for (i = from; i < run->steps; i++)
      if (!strncmp(run->trace[i], prefix, strlen(prefix)))
         return (int)i;
   return -1;
}

static unsigned count_steps(const run_t *run, const char *prefix)
{
   unsigned i, n = 0;
   for (i = 0; i < run->steps; i++)
      if (!strncmp(run->trace[i], prefix, strlen(prefix)))
         n++;
   return n;
}

static void dump(const char *what, const run_t *run)
{
   unsigned i;
   printf("  %s:\n", what);
   for (i = 0; i < run->steps; i++)
      printf("    %s\n", run->trace[i]);
}

/* "COPY saves/a.srm -> deleted/saves/a.srm-yymmdd-hhmmss" */
static bool backup_named_like_delete(const char *step)
{
   static const char want[] = "COPY saves/a.srm -> deleted/saves/a.srm-";
   const char *p = step + sizeof(want) - 1;
   int i;
   if (strncmp(step, want, sizeof(want) - 1))
      return false;
   for (i = 0; i < 13; i++)
      if (i == 6 ? p[i] != '-' : !isdigit((unsigned char)p[i]))
         return false;
   return p[13] == '\0';
}

static void test_backup_then_put(const char *tmp)
{
   script_t script = {{0}, {0}};
   run_t    run;
   int      copy, put;

   run_update("saves/a.srm", false, &script, &run, tmp);
   dump("non-destructive upload", &run);

   copy = find_step(&run, "COPY ", 0);
   put  = find_step(&run, "PUT saves/a.srm", 0);
   CHECK(copy >= 0, "non-destructive upload must back up the server copy");
   CHECK(put >= 0, "non-destructive upload must still upload");
   CHECK(copy < 0 || put < 0 || copy < put, "backup must come before the PUT");
   CHECK(copy < 0 || backup_named_like_delete(run.trace[copy]),
         "backup must go to deleted/<path>-<yymmdd-hhmmss>, got \"%s\"",
         copy < 0 ? "" : run.trace[copy]);
   CHECK(find_step(&run, "MKCOL deleted/saves/", 0) >= 0
         && (copy < 0 || find_step(&run, "MKCOL deleted/saves/", 0) < copy),
         "deleted/saves/ must exist before the COPY");
   CHECK(!count_steps(&run, "MOVE") && !count_steps(&run, "DELETE"),
         "the server copy must be copied, never moved or deleted");
   CHECK(run.calls == 1 && run.success,
         "non-destructive upload must report success once");
}

static void test_nothing_to_back_up(const char *tmp)
{
   script_t script = {{404}, {0}};
   run_t    run;

   run_update("saves/a.srm", false, &script, &run, tmp);
   CHECK(count_steps(&run, "PUT saves/a.srm") == 1,
         "404 on the backup (no server copy yet) must still upload");
   CHECK(run.calls == 1 && run.success,
         "404 on the backup must report success once");
}

static void test_backup_fails(const char *tmp)
{
   script_t script = {{500}, {0}};
   run_t    run;

   run_update("saves/a.srm", false, &script, &run, tmp);
   CHECK(!count_steps(&run, "PUT"),
         "a failed backup must stop the upload");
   CHECK(run.calls == 1 && !run.success,
         "a failed backup must report failure once");
}

static void test_destructive(const char *tmp)
{
   script_t script = {{0}, {0}};
   run_t    run;

   run_update("saves/a.srm", true, &script, &run, tmp);
   CHECK(!count_steps(&run, "COPY") && !count_steps(&run, "MKCOL deleted/"),
         "destructive sync must not back up");
   CHECK(count_steps(&run, "PUT saves/a.srm") == 1 && run.calls == 1
         && run.success, "destructive sync must upload once");
}

static void test_manifest(const char *tmp)
{
   script_t script = {{0}, {0}};
   run_t    run;

   run_update(CLOUD_SYNC_SERVER_MANIFEST, false, &script, &run, tmp);
   CHECK(!count_steps(&run, "COPY") && !count_steps(&run, "MKCOL"),
         "the server manifest must not be backed up");
   CHECK(count_steps(&run, "PUT " CLOUD_SYNC_SERVER_MANIFEST) == 1
         && run.calls == 1 && run.success,
         "the server manifest must upload once");
}

static void test_each_request_reauths(const char *tmp)
{
   script_t script = {{401, 201}, {401, 201}};
   run_t    run;

   run_update("saves/a.srm", false, &script, &run, tmp);
   dump("challenge on COPY and on PUT", &run);
   CHECK(count_steps(&run, "COPY") == 2,
         "COPY must get its own Digest retry");
   CHECK(count_steps(&run, "PUT") == 2,
         "PUT must get its own Digest retry after the COPY used one");
   CHECK(run.calls == 1 && run.success,
         "one challenge each must still succeed, once");
}

typedef struct
{
   const char *name;
   bool        destructive;
   script_t    script;
   bool        success;
   unsigned    propfinds;
} delete_case_t;

static const delete_case_t delete_cases[] = {
   { "DELETE 404",                   true,
      {{0}, {0}, {0},   {0},        {404}}, true,  0 },
   { "DELETE 500",                   true,
      {{0}, {0}, {0},   {0},        {500}}, false, 0 },
   { "MOVE 404",                     false,
      {{0}, {0}, {404}, {0},        {0}},   true,  0 },
   { "MOVE 403, PROPFIND 404",       false,
      {{0}, {0}, {403}, {404},      {0}},   true,  1 },
   { "MOVE 403, PROPFIND 207",       false,
      {{0}, {0}, {403}, {207},      {0}},   false, 1 },
   { "MOVE 409, PROPFIND 207",       false,
      {{0}, {0}, {409}, {207},      {0}},   false, 1 },
   { "MOVE 403, PROPFIND 401 + 404", false,
      {{0}, {0}, {403}, {401, 404}, {0}},   true,  2 },
   { "MOVE 403, PROPFIND -1",        false,
      {{0}, {0}, {403}, {-1},       {0}},   false, 1 }
};

static void test_deletes(void)
{
   unsigned i;

   for (i = 0; i < sizeof(delete_cases) / sizeof(delete_cases[0]); i++)
   {
      const delete_case_t *c = &delete_cases[i];
      run_t run;

      run_delete("saves/a.srm", c->destructive, &c->script, &run);
      if (c->propfinds)
         dump(c->name, &run);

      CHECK(run.calls == 1 && run.success == c->success,
            "%s: must report %s once", c->name,
            c->success ? "success" : "failure");
      CHECK(count_steps(&run, c->destructive
               ? "DELETE saves/a.srm" : "MOVE saves/a.srm -> deleted/saves/a.srm-") == 1
            && !count_steps(&run, c->destructive ? "MOVE" : "DELETE"),
            "%s: must send one %s", c->name,
            c->destructive ? "DELETE" : "MOVE into deleted/");
      CHECK(count_steps(&run, "PROPFIND") == c->propfinds
            && count_steps(&run, "PROPFIND saves/a.srm") == c->propfinds,
            "%s: must send %u PROPFIND(s) on the source", c->name,
            c->propfinds);
      if (c->script.propfind[0] == 401)
         CHECK(find_step(&run, "PROPFIND saves/a.srm (digest)", 0) >= 0,
               "%s: the PROPFIND retry must carry Digest", c->name);
   }
}

/* A PROPFIND that can't be started fails the delete: the sync is not
 * left waiting on a callback that never comes. */
static void test_propfind_refused(void)
{
   static const script_t script = {{0}, {0}, {403}, {0}, {0}};
   run_t run;

   stub_refuse = "PROPFIND";
   run_delete("saves/a.srm", false, &script, &run);
   stub_refuse = NULL;
   CHECK(run.calls == 1 && !run.success,
         "refused PROPFIND: must report failure once");
}

int main(void)
{
   settings_t     *settings  = config_get_ptr();
   webdav_state_t *webdav_st = webdav_state_get_ptr();
   char tmp[64];
   FILE *f;

   snprintf(tmp, sizeof(tmp), "/tmp/webdav_backup_%ld", (long)getpid());
   if ((f = fopen(tmp, "wb")))
   {
      fputs("SAVEDATA", f);
      fclose(f);
   }

   strlcpy(settings->arrays.webdav_username, "user",
         sizeof(settings->arrays.webdav_username));
   strlcpy(settings->arrays.webdav_password, "pass",
         sizeof(settings->arrays.webdav_password));
   strlcpy(webdav_st->url, BASE, sizeof(webdav_st->url));
   webdav_st->dav_verified = true;


   test_backup_then_put(tmp);
   test_nothing_to_back_up(tmp);
   test_backup_fails(tmp);
   test_destructive(tmp);
   test_manifest(tmp);
   test_each_request_reauths(tmp);
   test_deletes();
   test_propfind_refused();

   if (webdav_st->dirs)
      string_list_free(webdav_st->dirs);
   webdav_st->dirs = NULL;
   webdav_cleanup_digest();
   remove(tmp);

   if (failures)
   {
      printf("\n%u check(s) failed\n", failures);
      return 1;
   }
   printf("\nall WebDAV upload backup and delete checks passed\n");
   return 0;
}
