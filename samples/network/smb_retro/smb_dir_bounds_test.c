/* net_smb2.c's QUERY_DIRECTORY entry walk, UTF-16 names and span check
 * against hostile replies, with no server: the file is included so a
 * directory page can be handed to rsmb_readdir as received. Each page
 * sits at the very end of its own allocation, so a read past it is
 * caught under ASan. */
#include "../../../libretro-common/net/net_smb2.c"

#include <stdio.h>

static int failures;

#define CHECK(cond, what) do { if (!(cond)) { \
   printf("[FAIL] %s (line %d)\n", what, __LINE__); failures++; } } while (0)

struct page
{
   uint8_t *mem;
   size_t   len;
   size_t   fill;
};

static void page_init(struct page *p, size_t len)
{
   p->mem  = (uint8_t*)calloc(1, len ? len : 1);
   p->len  = len;
   p->fill = 0;
}

/* One FileDirectoryInformation entry at the fill point, padded with
 * zeros to @span octets: @name is ASCII, written as UTF-16LE, and
 * @name_len overrides its length field when not ~0. */
static void page_entry(struct page *p, uint32_t next, uint32_t name_len,
      const char *name, int is_dir, uint64_t size, size_t span)
{
   uint8_t *e = p->mem + p->fill;
   size_t   n = strlen(name), i;
   smb_put32(e, next);
   smb_put64(e + 40, size);
   smb_put32(e + 56, is_dir ? SMB2_FILE_ATTRIBUTE_DIRECTORY : 0);
   smb_put32(e + 60, name_len != 0xFFFFFFFFu ? name_len : (uint32_t)(n * 2));
   for (i = 0; i < n && 64 + i * 2 + 1 < span; i++)
      smb_put16(e + 64 + i * 2, (uint8_t)name[i]);
   p->fill += span;
}

static const struct rsmb_dirent *walk(struct rsmb_dir *d, struct page *p)
{
   d->buf     = p->mem;
   d->buf_len = p->len;
   d->buf_off = 0;
   d->done    = 1;                   /* no further QUERY_DIRECTORY */
   d->failed  = 0;
   return rsmb_readdir(NULL, d);
}

/* A malformed page is a read error, not the end of the listing. */
static int refused(struct rsmb_dir *d, struct page *p)
{
   return walk(d, p) == NULL && rsmb_readdir_failed(d);
}

static void test_bad_entries(void)
{
   struct rsmb_dir d;
   struct page     p;
   memset(&d, 0, sizeof(d));

   /* a FileNameLength that wraps 64 + len in 32 bits */
   page_init(&p, 72);
   page_entry(&p, 0, 0xFFFFFFC0u, "ab", 0, 0, 72);
   CHECK(refused(&d, &p), "name length wrapping 32 bits is refused");
   free(p.mem);

   /* a name one octet longer than the page */
   page_init(&p, 72);
   page_entry(&p, 0, 9, "ab", 0, 0, 72);
   CHECK(refused(&d, &p), "name past the page is refused");
   free(p.mem);

   /* next entry inside the current one */
   page_init(&p, 72 * 2);
   page_entry(&p, 1, ~0u, "ab", 0, 0, 72);
   page_entry(&p, 0, ~0u, "cd", 0, 0, 72);
   CHECK(refused(&d, &p), "next offset inside the entry is refused");
   free(p.mem);

   page_init(&p, 72 * 2);
   page_entry(&p, 67, ~0u, "ab", 0, 0, 72);
   page_entry(&p, 0, ~0u, "cd", 0, 0, 72);
   CHECK(refused(&d, &p), "next offset short of the name is refused");
   free(p.mem);

   /* next entry past the page */
   page_init(&p, 72);
   page_entry(&p, 73, ~0u, "ab", 0, 0, 72);
   CHECK(refused(&d, &p), "next offset past the page is refused");
   free(p.mem);

   /* a page shorter than one entry header */
   page_init(&p, 63);
   CHECK(refused(&d, &p), "page shorter than an entry is refused");
   free(p.mem);
}

static void test_good_page(void)
{
   struct rsmb_dir d;
   struct page     p;
   const struct rsmb_dirent *e;
   memset(&d, 0, sizeof(d));
   /* ".", a file whose name ends the page exactly, then a directory */
   page_init(&p, 72 + 80 + 70);
   page_entry(&p, 72, ~0u, ".", 1, 0, 72);
   page_entry(&p, 80, ~0u, "a.bin", 0, 1234, 80);
   page_entry(&p, 0, ~0u, "dir", 1, 0, 70);
   e = walk(&d, &p);
   CHECK(e && !strcmp(e->name, "a.bin") && !e->st.is_dir && e->st.size == 1234,
         "first entry after . is the file");
   e = rsmb_readdir(NULL, &d);
   CHECK(e && !strcmp(e->name, "dir") && e->st.is_dir, "last entry ends the page");
   CHECK(rsmb_readdir(NULL, &d) == NULL, "listing ends after the last entry");
   CHECK(!rsmb_readdir_failed(&d), "the end of the listing is not an error");
   free(p.mem);
}

/* A page read, then the next QUERY_DIRECTORY fails: the listing stops
 * there with a read error, so it is not taken for the whole folder. */
static void test_failed_page(void)
{
   struct rsmb_ctx *c = rsmb_new();   /* never connected: calls fail */
   struct rsmb_dir  d;
   struct page      p;
   const struct rsmb_dirent *e;
   memset(&d, 0, sizeof(d));
   page_init(&p, 80);
   page_entry(&p, 0, ~0u, "a.bin", 0, 1234, 80);
   d.buf     = p.mem;
   d.buf_len = p.len;
   e = c ? rsmb_readdir(c, &d) : NULL;
   CHECK(e && !strcmp(e->name, "a.bin"), "first page is read");
   CHECK(c && rsmb_readdir(c, &d) == NULL, "listing stops at the failed page");
   CHECK(rsmb_readdir_failed(&d), "a failed page is a read error");
   free(p.mem);
   rsmb_free(c);
}

/* A connected loopback pair: fd[0] for the context, fd[1] to hand it
 * canned replies. */
static int loop_pair(int fd[2])
{
   struct sockaddr_in a;
   socklen_t          len = sizeof(a);
   int                l   = (int)socket(AF_INET, SOCK_STREAM, 0);
   int                ok;
   fd[0] = fd[1] = -1;
   if (l < 0)
      return -1;
   memset(&a, 0, sizeof(a));
   a.sin_family      = AF_INET;
   a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   ok =  !bind(l, (struct sockaddr*)&a, sizeof(a))
      && !listen(l, 1)
      && !getsockname(l, (struct sockaddr*)&a, &len)
      && (fd[0] = (int)socket(AF_INET, SOCK_STREAM, 0)) >= 0
      && !connect(fd[0], (struct sockaddr*)&a, sizeof(a))
      && (fd[1] = (int)accept(l, NULL, NULL)) >= 0;
   socket_close(l);
   return ok ? 0 : -1;
}

/* The reply to message @mid: @status, and for a success @p as the
 * QUERY_DIRECTORY output (none when NULL). */
static void canned(int fd, uint64_t mid, uint32_t status,
      const struct page *p)
{
   uint8_t m[4 + SMB2_HDR_SIZE + 8 + 128];
   size_t  olen = p ? p->len : 0;
   size_t  len  = SMB2_HDR_SIZE + 8 + olen;
   memset(m, 0, sizeof(m));
   m[1] = (uint8_t)(len >> 16);
   m[2] = (uint8_t)(len >> 8);
   m[3] = (uint8_t)len;
   m[4] = 0xfe; m[5] = 'S'; m[6] = 'M'; m[7] = 'B';
   smb_put16(m + 4 + 4, SMB2_HDR_SIZE);
   smb_put32(m + 4 + 8, status);
   smb_put16(m + 4 + 12, SMB2_QUERY_DIRECTORY);
   smb_put32(m + 4 + 16, 1);                  /* a response */
   smb_put64(m + 4 + 24, mid);
   smb_put16(m + 4 + SMB2_HDR_SIZE, 9);
   smb_put16(m + 4 + SMB2_HDR_SIZE + 2, SMB2_HDR_SIZE + 8);
   smb_put32(m + 4 + SMB2_HDR_SIZE + 4, (uint32_t)olen);
   if (olen)
      memcpy(m + 4 + SMB2_HDR_SIZE + 8, p->mem, olen);
   send(fd, (const char*)m, (int)(4 + len), 0);
}

static void dir_reset(struct rsmb_dir *d)
{
   uint8_t *buf = d->buf;
   memset(d, 0, sizeof(*d));
   d->buf = buf;
}

/* What QUERY_DIRECTORY replies end a listing: an empty folder may
 * answer the first with STATUS_NO_SUCH_FILE; after a page that status
 * cuts the listing short, as does a page with no entries. */
static void test_query_replies(void)
{
   struct rsmb_ctx *c = rsmb_new();
   struct rsmb_dir  d;
   struct page      p;
   int              fd[2];
   if (!c || loop_pair(fd) != 0)
   {
      CHECK(0, "loopback pair");
      rsmb_free(c);
      return;
   }
   c->fd = fd[0];
   page_init(&p, 80);
   page_entry(&p, 0, ~0u, "a.bin", 0, 1234, 80);
   memset(&d, 0, sizeof(d));
   d.buf = (uint8_t*)malloc(SMB2_MAX_IO);

   canned(fd[1], c->message_id, STATUS_NO_SUCH_FILE, NULL);
   CHECK(rsmb_readdir(c, &d) == NULL && !rsmb_readdir_failed(&d),
         "an empty folder's NO_SUCH_FILE ends its listing");

   dir_reset(&d);
   canned(fd[1], c->message_id, STATUS_SUCCESS, &p);
   canned(fd[1], c->message_id + 1, STATUS_NO_SUCH_FILE, NULL);
   CHECK(rsmb_readdir(c, &d) != NULL, "a page is read");
   CHECK(rsmb_readdir(c, &d) == NULL && rsmb_readdir_failed(&d),
         "NO_SUCH_FILE after a page is a read error");

   dir_reset(&d);
   canned(fd[1], c->message_id, STATUS_SUCCESS, NULL);
   CHECK(rsmb_readdir(c, &d) == NULL && rsmb_readdir_failed(&d),
         "a page with no entries is a read error");

   free(d.buf);
   free(p.mem);
   socket_close(fd[1]);
   rsmb_free(c);
}

static void test_utf16(void)
{
   uint8_t in[8];
   char    out[32];

   /* high surrogate then an ordinary unit: U+FFFD then the unit */
   smb_put16(in, 0xD800); smb_put16(in + 2, 'x');
   rsmb_utf8(in, 4, out, sizeof(out));
   CHECK(!strcmp(out, "\xEF\xBF\xBDx"), "unpaired high surrogate");

   /* lone low surrogate */
   smb_put16(in, 0xDC00); smb_put16(in + 2, 'y');
   rsmb_utf8(in, 4, out, sizeof(out));
   CHECK(!strcmp(out, "\xEF\xBF\xBDy"), "lone low surrogate");

   /* high surrogate as the last unit */
   smb_put16(in, 'z'); smb_put16(in + 2, 0xDBFF);
   rsmb_utf8(in, 4, out, sizeof(out));
   CHECK(!strcmp(out, "z\xEF\xBF\xBD"), "high surrogate at the end");

   /* a proper pair: U+1F600 */
   smb_put16(in, 0xD83D); smb_put16(in + 2, 0xDE00);
   rsmb_utf8(in, 4, out, sizeof(out));
   CHECK(!strcmp(out, "\xF0\x9F\x98\x80"), "surrogate pair");

   /* odd length: the stray octet is dropped */
   smb_put16(in, 'q'); in[2] = 'r';
   rsmb_utf8(in, 3, out, sizeof(out));
   CHECK(!strcmp(out, "q"), "odd-length name");
}

static void test_span(void)
{
   CHECK(rsmb_span_ok(0, 0, 0), "empty span at the start");
   CHECK(rsmb_span_ok(10, 5, 15), "span ending at the end");
   CHECK(!rsmb_span_ok(10, 6, 15), "span one past the end");
   CHECK(!rsmb_span_ok(16, 0, 15), "offset past the end");
   CHECK(!rsmb_span_ok((size_t)-8, 16, 100), "offset whose sum would wrap");
   CHECK(!rsmb_span_ok(4, (size_t)-2, 100), "length whose sum would wrap");
}

int main(void)
{
   network_init();
   test_bad_entries();
   test_good_page();
   test_failed_page();
   test_query_replies();
   test_utf16();
   test_span();
   if (failures)
   {
      printf("smb_dir_bounds_test: %d failure(s)\n", failures);
      return 1;
   }
   printf("[pass] smb_dir_bounds_test\n");
   return 0;
}
