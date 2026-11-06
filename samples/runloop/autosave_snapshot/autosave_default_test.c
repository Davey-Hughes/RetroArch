/* DEFAULT_AUTOSAVE_INTERVAL: 10 s wherever threads exist, 0 without
 * them. Built with and without HAVE_THREADS. */
#include <stdio.h>

#include "../../../config.def.h"

#ifdef HAVE_THREADS
#define EXPECTED 10
#else
#define EXPECTED 0
#endif

int main(void)
{
   if (DEFAULT_AUTOSAVE_INTERVAL != EXPECTED)
   {
      printf("autosave default: %d, expected %d\n",
            DEFAULT_AUTOSAVE_INTERVAL, EXPECTED);
      return 1;
   }
   printf("autosave default %d: passed\n", DEFAULT_AUTOSAVE_INTERVAL);
   return 0;
}
