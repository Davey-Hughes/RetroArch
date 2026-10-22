/* Mapped Port in input/input_driver.c: the users mapped to a core port
 * feed it and get its rumble. Plays As: a controller's own player
 * places it, and one playing as another user's player feeds that
 * user's core port; the remap ports stay as they were. The real
 * input_driver.c, with the rest of the frontend stood in for. */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include <compat/strl.h>
#include <file/config_file.h>
#include <file/file_path.h>

#include "../../../configuration.h"
#include "../../../command.h"
#include "../../../retroarch.h"
#include "../../../runloop.h"
#include "../../../verbosity.h"
#include "../../../msg_hash.h"
#include "../../../gfx/video_driver.h"
#include "../../../input/input_driver.h"
#include "../../../input/input_remapping.h"
#include "../../../menu/menu_driver.h"
#include "../../../menu/menu_entries.h"
#include "../../../menu/menu_input.h"

static settings_t        settings;
static runloop_state_t   runloop_st;
static struct menu_state t_menu_st;
static int               failures;

#define CHECK(c, msg) do { if (!(c)) { \
   printf("FAIL line %d: %s\n", __LINE__, msg); failures++; } } while (0)

settings_t *config_get_ptr(void) { return &settings; }
runloop_state_t *runloop_state_get_ptr(void) { return &runloop_st; }
void RARCH_LOG(const char *fmt, ...) { (void)fmt; }
void RARCH_WARN(const char *fmt, ...) { (void)fmt; }
bool command_event(enum event_command cmd, void *data)
{ (void)cmd; (void)data; return false; }
bool retroarch_ctl(enum rarch_ctl_state state, void *data)
{ (void)state; (void)data; return false; }
/* config_file.c's #include and #reference, which no test reaches */
void path_linked_list_free(struct path_linked_list *l) { (void)l; }
void path_linked_list_add_path(struct path_linked_list *l, char *path)
{ (void)l; (void)path; }
void fill_pathname_resolve_relative(char *s, const char *ref,
      const char *in, size_t len)
{ (void)ref; strlcpy(s, in, len); }
size_t fill_pathname_abbreviated_or_relative(char *s, const char *ref,
      const char *in, size_t len)
{ (void)ref; return strlcpy(s, in, len); }
bool video_driver_has_focus(void) { return true; }
#if defined(_WIN32)
void win32_text_entry(bool active) { (void)active; }
#endif
bool video_driver_headset_focused(void) { return false; }
bool video_driver_get_viewport_info(struct video_viewport *vp)
{ (void)vp; return false; }
bool video_driver_translate_coord_viewport(struct video_viewport *vp,
      int x, int y, uint32_t *pos, uint32_t *screen_pos, bool oob)
{ (void)vp; (void)x; (void)y; (void)pos; (void)screen_pos; (void)oob;
  return false; }
struct menu_state *menu_state_get_ptr(void) { return &t_menu_st; }
bool menu_input_dialog_get_display_kb(void) { return false; }
void menu_entry_get(menu_entry_t *entry, size_t stack_idx,
      size_t i, void *userdata, bool use_representation)
{ (void)entry; (void)stack_idx; (void)i; (void)userdata;
  (void)use_representation; }
const char *msg_hash_to_str(enum msg_hash_enums msg)
{ (void)msg; return ""; }
void runloop_msg_queue_push(const char *msg, size_t len,
      unsigned prio, unsigned duration, bool flush, char *title,
      enum message_queue_icon icon, enum message_queue_category category)
{ (void)msg; (void)len; (void)prio; (void)duration; (void)flush;
  (void)title; (void)icon; (void)category; }

/* Slot n holds t_buttons[n] and pushes axis t_axis_held[n] all the
 * way; every slot's profile has the RetroPad's ids as its buttons and
 * L2 and R2 as axes 4 and 5, unless a test gives it another. */
static uint32_t t_buttons[MAX_USERS];
static uint32_t t_axis_held[MAX_USERS];
/* the button reads of each slot, apart from its RetroPad's */
static unsigned t_button_reads[MAX_USERS];
static unsigned t_rumbled[MAX_USERS];
/* bit n: slot n's motors answer false */
static unsigned t_rumble_refused;
static struct retro_keybind t_auto[RARCH_BIND_LIST_END];
/* An xpad's numbering: Back 6 is Select, Guide 8 none of the RetroPad's
 * buttons, LT axis 2 as well as button 15. */
static struct retro_keybind t_xpad[RARCH_BIND_LIST_END];
/* the xpad's, with A and B the other way round */
static struct retro_keybind t_xpad_ab[RARCH_BIND_LIST_END];
/* the RetroPad's ids, as t_auto, with hotkeys of its own */
static struct retro_keybind t_auto_own[RARCH_BIND_LIST_END];
#define T_OWN_MENU 20
#define T_OWN_FF   23
static bool t_query(unsigned pad) { return pad < MAX_USERS; }
static bool t_held(unsigned port, uint16_t joykey)
{
   return port < MAX_USERS && joykey < 32
      && ((t_buttons[port] >> joykey) & 1);
}
static int32_t t_button(unsigned port, uint16_t joykey)
{
   if (port < MAX_USERS)
      t_button_reads[port]++;
   return t_held(port, joykey);
}
static int16_t t_state(rarch_joypad_info_t *joypad_info,
      const uint16_t *joykeys, const uint32_t *joyaxes, unsigned port)
{
   unsigned i;
   int16_t ret = 0;
   (void)joyaxes;
   (void)port;
   for (i = 0; i < RARCH_FIRST_CUSTOM_BIND; i++)
      if (joykeys[i] != NO_BTN && t_held(joypad_info->joy_idx, joykeys[i]))
         ret |= (int16_t)(1 << i);
   return ret;
}
static int16_t t_axis(unsigned port, uint32_t joyaxis)
{
   return (port < MAX_USERS && joyaxis != AXIS_NONE
         && t_axis_held[port] == joyaxis) ? 0x7fff : 0;
}
static void t_poll(void) { }
static bool t_rumble(unsigned pad, enum retro_rumble_effect effect,
      uint16_t strength)
{
   (void)effect;
   if (pad < MAX_USERS && strength)
      t_rumbled[pad]++;
   return !((t_rumble_refused >> pad) & 1);
}
static const char *t_name(unsigned pad) { (void)pad; return "Pad"; }
static input_device_driver_t t_joypad = {
   NULL, t_query, NULL, t_button, t_state, NULL, t_axis, t_poll, t_rumble,
   NULL, NULL, NULL, t_name, "test" };

#define T_BIT(id) (1u << (id))
#define T_A       T_BIT(RETRO_DEVICE_ID_JOYPAD_A)
#define T_B       T_BIT(RETRO_DEVICE_ID_JOYPAD_B)

/* Four users, user n reading slot n into core port n; no binds of
 * their own; no reservations or Plays As entries; nothing held. */
static void t_reset(void)
{
   static const uint16_t xpad[RARCH_FIRST_CUSTOM_BIND] = {
      1, 3, 6, 7, 11, 12, 13, 14, 0, 2, 4, 5, 15, 16, 9, 10 };
   unsigned u, i;
   input_driver_state_t *st = input_state_get_ptr();

   t_rumble_refused = 0;

   for (i = 0; i < RARCH_BIND_LIST_END; i++)
   {
      t_auto[i].joykey  = (i < RARCH_FIRST_CUSTOM_BIND) ? (uint16_t)i : NO_BTN;
      t_auto[i].joyaxis = AXIS_NONE;
      t_auto[i].attr    = RETRO_KEYBIND_ATTR(RETROK_UNKNOWN, NO_BTN,
            i < RARCH_FIRST_CUSTOM_BIND);
   }
   t_auto[RETRO_DEVICE_ID_JOYPAD_L2].joyaxis = AXIS_POS(4);
   t_auto[RETRO_DEVICE_ID_JOYPAD_R2].joyaxis = AXIS_POS(5);
   for (i = 0; i < RARCH_BIND_LIST_END; i++)
   {
      t_xpad[i]         = t_auto[i];
      t_xpad[i].joykey  = (i < RARCH_FIRST_CUSTOM_BIND) ? xpad[i] : NO_BTN;
      t_xpad[i].joyaxis = AXIS_NONE;
      t_auto_own[i]     = t_auto[i];
   }
   t_xpad[RETRO_DEVICE_ID_JOYPAD_L2].joyaxis = AXIS_POS(2);
   for (i = 0; i < RARCH_BIND_LIST_END; i++)
      t_xpad_ab[i] = t_xpad[i];
   t_xpad_ab[RETRO_DEVICE_ID_JOYPAD_A].joykey = 1;
   t_xpad_ab[RETRO_DEVICE_ID_JOYPAD_B].joykey = 0;
   t_auto_own[RARCH_MENU_TOGGLE].joykey      = T_OWN_MENU;
   t_auto_own[RARCH_FAST_FORWARD_KEY].joykey = T_OWN_FF;
   for (u = 0; u < MAX_USERS; u++)
   {
      input_config_clear_device_name(u);
      input_config_clear_device_display_name(u);
      input_config_plays_as_release(u);
      input_config_set_device_vid(u, 0);
      input_config_set_device_pid(u, 0);
      settings.uints.input_device_reservation_type[u] =
         INPUT_DEVICE_RESERVATION_NONE;
      settings.arrays.input_reserved_devices[u][0]    = '\0';
      t_axis_held[u]    = AXIS_NONE;
      t_button_reads[u] = 0;
   }
   for (u = 0; u < MAX_USERS; u++)
   {
      for (i = 0; i < RARCH_BIND_LIST_END; i++)
      {
         input_config_binds[u][i].joykey  = NO_BTN;
         input_config_binds[u][i].joyaxis = AXIS_NONE;
         input_config_binds[u][i].attr    =
            RETRO_KEYBIND_ATTR(RETROK_UNKNOWN, NO_BTN, false);
      }
      input_autoconf_binds[u]                 = t_auto;
      settings.uints.input_joypad_index[u]    = u;
      settings.uints.input_remap_ports[u]     = u;
      settings.uints.input_libretro_device[u] = RETRO_DEVICE_JOYPAD;
      t_buttons[u]                            = 0;
      t_rumbled[u]                            = 0;
   }
   settings.uints.input_max_users                 = 4;
   settings.uints.input_rumble_gain               = 100;
   settings.uints.input_menu_toggle_gamepad_combo = INPUT_COMBO_NONE;
   settings.floats.input_axis_threshold           = 0.5f;
   settings.bools.input_hotkey_follows_player1    = false;
   settings.bools.input_all_users_control_menu    = false;
   st->primary_joypad                             = &t_joypad;
   t_menu_st.flags                               &= ~MENU_ST_FLAG_ALIVE;
   input_plays_as_read(NULL);
   input_remapping_update_port_map();
}

/* User @user on core port @port (MAX_USERS: None). */
static void t_map(unsigned user, unsigned port)
{
   settings.uints.input_remap_ports[user] = port;
   input_remapping_update_port_map();
}

/* Core port @port's buttons, read as a new frame's: the poll forgets
 * the frame's view, which would answer what an earlier read saw. */
static unsigned t_read(unsigned port)
{
   input_driver_state_t *st = input_state_get_ptr();
   memset(&st->frame_valid, 0, sizeof(st->frame_valid));
   return (uint16_t)input_driver_state_wrapper(port, RETRO_DEVICE_JOYPAD,
         0, RETRO_DEVICE_ID_JOYPAD_MASK);
}

/* Core port @port's strong motor at @strength, written as after a
 * frame; returns the slots that write reached, as bits. */
static unsigned t_rumble_port(unsigned port, uint16_t strength)
{
   unsigned s;
   unsigned slots = 0;
   memset(t_rumbled, 0, sizeof(t_rumbled));
   input_set_rumble_state(port, RETRO_RUMBLE_STRONG, strength);
   input_driver_flush_rumble();
   for (s = 0; s < MAX_USERS; s++)
      if (t_rumbled[s])
         slots |= 1u << s;
   return slots;
}

/* What core port @port's strong motor answers once written. */
static bool t_rumble_answer(unsigned port)
{
   input_set_rumble_state(port, RETRO_RUMBLE_STRONG, 0);
   input_driver_flush_rumble();
   return input_set_rumble_state(port, RETRO_RUMBLE_STRONG, 0);
}

/* Every user on its own core port: each port reads and rumbles its
 * own user's pad, as before. */
static void test_identity(void)
{
   t_reset();
   t_buttons[0] = T_A;
   t_buttons[3] = T_B;
   CHECK(t_read(0) == T_A && t_read(3) == T_B,
         "the identity map reads another user");
   CHECK(t_rumble_port(0, 1000) == 1u << 0,
         "the identity map: core port 1's rumble reached another slot");
   CHECK(t_rumble_port(3, 1001) == 1u << 3,
         "the identity map: core port 4's rumble reached another slot");
}

/* User 2 on Mapped Port 1: core port 1 reads and rumbles both pads;
 * core port 2, which nobody feeds, rumbles nobody. */
static void test_shared_port(void)
{
   t_reset();
   t_map(1, 0);
   t_buttons[0] = T_A;
   t_buttons[1] = T_B;
   CHECK(t_read(0) == (T_A | T_B), "core port 1 does not read both pads");
   CHECK(t_read(1) == 0, "core port 2 still reads user 2");
   CHECK(t_rumble_port(0, 1010) == ((1u << 0) | (1u << 1)),
         "core port 1's rumble did not reach users 1 and 2");
   CHECK(t_rumble_port(1, 1011) == 0,
         "core port 2's rumble reached user 2, who feeds core port 1");
   CHECK(t_rumble_answer(0), "core port 1's motor answered false");
   CHECK(!t_rumble_answer(1),
         "core port 2's motor, which reaches nobody, answered true");
}

/* A core port's motor answers true when any of its users' does. */
static void test_rumble_answer(void)
{
   t_reset();
   t_rumble_refused = 1u << 0;
   CHECK(!t_rumble_answer(0), "the identity map: a refusing motor answered true");
   t_map(1, 0);
   CHECK(t_rumble_answer(0),
         "core port 1 answered false with user 2's motor answering true");
   t_rumble_refused = (1u << 0) | (1u << 1);
   CHECK(!t_rumble_answer(0),
         "core port 1 answered true with both motors refusing");
}

/* Users 1 and 2 swapped: each core port's rumble follows its user. */
static void test_swapped_ports(void)
{
   t_reset();
   t_map(0, 1);
   t_map(1, 0);
   CHECK(t_rumble_port(0, 1020) == 1u << 1,
         "swapped: core port 1's rumble did not go to user 2 alone");
   CHECK(t_rumble_port(1, 1021) == 1u << 0,
         "swapped: core port 2's rumble did not go to user 1 alone");
}

/* Users 2 and 4 on core port 2, user 3 on None: core port 2's rumble
 * reaches users 2 and 4; core port 3's reaches nobody. */
static void test_port_2_users(void)
{
   t_reset();
   t_map(3, 1);
   t_map(2, MAX_USERS);
   CHECK(t_rumble_port(1, 1030) == ((1u << 1) | (1u << 3)),
         "core port 2's rumble did not reach users 2 and 4 alone");
   CHECK(t_rumble_port(2, 1031) == 0,
         "core port 3's rumble reached user 3, on None");
}

/* Slot @slot holds a controller called @name with @vid and @pid. */
static void t_dev(unsigned slot, const char *name, uint16_t vid,
      uint16_t pid)
{
   input_config_set_device_name(slot, name);
   input_config_set_device_display_name(slot, name);
   input_config_set_device_vid(slot, vid);
   input_config_set_device_pid(slot, pid);
}

/* Slot @slot's controller gone, as the frontend lets one go. */
static void t_disconnect(unsigned slot)
{
   input_config_clear_device_name(slot);
   input_config_clear_device_display_name(slot);
   input_config_set_device_vid(slot, 0);
   input_config_set_device_pid(slot, 0);
   input_config_plays_as_release(slot);
}

/* The Plays As entries a configuration's @text gives. */
static void t_entries(const char *text)
{
   char buf[1024];
   config_file_t *conf;
   strlcpy(buf, text, sizeof(buf));
   conf = config_file_new_from_string(buf, NULL);
   input_plays_as_read(conf);
   config_file_free(conf);
}

/* Whether @conf has @key, and as @value where that is not NULL. */
static bool t_conf_is(config_file_t *conf, const char *key,
      const char *value)
{
   char got[256];
   if (!config_get_array(conf, key, got, sizeof(got)))
      return false;
   return !value || !strcmp(got, value);
}

/* A reservation's vid:pid, as the reservations have always read it:
 * hex, a colon, hex, then a space or the end; anything else is a
 * name. */
static void test_vid_pid_parse(void)
{
   unsigned long v = 7;
   unsigned long p = 7;
   const char *n   = NULL;
   CHECK(input_config_parse_vid_pid("045e:028e Pad", &v, &p, &n)
         && v == 0x045e && p == 0x028e && n && !strcmp(n, "Pad"),
         "vid:pid name was not read as one");
   CHECK(input_config_parse_vid_pid("0000:0000", &v, &p, &n)
         && v == 0 && p == 0 && n && !*n,
         "a bare 0000:0000 was not read as one");
   v = 7;
   CHECK(!input_config_parse_vid_pid("Pad", &v, &p, NULL)
         && !input_config_parse_vid_pid("045e:Pad", &v, &p, NULL)
         && !input_config_parse_vid_pid("045e:028eX", &v, &p, NULL)
         && !input_config_parse_vid_pid("Pad 2: Turbo", &v, &p, NULL)
         && v == 7,
         "a name was read as a vid:pid");
}

/* An entry names its controller by vid:pid, or by name where it has
 * 0000:0000 or none; an entry is one controller's. */
static void test_plays_as_match(void)
{
   t_reset();
   t_dev(0, "Other Name", 0x045e, 0x028e);
   t_dev(1, "Headset", 0, 0);
   t_dev(2, "Other", 0, 0);
   t_dev(3, "Stick", 0x1234, 0x5678);
   t_entries(
         "input_plays_as_device_1 = \"045e:028e Pad\"\n"
         "input_plays_as_player_1 = \"2\"\n"
         "input_plays_as_device_2 = \"0000:0000 Headset\"\n"
         "input_plays_as_player_2 = \"1\"\n"
         "input_plays_as_device_3 = \"Stick\"\n"
         "input_plays_as_player_3 = \"3\"\n");
   CHECK(input_config_plays_as_entry(0) == 0,
         "vid:pid does not name its controller");
   CHECK(input_config_plays_as_entry(1) == 1,
         "0000:0000 does not name its controller by name");
   CHECK(input_config_plays_as_entry(2) == -1,
         "0000:0000 names every controller with no vid:pid");
   CHECK(input_config_plays_as_entry(3) == 2
         && input_config_plays_as_player(3) == 3,
         "a name alone does not name its controller");

   /* two of one model, one entry: the second plays Auto */
   t_reset();
   t_dev(0, "Pad", 0x045e, 0x028e);
   t_dev(1, "Pad", 0x045e, 0x028e);
   t_entries(
         "input_plays_as_device_1 = \"045e:028e Pad\"\n"
         "input_plays_as_player_1 = \"2\"\n");
   CHECK(input_config_plays_as_entry(0) == 0
         && input_config_plays_as_entry(1) == -1,
         "one entry was taken by two controllers");
}

/* The entries are written and read back; a free entry's keys go. */
static void test_plays_as_round_trip(void)
{
   config_file_t *conf;
   t_reset();
   t_dev(0, "Pad", 0x045e, 0x028e);
   CHECK(input_config_plays_as_set(0, 2), "Player 2 took no entry");
   conf = config_file_new_alloc();
   config_set_string(conf, "input_plays_as_device_5", "045e:0000 Gone");
   config_set_string(conf, "input_plays_as_player_5", "3");
   input_plays_as_write(conf);
   CHECK(t_conf_is(conf, "input_plays_as_device_1", "045e:028e Pad")
         && t_conf_is(conf, "input_plays_as_player_1", "2"),
         "the entry was not written as vid:pid name and player");
   CHECK(!t_conf_is(conf, "input_plays_as_device_5", NULL)
         && !t_conf_is(conf, "input_plays_as_player_5", NULL),
         "a free entry's keys were kept");
   CHECK(!t_conf_is(conf, "input_plays_as_device_2", NULL),
         "a free entry was written");

   input_plays_as_read(NULL);
   CHECK(input_config_plays_as_player(0) == 0, "the entries outlived a read");
   input_plays_as_read(conf);
   CHECK(input_config_plays_as_entry(0) == 0
         && input_config_plays_as_player(0) == 2,
         "the entry did not come back");
   config_file_free(conf);
}

/* An entry is made for a player and let go on Auto; with all sixteen
 * held by controllers that are gone, another is not made. */
static void test_plays_as_entry_life(void)
{
   unsigned e;
   char name[32];
   t_reset();
   t_dev(0, "Pad", 1, 1);
   CHECK(input_config_plays_as_set(0, 0)
         && input_config_plays_as_entry(0) < 0,
         "Auto took an entry");
   CHECK(input_config_plays_as_set(0, 2)
         && input_config_plays_as_entry(0) == 0
         && input_config_plays_as_active(),
         "Player 2 took no entry");
   CHECK(input_config_plays_as_set(0, 0)
         && input_config_plays_as_entry(0) < 0
         && !input_config_plays_as_active(),
         "an entry set back to Auto was kept");

   for (e = 0; e < MAX_USERS; e++)
   {
      snprintf(name, sizeof(name), "Gone %u", e);
      t_dev(1, name, (uint16_t)(e + 2), 1);
      input_config_plays_as_set(1, 1);
      t_disconnect(1);
   }
   CHECK(!input_config_plays_as_set(0, 3)
         && input_config_plays_as_entry(0) < 0,
         "a seventeenth entry was made");
   CHECK(input_config_plays_as_set(0, 0), "Auto needed an entry");
   CHECK(input_config_plays_as_entry_name(15)
         && !strcmp(input_config_plays_as_entry_name(15), "Gone 15")
         && !input_config_plays_as_entry_taken(15),
         "an entry of a controller that is gone is not listed by name");
   input_config_plays_as_set_entry(15, 0);
   CHECK(!input_config_plays_as_entry_name(15),
         "a gone controller's entry set to Auto was kept");
}

/* A controller in slot @slot, connected as the frontend connects one:
 * named, then placed. */
static void t_connect(unsigned slot, const char *name, uint16_t vid,
      uint16_t pid)
{
   t_dev(slot, name, vid, pid);
   input_config_plays_as_place(slot);
}

/* The user reading @slot below Max Users, or MAX_USERS. */
static unsigned t_user_of(unsigned slot)
{
   unsigned u;
   for (u = 0; u < settings.uints.input_max_users; u++)
      if (settings.uints.input_joypad_index[u] == slot)
         return u;
   return MAX_USERS;
}

/* Auto: with no entries the reservations place it, as before; with
 * entries, the first free port. */
static void test_place_auto(void)
{
   t_reset();
   t_dev(2, "Pad", 1, 1);
   CHECK(!input_config_plays_as_place(2) && t_user_of(2) == 2,
         "with no entries Plays As placed an Auto controller");
   t_entries(
         "input_plays_as_device_1 = \"Away\"\n"
         "input_plays_as_player_1 = \"4\"\n");
   CHECK(input_config_plays_as_place(2) && t_user_of(2) == 0,
         "an Auto controller is not on the first free port");
}

/* Player N: a free port N is its; an Auto controller there moves to
 * the first free port; another of player N's makes it share, from the
 * highest free port below Max Users. */
static void test_place_player(void)
{
   t_reset();
   t_entries(
         "input_plays_as_device_1 = \"0001:0001 A\"\n"
         "input_plays_as_player_1 = \"2\"\n");
   t_connect(0, "A", 1, 1);
   CHECK(t_user_of(0) == 1, "Player 2 did not take a free port 2");

   t_reset();
   t_entries(
         "input_plays_as_device_1 = \"0002:0002 B\"\n"
         "input_plays_as_player_1 = \"1\"\n");
   t_connect(0, "Auto", 9, 9);
   CHECK(t_user_of(0) == 0, "the Auto controller is not on port 1");
   t_connect(1, "B", 2, 2);
   CHECK(t_user_of(1) == 0 && t_user_of(0) == 1,
         "Player 1 did not take port 1 from an Auto controller");

   t_reset();
   t_entries(
         "input_plays_as_device_1 = \"0001:0001 A\"\n"
         "input_plays_as_player_1 = \"1\"\n"
         "input_plays_as_device_2 = \"0002:0002 B\"\n"
         "input_plays_as_player_2 = \"1\"\n");
   t_connect(0, "A", 1, 1);
   t_connect(1, "B", 2, 2);
   CHECK(t_user_of(0) == 0 && t_user_of(1) == 3,
         "a second Player 1 does not share from the highest free port");
   CHECK(settings.uints.input_joypad_index[1] == 3,
         "the port the sharer left does not read the slot it came from");
}

/* A controller a reservation put on port N is player N's; an explicit
 * player above Max Users plays Auto; with no free port a sharer gets
 * none. */
static void test_place_limits(void)
{
   t_reset();
   settings.uints.input_device_reservation_type[0] =
      INPUT_DEVICE_RESERVATION_PREFERRED;
   strlcpy(settings.arrays.input_reserved_devices[0], "0005:0005 Res",
         sizeof(settings.arrays.input_reserved_devices[0]));
   t_entries(
         "input_plays_as_device_1 = \"0002:0002 B\"\n"
         "input_plays_as_player_1 = \"1\"\n");
   t_dev(0, "Res", 5, 5);
   CHECK(!input_config_plays_as_place(0),
         "an Auto controller was placed with a reservation set");
   t_connect(1, "B", 2, 2);
   CHECK(t_user_of(0) == 0 && t_user_of(1) == 3,
         "Player 1 did not share with the reserved controller on port 1");

   t_reset();
   t_entries(
         "input_plays_as_device_1 = \"0002:0002 B\"\n"
         "input_plays_as_player_1 = \"5\"\n");
   t_connect(0, "A", 1, 1);
   t_connect(1, "B", 2, 2);
   CHECK(t_user_of(0) == 0 && t_user_of(1) == 1,
         "Player 5 with Max Users 4 did not play Auto");

   t_reset();
   settings.uints.input_max_users = 2;
   t_entries(
         "input_plays_as_device_1 = \"0001:0001 A\"\n"
         "input_plays_as_player_1 = \"1\"\n"
         "input_plays_as_device_2 = \"0002:0002 B\"\n"
         "input_plays_as_player_2 = \"1\"\n");
   t_connect(0, "A", 1, 1);
   t_connect(1, "C", 3, 3);
   t_connect(2, "B", 2, 2);
   CHECK(t_user_of(0) == 0 && t_user_of(1) == 1 && t_user_of(2) == MAX_USERS,
         "a sharer with no free port took one");
}

/* An explicit player beats a reservation naming the controller; a
 * reservation names as it places, a 0000:0000 one every controller
 * with no vid:pid. */
static void test_place_reservation(void)
{
   t_reset();
   settings.uints.input_device_reservation_type[0] =
      INPUT_DEVICE_RESERVATION_PREFERRED;
   strlcpy(settings.arrays.input_reserved_devices[0], "0005:0005 Res",
         sizeof(settings.arrays.input_reserved_devices[0]));
   t_dev(0, "Res", 5, 5);
   t_entries(
         "input_plays_as_device_1 = \"0005:0005 Res\"\n"
         "input_plays_as_player_1 = \"3\"\n"
         "input_plays_as_device_2 = \"0002:0002 B\"\n"
         "input_plays_as_player_2 = \"1\"\n");
   t_connect(1, "B", 2, 2);
   CHECK(t_user_of(1) == 0 && t_user_of(0) == 1,
         "a Player 3 controller that port 1's reservation names kept "
         "port 1 from Player 1");

   t_reset();
   settings.uints.input_device_reservation_type[0] =
      INPUT_DEVICE_RESERVATION_PREFERRED;
   strlcpy(settings.arrays.input_reserved_devices[0], "0000:0000 Res",
         sizeof(settings.arrays.input_reserved_devices[0]));
   t_entries(
         "input_plays_as_device_1 = \"0002:0002 B\"\n"
         "input_plays_as_player_1 = \"1\"\n");
   t_dev(0, "Other", 0, 0);
   t_connect(1, "B", 2, 2);
   CHECK(t_user_of(0) == 0 && t_user_of(1) == 3,
         "a 0000:0000 reservation did not name an unnumbered controller "
         "it places");
}

/* Port 1 reserved for one controller, another set to Player 1: either
 * order, the first has port 1 and the second shares. */
static void test_place_reservation_order(void)
{
   t_reset();
   settings.uints.input_device_reservation_type[0] =
      INPUT_DEVICE_RESERVATION_PREFERRED;
   strlcpy(settings.arrays.input_reserved_devices[0], "0005:0005 Res",
         sizeof(settings.arrays.input_reserved_devices[0]));
   t_entries(
         "input_plays_as_device_1 = \"0002:0002 B\"\n"
         "input_plays_as_player_1 = \"1\"\n");
   t_dev(0, "Res", 5, 5);
   CHECK(!input_config_plays_as_place(0), "the reservation did not place");
   t_connect(1, "B", 2, 2);
   CHECK(t_user_of(0) == 0 && t_user_of(1) == 3,
         "reserved first: Player 1 did not share");

   t_reset();
   settings.uints.input_device_reservation_type[0] =
      INPUT_DEVICE_RESERVATION_PREFERRED;
   strlcpy(settings.arrays.input_reserved_devices[0], "0005:0005 Res",
         sizeof(settings.arrays.input_reserved_devices[0]));
   t_entries(
         "input_plays_as_device_1 = \"0002:0002 B\"\n"
         "input_plays_as_player_1 = \"1\"\n");
   t_connect(0, "B", 2, 2);
   t_dev(1, "Res", 5, 5);
   CHECK(input_config_plays_as_place(1)
         && t_user_of(0) == 0 && t_user_of(1) == 3,
         "Player 1 first: the reserved controller did not share");

   /* twins reserved for ports 1 and 2, one set to Player 1: the other
    * goes to port 2 as the reservations give it */
   t_reset();
   settings.uints.input_device_reservation_type[0] =
      INPUT_DEVICE_RESERVATION_PREFERRED;
   settings.uints.input_device_reservation_type[1] =
      INPUT_DEVICE_RESERVATION_PREFERRED;
   strlcpy(settings.arrays.input_reserved_devices[0], "0005:0005 Res",
         sizeof(settings.arrays.input_reserved_devices[0]));
   strlcpy(settings.arrays.input_reserved_devices[1], "0005:0005 Res",
         sizeof(settings.arrays.input_reserved_devices[1]));
   t_entries(
         "input_plays_as_device_1 = \"0005:0005 Res\"\n"
         "input_plays_as_player_1 = \"1\"\n");
   t_connect(0, "Res", 5, 5);
   t_dev(1, "Res", 5, 5);
   CHECK(!input_config_plays_as_place(1)
         && t_user_of(0) == 0 && t_user_of(1) == 1,
         "a twin reserved for port 2 shared Player 1");
}

/* The controller on port N leaving: its sharers stay; the next Auto
 * controller takes port N. A player set and the controllers connected
 * again place them by it. */
static void test_place_leave_and_change(void)
{
   t_reset();
   t_entries(
         "input_plays_as_device_1 = \"0001:0001 A\"\n"
         "input_plays_as_player_1 = \"1\"\n"
         "input_plays_as_device_2 = \"0002:0002 B\"\n"
         "input_plays_as_player_2 = \"1\"\n");
   t_connect(0, "A", 1, 1);
   t_connect(1, "B", 2, 2);
   t_disconnect(0);
   CHECK(t_user_of(1) == 3, "the sharer moved when port 1 emptied");
   t_connect(0, "Auto", 9, 9);
   CHECK(t_user_of(0) == 0, "an Auto controller did not take the empty port 1");

   t_reset();
   t_connect(0, "A", 1, 1);
   t_connect(1, "B", 2, 2);
   CHECK(input_config_plays_as_set(1, 1), "Player 1 took no entry");
   input_config_plays_as_place(0);
   input_config_plays_as_place(1);
   CHECK(t_user_of(1) == 0 && t_user_of(0) == 1,
         "connected again, the controller set to Player 1 is not on port 1");
}

/* Entries that give no player within Max Users: Auto controllers are
 * placed as with no entries, a Device Index swap kept, and a driver
 * restart still puts each controller back on its port. */
static void test_place_no_player(void)
{
   t_reset();
   settings.uints.input_joypad_index[0] = 1;
   settings.uints.input_joypad_index[1] = 0;
   t_entries(
         "input_plays_as_device_1 = \"0009:0009 Away\"\n"
         "input_plays_as_player_1 = \"0\"\n"
         "input_plays_as_device_2 = \"0002:0002 B\"\n"
         "input_plays_as_player_2 = \"5\"\n");
   CHECK(input_config_plays_as_active(), "the entries are not in use");
   t_dev(0, "A", 1, 1);
   CHECK(!input_config_plays_as_place(0) && t_user_of(0) == 1,
         "an entry with no player undid a Device Index swap");
   t_dev(2, "C", 3, 3);
   CHECK(!input_config_plays_as_place(2) && t_user_of(2) == 2,
         "an entry with no player moved an Auto controller");
   t_connect(1, "B", 2, 2);
   CHECK(t_user_of(1) == 0 && t_user_of(0) == 1,
         "Player 5 with Max Users 4 was placed");

   t_reset();
   t_entries(
         "input_plays_as_device_1 = \"0001:0001 A\"\n"
         "input_plays_as_player_1 = \"0\"\n");
   input_driver_registry_connect(0, "x", "A", "", 1, 1);
   input_driver_registry_connect(1, "x", "B", "", 2, 2);
   input_driver_registry_restart();
   input_driver_registry_connect(0, "x", "B", "", 2, 2);
   input_driver_registry_connect(1, "x", "A", "", 1, 1);
   CHECK(settings.uints.input_joypad_index[0] == 1
         && settings.uints.input_joypad_index[1] == 0,
         "an entry with no player kept a restart from restoring the ports");
   input_driver_registry_disconnect(0);
   input_driver_registry_disconnect(1);
}

/* t_connect, with the registry told as the connect callback tells it. */
static void t_plug(unsigned slot, const char *name, uint16_t vid,
      uint16_t pid)
{
   t_connect(slot, name, vid, pid);
   input_driver_registry_connect(slot, "x", name, "", vid, pid);
}

/* A driver restart that reports the pads in other slots with no
 * disconnects between: the Player 1 pad keeps its entry and port 1. */
static void test_place_restart(void)
{
   t_reset();
   t_entries(
         "input_plays_as_device_1 = \"0002:0002 B\"\n"
         "input_plays_as_player_1 = \"1\"\n");
   t_plug(0, "A", 1, 1);
   t_plug(1, "B", 2, 2);
   input_driver_registry_restart();
   t_plug(0, "B", 2, 2);
   t_plug(1, "A", 1, 1);
   CHECK(t_user_of(0) == 0 && input_config_plays_as_player(0) == 1
         && t_user_of(1) == 1,
         "after a restart the Player 1 pad lost its entry or port 1");
   input_driver_registry_disconnect(0);
   input_driver_registry_disconnect(1);
}

/* The reservations writing the ports after a Plays As move, as
 * reallocate_port_if_needed() hands them on: the config file still
 * gets the ports the user set. */
static void test_place_saved_ports(void)
{
   unsigned u;
   bool runtime;
   bool configured = true;
   t_reset();
   t_entries(
         "input_plays_as_device_1 = \"0002:0002 B\"\n"
         "input_plays_as_player_1 = \"2\"\n");
   t_connect(0, "B", 2, 2);
   runtime = input_driver_ports_runtime();
   settings.uints.input_joypad_index[2] = 3;
   settings.uints.input_joypad_index[3] = 2;
   if (runtime)
      input_driver_ports_runtime_moved();
   for (u = 0; u < MAX_USERS; u++)
      if (input_config_get_saved_joypad_index(u) != u)
         configured = false;
   CHECK(runtime && t_user_of(0) == 1 && configured,
         "a reservation's write saved the ports Plays As moved");
}

/* A load made only to compare against, as saving an override makes
 * one, leaves the entries and claims as they were once the copy taken
 * before it is put back. */
static void test_plays_as_copy(void)
{
   input_plays_as_copy_t *copy;
   t_reset();
   t_dev(0, "Pad", 0x045e, 0x028e);
   t_dev(1, "Stick", 0x1234, 0x5678);
   CHECK(input_config_plays_as_set(1, 3), "Player 3 took no entry");
   copy = input_plays_as_copy_new();
   CHECK(copy, "no copy of the entries");
   if (!copy)
      return;
   t_entries(
         "input_plays_as_device_1 = \"045e:028e Pad\"\n"
         "input_plays_as_player_1 = \"2\"\n");
   CHECK(input_config_plays_as_entry(0) == 0
         && input_config_plays_as_entry(1) == -1,
         "the load did not read the file's entries");
   input_plays_as_copy_restore(copy);
   input_plays_as_copy_free(copy);
   CHECK(input_config_plays_as_entry(0) == -1
         && input_config_plays_as_entry(1) == 0
         && input_config_plays_as_player(1) == 3
         && input_config_plays_as_entry_name(0)
         && !strcmp(input_config_plays_as_entry_name(0), "Stick")
         && !input_config_plays_as_entry_name(1),
         "the entries read to compare against outlived the copy");
}

#define T_ENABLE 20
#define T_FF     21
#define T_MENU   22
#define T_Y      T_BIT(RETRO_DEVICE_ID_JOYPAD_Y)

/* A frame's system input, the block flags cleared first as the run
 * loop clears them. */
static void t_collect(input_bits_t *bits)
{
   input_driver_state_t *st = input_state_get_ptr();
   st->flags &= ~(INP_FLAG_BLOCK_LIBRETRO_INPUT | INP_FLAG_BLOCK_HOTKEY);
   memset(bits, 0, sizeof(*bits));
   input_driver_collect_system_input(st, &settings, bits);
}

/* User 1's hotkey @id on button @joykey of any controller that reads
 * it; counted as a change, as the menu's bind settings count it. */
static void t_bind(unsigned id, uint16_t joykey)
{
   input_config_binds[0][id].joykey = joykey;
   input_config_binds[0][id].attr   =
      RETRO_KEYBIND_ATTR(RETROK_UNKNOWN, NO_BTN, true);
   input_config_binds_changed();
}

/* With the menu up: user 1 wherever it is mapped, and the users on
 * its core port; never a user on None. */
static void test_menu_shared(void)
{
   input_bits_t bits;
   t_reset();
   t_menu_st.flags |= MENU_ST_FLAG_ALIVE;
   t_buttons[1] = T_B;
   t_collect(&bits);
   CHECK(!BIT256_GET(bits, RETRO_DEVICE_ID_JOYPAD_B),
         "the identity map: user 2 drives the menu");

   t_map(1, 0);
   t_collect(&bits);
   CHECK(BIT256_GET(bits, RETRO_DEVICE_ID_JOYPAD_B),
         "user 2 on core port 1 does not drive the menu");
   t_buttons[1] = 0;
   t_buttons[0] = T_A;
   t_collect(&bits);
   CHECK(BIT256_GET(bits, RETRO_DEVICE_ID_JOYPAD_A),
         "user 1 no longer drives the menu");
   t_buttons[0] = 0;
   t_buttons[2] = T_Y;
   t_collect(&bits);
   CHECK(!BIT256_GET(bits, RETRO_DEVICE_ID_JOYPAD_Y),
         "user 3, on its own core port, drives the menu");

   /* swapped: user 1 on core port 2, user 2 on core port 1 */
   t_map(0, 1);
   t_buttons[2] = 0;
   t_buttons[0] = T_A;
   t_collect(&bits);
   CHECK(BIT256_GET(bits, RETRO_DEVICE_ID_JOYPAD_A),
         "swapped: user 1 no longer drives the menu");
   t_buttons[0] = 0;
   t_buttons[1] = T_B;
   t_collect(&bits);
   CHECK(!BIT256_GET(bits, RETRO_DEVICE_ID_JOYPAD_B),
         "swapped: user 2, on core port 1, drives the menu");

   /* both on None: nothing is shared */
   t_map(0, MAX_USERS);
   t_map(1, MAX_USERS);
   t_collect(&bits);
   CHECK(!BIT256_GET(bits, RETRO_DEVICE_ID_JOYPAD_B),
         "user 2 on None drives the menu with user 1 on None");
   t_buttons[1] = 0;
   t_buttons[0] = T_A;
   t_collect(&bits);
   CHECK(BIT256_GET(bits, RETRO_DEVICE_ID_JOYPAD_A),
         "user 1 on None no longer drives the menu");
   t_menu_st.flags &= ~MENU_ST_FLAG_ALIVE;
}

/* The users on user 1's core port fire user 1's hotkeys; no other user
 * does, and a user on None shares nothing. */
static void test_hotkeys_shared(void)
{
   input_bits_t bits;
   t_reset();
   t_bind(RARCH_FAST_FORWARD_KEY, T_FF);
   t_buttons[1] = T_BIT(T_FF);
   t_collect(&bits);
   CHECK(!BIT256_GET(bits, RARCH_FAST_FORWARD_KEY),
         "the identity map: user 2 fires user 1's hotkeys");

   t_map(1, 0);
   t_collect(&bits);
   CHECK(BIT256_GET(bits, RARCH_FAST_FORWARD_KEY),
         "user 2 on core port 1 does not fire user 1's hotkeys");
   t_buttons[1] = 0;
   t_buttons[0] = T_BIT(T_FF);
   t_collect(&bits);
   CHECK(BIT256_GET(bits, RARCH_FAST_FORWARD_KEY),
         "user 1's pad lost its hotkeys to a shared core port");
   t_buttons[0] = 0;
   t_buttons[2] = T_BIT(T_FF);
   t_collect(&bits);
   CHECK(!BIT256_GET(bits, RARCH_FAST_FORWARD_KEY),
         "user 3, on its own core port, fires user 1's hotkeys");

   t_map(0, 1);
   t_buttons[2] = 0;
   t_buttons[1] = T_BIT(T_FF);
   t_collect(&bits);
   CHECK(!BIT256_GET(bits, RARCH_FAST_FORWARD_KEY),
         "swapped: user 2 fires user 1's hotkeys");

   t_map(0, MAX_USERS);
   t_map(1, MAX_USERS);
   t_collect(&bits);
   CHECK(!BIT256_GET(bits, RARCH_FAST_FORWARD_KEY),
         "user 2 on None fires user 1's hotkeys with user 1 on None");
}

/* Hotkey Enable goes with the hotkeys of the controller it is held on;
 * held on any of them, it holds game input back. */
static void test_hotkey_enable_shared(void)
{
   input_bits_t bits;
   input_driver_state_t *st = input_state_get_ptr();

   t_reset();
   t_bind(RARCH_ENABLE_HOTKEY, T_ENABLE);
   t_bind(RARCH_FAST_FORWARD_KEY, T_FF);
   t_buttons[3] = T_BIT(T_ENABLE) | T_BIT(T_FF);
   t_collect(&bits);
   CHECK(!BIT256_GET(bits, RARCH_FAST_FORWARD_KEY)
         && !(st->flags & INP_FLAG_BLOCK_LIBRETRO_INPUT),
         "the identity map: user 4's pad has user 1's hotkeys");

   t_map(1, 0);
   t_buttons[3] = 0;
   t_buttons[1] = T_BIT(T_ENABLE) | T_BIT(T_FF);
   t_collect(&bits);
   CHECK(BIT256_GET(bits, RARCH_FAST_FORWARD_KEY),
         "Hotkey Enable and a hotkey on user 2's pad do not fire it");
   CHECK(st->flags & INP_FLAG_BLOCK_LIBRETRO_INPUT,
         "Hotkey Enable on user 2's pad does not hold game input back");

   t_buttons[1] = T_BIT(T_FF);
   t_collect(&bits);
   CHECK(!BIT256_GET(bits, RARCH_FAST_FORWARD_KEY),
         "a hotkey on user 2's pad fires without Hotkey Enable");

   t_buttons[1] = 0;
   t_buttons[0] = T_BIT(T_ENABLE) | T_BIT(T_FF);
   t_collect(&bits);
   CHECK(BIT256_GET(bits, RARCH_FAST_FORWARD_KEY),
         "Hotkey Enable and a hotkey on user 1's pad do not fire it");
   t_buttons[0] = T_BIT(T_FF);
   t_collect(&bits);
   CHECK(!BIT256_GET(bits, RARCH_FAST_FORWARD_KEY),
         "a hotkey on user 1's pad fires without Hotkey Enable");

   t_buttons[0] = T_BIT(T_ENABLE);
   t_buttons[1] = T_BIT(T_FF);
   t_collect(&bits);
   CHECK(!BIT256_GET(bits, RARCH_FAST_FORWARD_KEY),
         "Hotkey Enable on user 1's pad fires user 2's hotkey");

   t_buttons[0] = T_BIT(T_FF);
   t_buttons[1] = T_BIT(T_ENABLE);
   t_collect(&bits);
   CHECK(!BIT256_GET(bits, RARCH_FAST_FORWARD_KEY),
         "Hotkey Enable on user 2's pad fires user 1's hotkey");
}

/* The menu toggle's button, which acts on being let go, and its
 * controller combination work from either pad on core port 1. */
static void test_menu_toggle_shared(void)
{
   input_bits_t bits;
   t_reset();
   t_bind(RARCH_MENU_TOGGLE, T_MENU);
   settings.uints.input_menu_toggle_gamepad_combo = INPUT_COMBO_START_SELECT;
   t_map(1, 0);

   t_buttons[1] = T_BIT(T_MENU);
   t_collect(&bits);
   t_buttons[1] = 0;
   t_collect(&bits);
   CHECK(BIT256_GET(bits, RARCH_MENU_TOGGLE),
         "user 2's menu toggle button does not open the menu");
   t_buttons[0] = T_BIT(T_MENU);
   t_collect(&bits);
   t_buttons[0] = 0;
   t_collect(&bits);
   CHECK(BIT256_GET(bits, RARCH_MENU_TOGGLE),
         "user 1's menu toggle button does not open the menu");

   /* a button no RetroPad button shares needs no Hotkey Enable */
   t_bind(RARCH_ENABLE_HOTKEY, T_ENABLE);
   t_buttons[1] = T_BIT(T_MENU);
   t_collect(&bits);
   t_buttons[1] = 0;
   t_collect(&bits);
   CHECK(BIT256_GET(bits, RARCH_MENU_TOGGLE),
         "user 2's menu toggle button waits for Hotkey Enable");

   t_menu_st.flags |= MENU_ST_FLAG_ALIVE;
   t_buttons[1] = T_BIT(RETRO_DEVICE_ID_JOYPAD_START)
      | T_BIT(RETRO_DEVICE_ID_JOYPAD_SELECT);
   t_collect(&bits);
   CHECK(BIT256_GET(bits, RARCH_MENU_TOGGLE),
         "the menu toggle combination on user 2's pad does not close the menu");
   t_buttons[1] = 0;
   t_buttons[0] = T_BIT(RETRO_DEVICE_ID_JOYPAD_START)
      | T_BIT(RETRO_DEVICE_ID_JOYPAD_SELECT);
   t_collect(&bits);
   CHECK(BIT256_GET(bits, RARCH_MENU_TOGGLE),
         "the menu toggle combination on user 1's pad does not close the menu");
   t_menu_st.flags &= ~MENU_ST_FLAG_ALIVE;
}

/* Hotkeys Follow Player 1: the hotkeys are core port 1's users',
 * wherever user 1 is. */
static void test_hotkeys_follow(void)
{
   input_bits_t bits;
   t_reset();
   settings.bools.input_hotkey_follows_player1 = true;
   t_bind(RARCH_FAST_FORWARD_KEY, T_FF);
   t_map(1, 0);

   t_buttons[1] = T_BIT(T_FF);
   t_collect(&bits);
   CHECK(BIT256_GET(bits, RARCH_FAST_FORWARD_KEY),
         "Follow Player 1: user 2 on core port 1 does not fire the hotkeys");
   t_buttons[1] = 0;
   t_buttons[0] = T_BIT(T_FF);
   t_collect(&bits);
   CHECK(BIT256_GET(bits, RARCH_FAST_FORWARD_KEY),
         "Follow Player 1: user 1's pad does not fire the hotkeys");

   /* user 1 moves to core port 3 */
   t_map(0, 2);
   t_collect(&bits);
   CHECK(!BIT256_GET(bits, RARCH_FAST_FORWARD_KEY),
         "Follow Player 1: user 1's pad on core port 3 fires the hotkeys");
   t_buttons[0] = 0;
   t_buttons[1] = T_BIT(T_FF);
   t_collect(&bits);
   CHECK(BIT256_GET(bits, RARCH_FAST_FORWARD_KEY),
         "Follow Player 1: user 2 alone on core port 1 does not fire them");
}

/* User 1 on None: core port 1's users drive the menu and fire user 1's
 * hotkeys, each by its own Hotkey Enable, and user 1 keeps both. */
static void test_none_player1(void)
{
   input_bits_t bits;
   t_reset();
   t_bind(RARCH_ENABLE_HOTKEY, T_ENABLE);
   t_bind(RARCH_FAST_FORWARD_KEY, T_FF);
   t_map(0, MAX_USERS);
   t_map(1, 0);

   t_menu_st.flags |= MENU_ST_FLAG_ALIVE;
   t_buttons[1] = T_B;
   t_collect(&bits);
   CHECK(BIT256_GET(bits, RETRO_DEVICE_ID_JOYPAD_B),
         "user 1 on None: user 2 on core port 1 does not drive the menu");
   t_buttons[1] = 0;
   t_buttons[2] = T_Y;
   t_collect(&bits);
   CHECK(!BIT256_GET(bits, RETRO_DEVICE_ID_JOYPAD_Y),
         "user 1 on None: user 3, on its own core port, drives the menu");
   t_buttons[2] = 0;
   t_buttons[0] = T_A;
   t_collect(&bits);
   CHECK(BIT256_GET(bits, RETRO_DEVICE_ID_JOYPAD_A),
         "user 1 on None no longer drives the menu");
   t_buttons[0] = 0;
   t_menu_st.flags &= ~MENU_ST_FLAG_ALIVE;

   t_buttons[1] = T_BIT(T_ENABLE) | T_BIT(T_FF);
   t_collect(&bits);
   CHECK(BIT256_GET(bits, RARCH_FAST_FORWARD_KEY),
         "user 1 on None: Hotkey Enable and a hotkey on user 2's pad do "
         "not fire it");
   t_buttons[1] = T_BIT(T_FF);
   t_collect(&bits);
   CHECK(!BIT256_GET(bits, RARCH_FAST_FORWARD_KEY),
         "user 1 on None: a hotkey on user 2's pad fires without Hotkey "
         "Enable");
   t_buttons[1] = 0;
   t_buttons[2] = T_BIT(T_ENABLE) | T_BIT(T_FF);
   t_collect(&bits);
   CHECK(!BIT256_GET(bits, RARCH_FAST_FORWARD_KEY),
         "user 1 on None: user 3, on its own core port, fires user 1's "
         "hotkeys");
   t_buttons[2] = 0;
   t_buttons[0] = T_BIT(T_ENABLE) | T_BIT(T_FF);
   t_collect(&bits);
   CHECK(BIT256_GET(bits, RARCH_FAST_FORWARD_KEY),
         "user 1 on None lost its hotkeys");
}

#define T_X      T_BIT(RETRO_DEVICE_ID_JOYPAD_X)
#define T_SELECT T_BIT(RETRO_DEVICE_ID_JOYPAD_SELECT)
#define T_START  T_BIT(RETRO_DEVICE_ID_JOYPAD_START)

/* A frame with the buttons as set, then one with all let go: whether
 * the menu toggles as they are let go. */
static bool t_menu_tap(void)
{
   input_bits_t bits;
   unsigned u;
   t_collect(&bits);
   for (u = 0; u < MAX_USERS; u++)
      t_buttons[u] = 0;
   t_collect(&bits);
   return BIT256_GET(bits, RARCH_MENU_TOGGLE) != 0;
}

/* Menu toggle on RetroPad X, Hotkey Enable on Select: X is a game
 * button, and opens the menu only with Select on the same pad. */
static void test_menu_toggle_game_button(void)
{
   t_reset();
   t_bind(RARCH_ENABLE_HOTKEY, RETRO_DEVICE_ID_JOYPAD_SELECT);
   t_bind(RARCH_MENU_TOGGLE, RETRO_DEVICE_ID_JOYPAD_X);
   t_buttons[0] = T_X;
   CHECK(!t_menu_tap(), "the identity map: X on user 1's pad opens the menu");
   t_buttons[0] = T_SELECT | T_X;
   CHECK(t_menu_tap(),
         "the identity map: Select+X on user 1's pad does not open the menu");

   t_map(1, 0);
   t_buttons[1] = T_X;
   CHECK(!t_menu_tap(), "X on user 2's pad opens the menu");
   t_buttons[1] = T_SELECT | T_X;
   CHECK(t_menu_tap(), "Select+X on user 2's pad does not open the menu");
   t_buttons[0] = T_X;
   CHECK(!t_menu_tap(), "X on user 1's pad opens the menu, user 2 sharing");
   t_buttons[0] = T_SELECT | T_X;
   CHECK(t_menu_tap(),
         "Select+X on user 1's pad does not open the menu, user 2 sharing");
   t_buttons[0] = T_SELECT;
   t_buttons[1] = T_X;
   CHECK(!t_menu_tap(), "Select on user 1's pad and X on user 2's open the menu");
   t_buttons[0] = T_X;
   t_buttons[1] = T_SELECT;
   CHECK(!t_menu_tap(), "Select on user 2's pad and X on user 1's open the menu");
}

/* The menu toggle combination may be held across the pads on core
 * port 1. */
static void test_menu_combo_split(void)
{
   input_bits_t bits;
   t_reset();
   settings.uints.input_menu_toggle_gamepad_combo = INPUT_COMBO_START_SELECT;
   t_map(1, 0);
   t_buttons[0] = T_START;
   t_buttons[1] = T_SELECT;
   t_collect(&bits);
   CHECK(BIT256_GET(bits, RARCH_MENU_TOGGLE),
         "Start on user 1's pad and Select on user 2's do not open the menu");
   t_buttons[0] = 0;
   t_buttons[1] = 0;
   t_collect(&bits);
}

/* A user sharing core port 1 fires user 1's hotkeys with the menu up. */
static void test_hotkeys_shared_menu(void)
{
   input_bits_t bits;
   t_reset();
   t_bind(RARCH_FAST_FORWARD_KEY, T_FF);
   t_map(1, 0);
   t_menu_st.flags |= MENU_ST_FLAG_ALIVE;
   t_buttons[1] = T_BIT(T_FF);
   t_collect(&bits);
   CHECK(BIT256_GET(bits, RARCH_FAST_FORWARD_KEY),
         "with the menu up, user 2 on core port 1 does not fire user 1's hotkeys");
   t_menu_st.flags &= ~MENU_ST_FLAG_ALIVE;
}

/* the key held on the keyboard, or 0 */
static unsigned t_key;
/* A keyboard driver asked for each bind; @binds is input_config_binds,
 * its pointers cast. */
static int16_t t_kb_state(void *data, const input_device_driver_t *joypad,
      rarch_joypad_info_t *joypad_info, const retro_keybind_set *binds,
      unsigned port, unsigned device, unsigned idx, unsigned id)
{
   (void)data;
   (void)joypad;
   (void)joypad_info;
   (void)binds;
   (void)idx;
   if (!t_key || port >= MAX_USERS)
      return 0;
   if (device == RETRO_DEVICE_KEYBOARD)
      return id == t_key;
   return device == RETRO_DEVICE_JOYPAD && id < RARCH_BIND_LIST_END
      && RETRO_KEYBIND_KEY(&input_config_binds[port][id]) == t_key;
}
static input_driver_t t_kb;

/* Hotkey Enable on the keyboard and on the pads: held on the keyboard,
 * it opens the hotkeys of every pad on core port 1. */
static void test_hotkey_enable_keyboard(void)
{
   input_bits_t bits;
   input_driver_state_t *st = input_state_get_ptr();
   t_reset();
   t_kb.input_state   = t_kb_state;
   st->current_driver = &t_kb;
   t_bind(RARCH_ENABLE_HOTKEY, T_ENABLE);
   input_config_binds[0][RARCH_ENABLE_HOTKEY].attr =
      RETRO_KEYBIND_ATTR(RETROK_TAB, NO_BTN, true);
   t_bind(RARCH_FAST_FORWARD_KEY, T_FF);
   t_map(1, 0);

   t_buttons[1] = T_BIT(T_FF);
   t_collect(&bits);
   CHECK(!BIT256_GET(bits, RARCH_FAST_FORWARD_KEY),
         "a hotkey on user 2's pad fires with no Hotkey Enable held");
   t_key = RETROK_TAB;
   t_collect(&bits);
   CHECK(BIT256_GET(bits, RARCH_FAST_FORWARD_KEY),
         "Hotkey Enable on the keyboard does not open user 2's hotkeys");
   t_key              = 0;
   st->current_driver = NULL;
}

#define T_XPAD_SELECT 6
#define T_XPAD_X      2
#define T_XPAD_Y      3
#define T_GUIDE       8

/* Slot @slot's controller: its profile and its name, counted as a
 * change, as a connect is. */
static void t_pad(unsigned slot, struct retro_keybind *profile,
      const char *name)
{
   input_autoconf_binds[slot] = profile;
   input_config_set_device_name(slot, name);
   input_config_binds_changed();
}

/* User 1's hand-set Hotkey Enable and hotkeys are its xpad's buttons.
 * On user 2's pad of another numbering they are the RetroPad buttons
 * they are on the xpad: Back is user 2's Select, not its button 6. */
static void test_hotkeys_other_model(void)
{
   input_bits_t bits;
   input_driver_state_t *st = input_state_get_ptr();

   t_reset();
   t_pad(0, t_xpad, "Xpad");
   t_pad(1, t_auto, "Headset");
   t_bind(RARCH_ENABLE_HOTKEY, T_XPAD_SELECT);
   t_bind(RARCH_FAST_FORWARD_KEY, T_XPAD_X);

   /* the identity map: nobody else's controller read for the hotkeys */
   t_buttons[1] = T_SELECT | T_X;
   t_collect(&bits);
   CHECK(!BIT256_GET(bits, RARCH_FAST_FORWARD_KEY)
         && !(st->flags & INP_FLAG_BLOCK_LIBRETRO_INPUT),
         "the identity map: user 2's pad has user 1's hotkeys");
   CHECK(!t_button_reads[1],
         "the identity map: user 2's pad read for user 1's hotkeys");
   t_buttons[1] = 0;
   t_buttons[0] = T_BIT(T_XPAD_SELECT) | T_BIT(T_XPAD_X);
   t_collect(&bits);
   CHECK(BIT256_GET(bits, RARCH_FAST_FORWARD_KEY),
         "the identity map: user 1's Back and X do not fire it");
   t_buttons[0] = 0;

   t_map(1, 0);
   t_buttons[1] = T_BIT(T_XPAD_SELECT);
   t_collect(&bits);
   CHECK(!(st->flags & INP_FLAG_BLOCK_LIBRETRO_INPUT),
         "user 2's button 6, its D-Pad Left, holds game input back");
   t_buttons[1] = T_BIT(T_XPAD_SELECT) | T_BIT(T_XPAD_X);
   t_collect(&bits);
   CHECK(!BIT256_GET(bits, RARCH_FAST_FORWARD_KEY),
         "user 2's buttons 6 and 2 fire fast-forward");
   t_buttons[1] = T_SELECT;
   t_collect(&bits);
   CHECK(st->flags & INP_FLAG_BLOCK_LIBRETRO_INPUT,
         "user 2's Select is not its Hotkey Enable");
   t_buttons[1] = T_SELECT | T_X;
   t_collect(&bits);
   CHECK(BIT256_GET(bits, RARCH_FAST_FORWARD_KEY),
         "user 2's Select and X do not fire fast-forward");
   t_buttons[1] = T_X;
   t_collect(&bits);
   CHECK(!BIT256_GET(bits, RARCH_FAST_FORWARD_KEY),
         "user 2's X fires fast-forward without Hotkey Enable");
   t_buttons[1] = 0;
   t_buttons[0] = T_BIT(T_XPAD_SELECT) | T_BIT(T_XPAD_X);
   t_collect(&bits);
   CHECK(BIT256_GET(bits, RARCH_FAST_FORWARD_KEY),
         "user 2 sharing: user 1's Back and X do not fire it");
   t_buttons[0] = 0;

   /* a bind changed: Hotkey Enable on the xpad's Y */
   t_bind(RARCH_ENABLE_HOTKEY, T_XPAD_Y);
   t_buttons[1] = T_Y | T_X;
   t_collect(&bits);
   CHECK(BIT256_GET(bits, RARCH_FAST_FORWARD_KEY),
         "Hotkey Enable moved to Y: user 2's Y and X do not fire it");
   t_buttons[1] = T_SELECT | T_X;
   t_collect(&bits);
   CHECK(!BIT256_GET(bits, RARCH_FAST_FORWARD_KEY),
         "Hotkey Enable moved to Y: user 2's Select still enables");

   /* user 2's pad made an xpad: the binds as they are */
   t_pad(1, t_xpad, "Xpad");
   t_buttons[1] = T_BIT(T_XPAD_Y) | T_BIT(T_XPAD_X);
   t_collect(&bits);
   CHECK(BIT256_GET(bits, RARCH_FAST_FORWARD_KEY),
         "user 2 now an xpad: its Y and X do not fire it");
   t_buttons[1] = 0;
}

/* Game Focus, which lets a held Hotkey Enable through: user 2's is
 * its Select, not its button 6. */
static void test_hotkey_game_focus_other_model(void)
{
   input_bits_t bits;
   input_driver_state_t *st = input_state_get_ptr();

   t_reset();
   t_pad(0, t_xpad, "Xpad");
   t_pad(1, t_auto, "Headset");
   t_bind(RARCH_ENABLE_HOTKEY, T_XPAD_SELECT);
   t_bind(RARCH_FAST_FORWARD_KEY, T_XPAD_Y);
   t_map(1, 0);
   st->flags |= INP_FLAG_KB_MAPPING_BLOCKED;

   t_buttons[1] = T_BIT(T_XPAD_SELECT) | T_BIT(T_XPAD_Y) | T_Y;
   t_collect(&bits);
   CHECK(!BIT256_GET(bits, RARCH_FAST_FORWARD_KEY),
         "Game Focus: user 2's button 6 lets its Y fire fast-forward");
   t_buttons[1] = T_SELECT | T_Y;
   t_collect(&bits);
   CHECK(BIT256_GET(bits, RARCH_FAST_FORWARD_KEY),
         "Game Focus: user 2's Select and Y do not fire fast-forward");
   t_buttons[1] = 0;
   st->flags   &= ~INP_FLAG_KB_MAPPING_BLOCKED;
}

/* A hand-set axis: the xpad's LT is user 2's L2, not its axis 2. */
static void test_hotkey_axis_other_model(void)
{
   input_bits_t bits;
   t_reset();
   t_pad(0, t_xpad, "Xpad");
   t_pad(1, t_auto, "Headset");
   t_bind(RARCH_ENABLE_HOTKEY, T_XPAD_SELECT);
   input_config_binds[0][RARCH_FAST_FORWARD_KEY].joyaxis = AXIS_POS(2);
   input_config_binds[0][RARCH_FAST_FORWARD_KEY].attr    =
      RETRO_KEYBIND_ATTR(RETROK_UNKNOWN, NO_BTN, true);
   input_config_binds_changed();
   t_map(1, 0);

   t_buttons[1]   = T_SELECT | T_BIT(T_XPAD_SELECT);
   t_axis_held[1] = AXIS_POS(2);
   t_collect(&bits);
   CHECK(!BIT256_GET(bits, RARCH_FAST_FORWARD_KEY),
         "user 2's axis 2 fires the hotkey on the xpad's LT");
   t_buttons[1]   = T_SELECT;
   t_axis_held[1] = AXIS_POS(4);
   t_collect(&bits);
   CHECK(BIT256_GET(bits, RARCH_FAST_FORWARD_KEY),
         "user 2's L2 axis does not fire the hotkey on the xpad's LT");
   t_axis_held[1] = AXIS_NONE;
   t_buttons[1]   = T_SELECT | T_BIT(RETRO_DEVICE_ID_JOYPAD_L2);
   t_collect(&bits);
   CHECK(BIT256_GET(bits, RARCH_FAST_FORWARD_KEY),
         "user 2's L2 button does not fire the hotkey on the xpad's LT");
   t_buttons[1] = 0;

   /* user 2's profile has a fast-forward button of its own: kept */
   t_pad(1, t_auto_own, "Headset");
   t_buttons[1] = T_SELECT | T_BIT(T_OWN_FF);
   t_collect(&bits);
   CHECK(BIT256_GET(bits, RARCH_FAST_FORWARD_KEY),
         "a hand-set axis took user 2's own fast-forward button");
   t_buttons[1]   = T_SELECT;
   t_axis_held[1] = AXIS_POS(4);
   t_collect(&bits);
   CHECK(BIT256_GET(bits, RARCH_FAST_FORWARD_KEY),
         "with its own button, user 2's L2 axis does not fire the hotkey "
         "on the xpad's LT");
   t_axis_held[1] = AXIS_NONE;
   t_buttons[1]   = 0;
}

/* A hand-set menu toggle on Guide, which is no RetroPad button: user
 * 2's pad of another numbering has it where its own profile puts it. */
static void test_menu_toggle_other_model(void)
{
   t_reset();
   t_pad(0, t_xpad, "Xpad");
   t_pad(1, t_auto, "Headset");
   t_bind(RARCH_MENU_TOGGLE, T_GUIDE);
   t_map(1, 0);

   t_buttons[1] = T_BIT(T_GUIDE);
   CHECK(!t_menu_tap(), "user 2's button 8, its A, opens the menu");
   t_buttons[0] = T_BIT(T_GUIDE);
   CHECK(t_menu_tap(), "user 1's Guide does not open the menu");

   t_pad(1, t_auto_own, "Headset");
   t_buttons[1] = T_BIT(T_OWN_MENU);
   CHECK(t_menu_tap(),
         "user 2's own profile's menu toggle does not open the menu");
   t_buttons[1] = T_BIT(T_GUIDE);
   CHECK(!t_menu_tap(),
         "with a menu toggle of its own, user 2's button 8 opens the menu");
}

/* Two pads of one model: user 1's hand-set binds on user 2's as they
 * are. */
static void test_hotkeys_same_model(void)
{
   input_bits_t bits;
   input_driver_state_t *st = input_state_get_ptr();

   t_reset();
   t_pad(0, t_xpad, "Xpad");
   t_pad(1, t_xpad, "Xpad");
   t_bind(RARCH_ENABLE_HOTKEY, T_XPAD_SELECT);
   t_bind(RARCH_FAST_FORWARD_KEY, T_XPAD_X);
   t_map(1, 0);

   t_buttons[1] = T_BIT(T_XPAD_SELECT);
   t_collect(&bits);
   CHECK(st->flags & INP_FLAG_BLOCK_LIBRETRO_INPUT,
         "same model: user 2's Back is not its Hotkey Enable");
   t_buttons[1] = T_BIT(T_XPAD_SELECT) | T_BIT(T_XPAD_X);
   t_collect(&bits);
   CHECK(BIT256_GET(bits, RARCH_FAST_FORWARD_KEY),
         "same model: user 2's Back and X do not fire it");
   t_buttons[1] = 0;

   t_reset();
   t_pad(0, t_xpad, "Xpad");
   t_pad(1, t_xpad, "Xpad");
   t_bind(RARCH_MENU_TOGGLE, T_GUIDE);
   t_map(1, 0);
   t_buttons[1] = T_BIT(T_GUIDE);
   CHECK(t_menu_tap(), "same model: user 2's Guide does not open the menu");
}

/* a driver that hands the frontend its keys: none down */
static void t_keys_down(void *data, unsigned port, const uint16_t *keys,
      const uint8_t *bind, unsigned count, uint32_t *down)
{ (void)data; (void)port; (void)keys; (void)bind; (void)count; (void)down; }
static int16_t t_kd_state(void *data, const input_device_driver_t *joypad,
      rarch_joypad_info_t *joypad_info, const retro_keybind_set *binds,
      unsigned port, unsigned device, unsigned idx, unsigned id)
{
   (void)data; (void)joypad; (void)joypad_info; (void)binds; (void)port;
   (void)device; (void)idx; (void)id;
   return 0;
}
static input_driver_t t_kd;

/* With the keys the frontend's, only the hotkeys something may hold
 * are gone through: one that only user 2's profile binds is among
 * them while user 2 shares the port. */
static void test_hotkey_sharer_profile(void)
{
   input_bits_t bits;
   input_driver_state_t *st = input_state_get_ptr();

   t_reset();
   t_kd.input_state   = t_kd_state;
   t_kd.keys_down     = t_keys_down;
   st->current_driver = &t_kd;
   t_pad(0, t_xpad, "Xpad");
   t_pad(1, t_auto_own, "Headset");
   input_config_binds[0][RARCH_FAST_FORWARD_KEY].attr =
      RETRO_KEYBIND_ATTR(RETROK_UNKNOWN, NO_BTN, true);
   input_config_binds_changed();
   t_map(1, 0);

   t_buttons[1] = T_BIT(T_OWN_FF);
   t_collect(&bits);
   CHECK(BIT256_GET(bits, RARCH_FAST_FORWARD_KEY),
         "user 2's own profile's fast-forward is passed over");
   t_buttons[1] = 0;
   t_buttons[0] = T_BIT(T_OWN_FF);
   t_collect(&bits);
   CHECK(!BIT256_GET(bits, RARCH_FAST_FORWARD_KEY),
         "user 2's profile's fast-forward fires on user 1's pad");
   t_buttons[0]       = 0;
   st->current_driver = NULL;
}

/* Pads of two names that number the RetroPad alike: the binds as they
 * are, Guide included. Another numbering still translates. */
static void test_hotkeys_same_layout(void)
{
   input_bits_t bits;

   t_reset();
   t_pad(0, t_xpad, "Xpad A");
   t_pad(1, t_xpad, "Xpad B");
   t_bind(RARCH_ENABLE_HOTKEY, T_GUIDE);
   t_bind(RARCH_FAST_FORWARD_KEY, T_XPAD_X);
   t_map(1, 0);

   t_buttons[1] = T_BIT(T_GUIDE) | T_BIT(T_XPAD_X);
   t_collect(&bits);
   CHECK(BIT256_GET(bits, RARCH_FAST_FORWARD_KEY),
         "one layout: user 2's Guide and X do not fire fast-forward");
   t_buttons[1] = T_BIT(T_XPAD_X);
   t_collect(&bits);
   CHECK(!BIT256_GET(bits, RARCH_FAST_FORWARD_KEY),
         "one layout: user 2's X fires fast-forward without Hotkey Enable");

   /* A and B the other way round */
   t_reset();
   t_pad(0, t_xpad, "Xpad A");
   t_pad(1, t_xpad_ab, "Xpad B");
   t_bind(RARCH_ENABLE_HOTKEY, T_XPAD_SELECT);
   t_bind(RARCH_FAST_FORWARD_KEY, 0);
   t_map(1, 0);

   t_buttons[1] = T_BIT(T_XPAD_SELECT) | T_BIT(1);
   t_collect(&bits);
   CHECK(BIT256_GET(bits, RARCH_FAST_FORWARD_KEY),
         "A and B swapped: user 2's Back and A do not fire fast-forward");
   t_buttons[1] = T_BIT(T_XPAD_SELECT) | T_BIT(0);
   t_collect(&bits);
   CHECK(!BIT256_GET(bits, RARCH_FAST_FORWARD_KEY),
         "A and B swapped: user 2's Back and B fire fast-forward");
   t_buttons[1] = 0;
}

int main(void)
{
   test_identity();
   test_shared_port();
   test_swapped_ports();
   test_port_2_users();
   test_rumble_answer();
   test_vid_pid_parse();
   test_plays_as_match();
   test_plays_as_round_trip();
   test_plays_as_entry_life();
   test_place_auto();
   test_place_player();
   test_place_limits();
   test_place_reservation();
   test_place_reservation_order();
   test_place_leave_and_change();
   test_place_no_player();
   test_place_restart();
   test_place_saved_ports();
   test_plays_as_copy();
   test_menu_shared();
   test_hotkeys_shared();
   test_hotkey_enable_shared();
   test_menu_toggle_shared();
   test_hotkeys_follow();
   test_menu_toggle_game_button();
   test_menu_combo_split();
   test_hotkeys_shared_menu();
   test_hotkey_enable_keyboard();
   test_none_player1();
   test_hotkeys_other_model();
   test_hotkey_game_focus_other_model();
   test_hotkey_axis_other_model();
   test_menu_toggle_other_model();
   test_hotkeys_same_model();
   test_hotkeys_same_layout();
   test_hotkey_sharer_profile();
   if (failures)
   {
      printf("%d failure(s)\n", failures);
      return 1;
   }
   printf("PASS mapped_port_test\n");
   return 0;
}
