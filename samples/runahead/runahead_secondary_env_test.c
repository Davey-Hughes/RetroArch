/* Run-ahead's second instance and the save interface: the copy is told
 * it is the secondary instance, and its SET_SAVE_INTERFACE gets the
 * primary's answer without reaching the frontend's registry. The filter
 * and the hook are cut from runahead.c; save_items.c is the real one. */
/* The checks are asserts: keep them in every build. */
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <boolean.h>
#include <libretro.h>

#include "../../runloop.h"
#include "../../save_items.h"

static runloop_state_t runloop_st;
static unsigned primary_calls;

runloop_state_t *runloop_state_get_ptr(void)
{
   return &runloop_st;
}

bool runloop_environment_cb(unsigned cmd, void *data)
{
   (void)cmd;
   (void)data;
   primary_calls++;
   return true;
}

void RARCH_LOG(const char *fmt, ...) { (void)fmt; }
void RARCH_WARN(const char *fmt, ...) { (void)fmt; }
void RARCH_ERR(const char *fmt, ...) { (void)fmt; }

#include "runahead_secondary_env_fragment.c"

int main(void)
{
   static unsigned char sram[16];
   struct retro_save_frontend_interface fi;
   struct retro_save_interface iface;
   struct retro_save_item item;
   struct retro_save_segment seg;

   memset(&fi, 0, sizeof(fi));
   fi.interface_version = 1;
   fi.storage_kinds     =
      RETRO_SAVE_STORAGE_BIT(RETRO_SAVE_STORAGE_FRONTEND_BUFFER);
   assert(runloop_environment_secondary_core_hook(
         RETRO_ENVIRONMENT_GET_SAVE_FRONTEND_INTERFACE, &fi));
   assert(fi.instance == RETRO_SAVE_INSTANCE_SECONDARY);
   assert(fi.interface_version == 1 && fi.get_path && fi.notify);
   assert(fi.storage_kinds
         == RETRO_SAVE_STORAGE_BIT(RETRO_SAVE_STORAGE_FRONTEND_BUFFER));

   /* A probe and a version too new, answered as for the primary. */
   assert(runloop_environment_secondary_core_hook(
         RETRO_ENVIRONMENT_GET_SAVE_FRONTEND_INTERFACE, NULL));
   fi.interface_version = 2;
   assert(!runloop_environment_secondary_core_hook(
         RETRO_ENVIRONMENT_GET_SAVE_FRONTEND_INTERFACE, &fi));

   /* The copy's registration is accepted and stored nowhere: save
    * RAM stays the primary's to write. */
   seg.data = sram;
   seg.size = sizeof(sram);
   memset(&item, 0, sizeof(item));
   item.id            = "srm";
   item.path          = "srm";
   item.storage       = RETRO_SAVE_STORAGE_FRONTEND_BUFFER;
   item.segments      = &seg;
   item.num_segments  = 1;
   item.legacy_memory = RETRO_MEMORY_SAVE_RAM;
   memset(&iface, 0, sizeof(iface));
   iface.interface_version = 1;
   iface.num_items         = 1;
   iface.item_size         = sizeof(item);
   iface.items             = &item;
   assert(runloop_environment_secondary_core_hook(
         RETRO_ENVIRONMENT_SET_SAVE_INTERFACE, &iface));
   assert(!save_items_state_get_ptr()->current);
   assert(!save_items_legacy_retired(RETRO_MEMORY_SAVE_RAM));
   assert(primary_calls == 0);

   /* A set the primary would refuse gets the same answer, and still
    * changes nothing. */
   iface.interface_version = 0;
   assert(!runloop_environment_secondary_core_hook(
         RETRO_ENVIRONMENT_SET_SAVE_INTERFACE, &iface));
   iface.interface_version = 1;
   item.id                 = NULL;
   assert(!runloop_environment_secondary_core_hook(
         RETRO_ENVIRONMENT_SET_SAVE_INTERFACE, &iface));
   assert(!save_items_state_get_ptr()->current);
   assert(!save_items_state_get_ptr()->refused);
   assert(!save_items_legacy_retired(RETRO_MEMORY_SAVE_RAM));
   assert(primary_calls == 0);

   /* Anything else still reaches the frontend's handler. */
   assert(runloop_environment_secondary_core_hook(
         RETRO_ENVIRONMENT_GET_CAN_DUPE, NULL));
   assert(primary_calls == 1);

   printf("runahead_secondary_env_test: OK\n");
   return 0;
}
