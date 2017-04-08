#ifndef _CONF_H_
#define _CONF_H_

#include <stdint.h>
#include <stddef.h>

enum sackmode {
  SACKMODE_ENABLE,
  SACKMODE_DISABLE,
  SACKMODE_HASHIP,
  SACKMODE_HASHIPPORT,
};

struct ratehashconf {
  size_t size;
  uint32_t timer_period_usec;
  uint32_t timer_add;
  uint32_t initial_tokens;
};

struct conf {
  enum sackmode sackmode;
  size_t sackhashsize;
  size_t conntablesize;
  size_t timerheapsize;
  struct ratehashconf ratehash;
};

#define CONF_INITIALIZER { \
  .sackmode = SACKMODE_HASHIP, \
  .sackhashsize = 131072, \
  .conntablesize = 131072, \
  .timerheapsize = 131072, \
  .ratehash = { \
    .size = 131072, \
    .timer_period_usec = (1000*1000), \
    .timer_add = 400, \
    .initial_tokens = 2000, \
  }, \
}

#endif
