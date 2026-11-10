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

#ifndef _INPUT_PAD_TAG_H
#define _INPUT_PAD_TAG_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <retro_inline.h>

/* Connected controllers of one name are told apart by the word a hash
 * of each one's own identity picks here. A controller keeps its word
 * across runs and versions only while this list stays as it is: never
 * reorder, replace or remove a word. No two look or sound alike;
 * samples/input/pad_tag checks it. Names, so not translated. */
static const char *const input_pad_tag_words[256] = {
   "Acorn", "Alpaca", "Anchor", "Ant", "Attic", "Autumn", "Baboon", "Badger",
   "Bag", "Bakery", "Banana", "Barn", "Basket", "Bath", "Beach", "Bear",
   "Bee", "Bell", "Beluga", "Bib", "Bison", "Book", "Bottle", "Bread",
   "Bridge", "Broom", "Brush", "Bubble", "Budgie", "Bunny", "Bus", "Cabin",
   "Cactus", "Cake", "Camel", "Canary", "Canoe", "Canvas", "Castle", "Cat",
   "Celery", "Chair", "Cheese", "Chimp", "Cinema", "Circus", "Clock", "Collar",
   "Comb", "Comic", "Condor", "Cookie", "Cow", "Coyote", "Crab", "Crown",
   "Cup", "Daisy", "Deer", "Desert", "Dog", "Doll", "Donut", "Duck",
   "Eagle", "Earth", "Eel", "Egg", "Emu", "Engine", "Eraser", "Falcon",
   "Fence", "Ferry", "Fish", "Flower", "Flute", "Forest", "Fox", "Frog",
   "Garage", "Garden", "Garlic", "Gator", "Gecko", "Gerbil", "Gibbon", "Gift",
   "Glass", "Globe", "Glue", "Goat", "Goose", "Gopher", "Grape", "Hammer",
   "Harp", "Hawk", "Helmet", "Hen", "Hippo", "Hornet", "Horse", "Hotel",
   "Hound", "Hut", "Hyena", "Igloo", "Iguana", "Island", "Ivy", "Jackal",
   "Jam", "Jelly", "Jet", "Juice", "Jungle", "Kayak", "Kazoo", "Kite",
   "Kitten", "Koala", "Koi", "Lamp", "Leaf", "Lemur", "Lily", "Lion",
   "Lizard", "Llama", "Macaw", "Magpie", "Mango", "Mantis", "Marble", "Meadow",
   "Melon", "Milk", "Minnow", "Mirror", "Monkey", "Moon", "Mop", "Moth",
   "Mud", "Museum", "Napkin", "Newt", "Nugget", "Ocean", "Orca", "Otter",
   "Owl", "Paint", "Panda", "Paper", "Parcel", "Parrot", "Pastry", "Pencil",
   "Piano", "Pickle", "Picnic", "Pig", "Pigeon", "Pillow", "Pizza", "Planet",
   "Poncho", "Pony", "Poodle", "Possum", "Potato", "Puffin", "Puppy", "Puzzle",
   "Quail", "Rabbit", "Radio", "Raft", "Raisin", "Raven", "Rhino", "Rice",
   "Robot", "Rocket", "Roof", "Rope", "Rug", "Salad", "Saucer", "School",
   "Seal", "Seesaw", "Shark", "Sheep", "Shoe", "Shovel", "Shrimp", "Sink",
   "Ski", "Sled", "Sloth", "Snake", "Sock", "Sofa", "Soup", "Spider",
   "Sponge", "Spoon", "Squash", "Squid", "Stew", "Stove", "Sugar", "Sun",
   "Sushi", "Swan", "Syrup", "Table", "Taxi", "Teapot", "Tent", "Tiara",
   "Tiger", "Toad", "Toffee", "Toucan", "Train", "Tree", "Trout", "Truck",
   "Tulip", "Tuna", "Tunnel", "Twig", "Valley", "Van", "Violin", "Waffle",
   "Wagon", "Walrus", "Wasp", "Watch", "Weasel", "Whale", "Winter", "Wolf",
   "Wombat", "Worm", "Wren", "Yacht", "Yak", "Yogurt", "Zebra", "Zoo",
};

/* FNV-1a, 32 bits, of @len bytes at @p, from @h (0x811c9dc5 to start) */
static INLINE uint32_t input_pad_tag_fnv1a(uint32_t h,
      const unsigned char *p, size_t len)
{
   size_t i;
   for (i = 0; i < len; i++)
   {
      h ^= p[i];
      h *= 0x01000193UL;
   }
   return h;
}

/* A controller's identity: its vendor and product ids, then its own
 * serial or Bluetooth address (@uniq) when it has one, else where it is
 * plugged in (@phys). 0 for none: @uniq NULL (the driver cannot tell
 * controllers of one model apart) or neither given. Never 0 else. */
static INLINE uint32_t input_pad_tag_ident(unsigned vid, unsigned pid,
      const char *uniq, const char *phys)
{
   uint32_t h;
   unsigned char ids[4];
   const char *id = (uniq && *uniq) ? uniq : phys;
   if (!uniq || !id || !*id)
      return 0;
   ids[0] = (unsigned char)(vid        & 0xff);
   ids[1] = (unsigned char)((vid >> 8) & 0xff);
   ids[2] = (unsigned char)(pid        & 0xff);
   ids[3] = (unsigned char)((pid >> 8) & 0xff);
   h      = input_pad_tag_fnv1a(0x811c9dc5UL, ids, sizeof(ids));
   h      = input_pad_tag_fnv1a(h, (const unsigned char*)id, strlen(id));
   return h ? h : 1;
}

/* The word a non-zero identity picks: its four bytes folded into one */
static INLINE const char *input_pad_tag_word(uint32_t ident)
{
   return input_pad_tag_words[
      (ident ^ (ident >> 8) ^ (ident >> 16) ^ (ident >> 24)) & 0xff];
}

#endif
