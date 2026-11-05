/* How long the fill of a save item takes, through the real save.c and
 * real file reads. An item a core registers while the game runs is
 * read on the main thread before the next frame; this times that read
 * beside save RAM's read at content start, warm and, on Linux, with the
 * file dropped from the page cache first. A measurement to read, not a
 * test: it exits 0 whatever the numbers are, and 1 when a read did not
 * fill the buffer.
 *
 * Usage: save_items_fill_bench <dir> [rounds]
 * <dir> must be on the disk saves normally live on, not on tmpfs. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#endif

#include <features/features_cpu.h>

#include "../../../save.c"

#define FRAME_MS 16.7
#define MAX_ROUNDS 64

static unsigned char *ram;
static size_t ram_size;
static struct retro_save_segment seg;
/* The tail row fills only the first half. */
static bool check_fill = true;
static int status      = 0;

void runloop_msg_queue_push(const char *msg, size_t len, unsigned prio,
      unsigned duration, bool flush, char *title,
      enum message_queue_icon icon, enum message_queue_category category)
{
   (void)msg;
   (void)len;
   (void)prio;
   (void)duration;
   (void)flush;
   (void)title;
   (void)icon;
   (void)category;
}

void RARCH_LOG(const char *fmt, ...) { (void)fmt; }
void RARCH_WARN(const char *fmt, ...) { (void)fmt; }
void RARCH_ERR(const char *fmt, ...) { (void)fmt; }

const char *msg_hash_to_str(enum msg_hash_enums msg)
{
   (void)msg;
   return "";
}

bool fill_pathname_application_data(char *s, size_t len)
{
   (void)s;
   (void)len;
   return false;
}

void runloop_save_items_notice(const char *name)
{
   (void)name;
}

bool core_get_memory(retro_ctx_memory_info_t *info)
{
   info->data = info->id == RETRO_MEMORY_SAVE_RAM ? ram : NULL;
   info->size = info->id == RETRO_MEMORY_SAVE_RAM ? ram_size : 0;
   return true;
}

/* Drops @path from the page cache, so the next read comes from the
 * disk. False where that cannot be done. */
static bool drop_cache(const char *path)
{
#if defined(__linux__) && defined(POSIX_FADV_DONTNEED)
   bool ok;
   int fd = open(path, O_RDONLY);
   if (fd < 0)
      return false;
   ok = fdatasync(fd) == 0
      && posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED) == 0;
   close(fd);
   return ok;
#else
   (void)path;
   return false;
#endif
}

static void reset(void)
{
   event_save_items_deinit();
   save_items_clear();
   path_deinit_savefile();
   path_init_savefile_new();
}

static void register_set(struct retro_save_interface *iface)
{
   struct save_items_paths paths;
   memset(&paths, 0, sizeof(paths));
   save_items_register(iface, &paths);
   event_save_items_registered();
   save_items_drop_previous();
}

/* Save RAM's read at content start, as event_load_save_files() does
 * it today. */
static double time_save_ram(const char *path, bool cold)
{
   retro_time_t t0;
   union string_list_elem_attr attr;
   reset();
   attr.i = RETRO_MEMORY_SAVE_RAM;
   string_list_append(task_save_files, path, attr);
   if (cold)
      drop_cache(path);
   t0 = cpu_features_get_time_usec();
   event_load_save_files(false);
   return (double)(cpu_features_get_time_usec() - t0) / 1000.0;
}

/* An item registered while the game runs: the fill
 * event_save_items_fill() makes before the next frame. */
static double time_item(const char *path, bool cold)
{
   retro_time_t t0;
   struct retro_save_item item;
   struct retro_save_interface iface;
   reset();
   memset(&iface, 0, sizeof(iface));
   iface.interface_version = 1;
   iface.item_size         = sizeof(item);
   register_set(&iface);
   event_load_save_files(false);
   memset(&item, 0, sizeof(item));
   item.id            = "late";
   item.path          = path;
   item.storage       = RETRO_SAVE_STORAGE_FRONTEND_BUFFER;
   item.root          = RETRO_SAVE_ROOT_ABSOLUTE;
   item.segments      = &seg;
   item.num_segments  = 1;
   item.legacy_memory = RETRO_SAVE_NO_LEGACY_MEMORY;
   iface.num_items    = 1;
   iface.items        = &item;
   register_set(&iface);
   if (cold)
      drop_cache(path);
   t0 = cpu_features_get_time_usec();
   event_save_items_fill(false);
   return (double)(cpu_features_get_time_usec() - t0) / 1000.0;
}

static bool RETRO_CALLCONV bench_legacy_load(unsigned item,
      unsigned index, const char *format, const void *buf, size_t len)
{
   (void)item;
   (void)index;
   (void)format;
   memcpy(ram, buf, len < ram_size ? len : ram_size);
   return true;
}

/* An item whose own file is missing, filled at load from @path, its
 * earlier name: raw, or converted by legacy_load(). */
static double time_legacy(const char *path, bool cold, bool convert)
{
   static char own[PATH_MAX_LENGTH + 16];
   retro_time_t t0;
   const char *legacy[2];
   struct retro_save_item item;
   struct retro_save_interface iface;
   struct retro_save_frontend_interface fi;
   reset();
   memset(&fi, 0, sizeof(fi));
   fi.interface_version = 1;
   fi.features          = RETRO_SAVE_FEATURE_LEGACY_LOAD;
   save_items_get_frontend_interface(&fi, RETRO_SAVE_INSTANCE_PRIMARY);
   snprintf(own, sizeof(own), "%s.missing", path);
   legacy[0] = path;
   legacy[1] = NULL;
   memset(&iface, 0, sizeof(iface));
   iface.interface_version = 1;
   iface.item_size         = sizeof(item);
   iface.legacy_load       = convert ? bench_legacy_load : NULL;
   memset(&item, 0, sizeof(item));
   item.id            = "srm";
   item.path          = own;
   item.storage       = RETRO_SAVE_STORAGE_FRONTEND_BUFFER;
   item.root          = RETRO_SAVE_ROOT_ABSOLUTE;
   item.segments      = &seg;
   item.num_segments  = 1;
   item.legacy_memory = RETRO_SAVE_NO_LEGACY_MEMORY;
   item.legacy_paths  = legacy;
   iface.num_items    = 1;
   iface.items        = &item;
   register_set(&iface);
   if (cold)
      drop_cache(path);
   t0 = cpu_features_get_time_usec();
   event_load_save_files(false);
   return (double)(cpu_features_get_time_usec() - t0) / 1000.0;
}

static double time_legacy_raw(const char *path, bool cold)
{
   return time_legacy(path, cold, false);
}

static double time_legacy_convert(const char *path, bool cold)
{
   return time_legacy(path, cold, true);
}

/* SAVE_FILES on an unchanged item that holds half of @path: the
 * compare reads the head and the kept tail. */
static double time_tail_compare(const char *path, bool cold)
{
   retro_time_t t0;
   struct retro_save_item item;
   struct retro_save_interface iface;
   struct retro_save_segment half;
   reset();
   half.data = ram;
   half.size = ram_size / 2;
   memset(&iface, 0, sizeof(iface));
   iface.interface_version = 1;
   iface.item_size         = sizeof(item);
   memset(&item, 0, sizeof(item));
   item.id            = "srm";
   item.path          = path;
   item.storage       = RETRO_SAVE_STORAGE_FRONTEND_BUFFER;
   item.root          = RETRO_SAVE_ROOT_ABSOLUTE;
   item.segments      = &half;
   item.num_segments  = 1;
   item.legacy_memory = RETRO_SAVE_NO_LEGACY_MEMORY;
   iface.num_items    = 1;
   iface.items        = &item;
   register_set(&iface);
   event_load_save_files(false);
   if (cold)
      drop_cache(path);
   t0 = cpu_features_get_time_usec();
   event_save_files(true, false, NULL);
   return (double)(cpu_features_get_time_usec() - t0) / 1000.0;
}

/* The copy SAVE_FILES or the final write makes before it drops a tail
 * or replaces a refused file. */
static double time_copy(const char *path, bool cold)
{
   static char dir[PATH_MAX_LENGTH + 16];
   static char copy[PATH_MAX_LENGTH + 16];
   retro_time_t t0;
   unsigned n;
   snprintf(dir, sizeof(dir), "%s.undo", path);
   /* A copy already holding the file is not written again. */
   fill_pathname_join(copy, dir, path_basename(path), sizeof(copy));
   filestream_delete(copy);
   if (cold)
      drop_cache(path);
   t0 = cpu_features_get_time_usec();
   save_item_copy_file(path, dir, SAVE_ITEM_TMP, &n, false);
   return (double)(cpu_features_get_time_usec() - t0) / 1000.0;
}

static int cmp_double(const void *a, const void *b)
{
   double x = *(const double*)a;
   double y = *(const double*)b;
   return x < y ? -1 : x > y;
}

static void row(const char *size, const char *what, const char *path,
      bool cold, unsigned rounds,
      double (*timed)(const char *path, bool cold))
{
   unsigned i;
   double ms[MAX_ROUNDS];
   double median;
   if (cold && !drop_cache(path))
   {
      printf("%-9s %-20s %-5s %10s %8s  %s\n", size, what, "cold",
            "-", "-", "not measured here");
      return;
   }
   if (!cold)
      timed(path, false);
   for (i = 0; i < rounds; i++)
   {
      memset(ram, 0, ram_size);
      ms[i] = timed(path, cold);
      if (check_fill && (ram[0] != 0xa5 || ram[ram_size - 1] != 0x5a))
      {
         printf("%-9s %-20s the read did not fill the buffer\n", size, what);
         status = 1;
         return;
      }
   }
   qsort(ms, rounds, sizeof(ms[0]), cmp_double);
   median = ms[rounds / 2];
   printf("%-9s %-20s %-5s %10.3f %8.3f  %s\n", size, what,
         cold ? "cold" : "warm", median, ms[rounds - 1],
         ms[rounds - 1] > FRAME_MS ? "OVER" : "within");
}

int main(int argc, char *argv[])
{
   static const size_t sizes[]       = {128 * 1024, 1024 * 1024,
      8 * 1024 * 1024, 70 * 1024 * 1024};
   static const char *const labels[] = {"128 KiB", "1 MiB", "8 MiB",
      "70 MiB"};
   static char path[PATH_MAX_LENGTH];
   unsigned rounds = 7;
   unsigned s;
#if defined(__linux__)
   char load[128];
   FILE *f;
#endif

   if (argc < 2)
   {
      fprintf(stderr, "usage: save_items_fill_bench <dir> [rounds]\n");
      return 2;
   }
   if (argc > 2)
      rounds = (unsigned)strtoul(argv[2], NULL, 10);
   if (rounds < 1 || rounds > MAX_ROUNDS)
      rounds = 7;

   printf("save item fill, %u rounds a row, frame budget %.1f ms\n",
         rounds, FRAME_MS);
#if defined(__linux__)
   if ((f = fopen("/proc/loadavg", "r")))
   {
      if (fgets(load, sizeof(load), f))
         printf("load average: %s", load);
      fclose(f);
   }
#else
   printf("cold reads: not measured on this platform\n");
#endif
   printf("%-9s %-20s %-5s %10s %8s  %s\n", "size", "read", "cache",
         "median ms", "max ms", "frame");

   for (s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++)
   {
      size_t i;
      unsigned char *file;
      ram_size = sizes[s];
      if (!(ram = (unsigned char*)malloc(ram_size))
            || !(file = (unsigned char*)malloc(ram_size)))
      {
         fprintf(stderr, "no memory for %s\n", labels[s]);
         return 1;
      }
      for (i = 0; i < ram_size; i++)
         file[i] = (unsigned char)(i * 7);
      file[0]            = 0xa5;
      file[ram_size - 1] = 0x5a;
      seg.data           = ram;
      seg.size           = ram_size;
      snprintf(path, sizeof(path), "%s/fill_bench_%u.srm", argv[1], s);
      if (!filestream_write_file(path, file, (int64_t)ram_size))
      {
         fprintf(stderr, "cannot write %s\n", path);
         return 1;
      }
      free(file);
      row(labels[s], "save RAM at load", path, false, rounds, time_save_ram);
      row(labels[s], "save RAM at load", path, true, rounds, time_save_ram);
      row(labels[s], "item while running", path, false, rounds, time_item);
      row(labels[s], "item while running", path, true, rounds, time_item);
      row(labels[s], "old name, raw", path, false, rounds, time_legacy_raw);
      row(labels[s], "old name, raw", path, true, rounds, time_legacy_raw);
      row(labels[s], "old name, converted", path, false, rounds,
            time_legacy_convert);
      row(labels[s], "old name, converted", path, true, rounds,
            time_legacy_convert);
      check_fill = false;
      row(labels[s], "tail at SAVE_FILES", path, false, rounds,
            time_tail_compare);
      row(labels[s], "tail at SAVE_FILES", path, true, rounds,
            time_tail_compare);
      row(labels[s], "safety copy", path, false, rounds, time_copy);
      row(labels[s], "safety copy", path, true, rounds, time_copy);
      check_fill = true;
      reset();
      {
         static char copy[PATH_MAX_LENGTH + 16];
         snprintf(copy, sizeof(copy), "%s.undo", path);
         fill_pathname_join(copy, copy, path_basename(path), sizeof(copy));
         filestream_delete(copy);
         snprintf(copy, sizeof(copy), "%s.undo", path);
         path_rmdir(copy);
      }
      filestream_delete(path);
      free(ram);
   }
   path_deinit_savefile();
   return status;
}
