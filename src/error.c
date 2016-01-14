#include "luaT.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include <time.h>

int _parallel_log_error(int ret, const char* file, int line) {
   int pos_ret;

   if (ret < 0) {
      pos_ret = -ret;
   } else {
      pos_ret = ret;
   }
   fprintf(stderr, "ERROR: (%s, %d): (%d, %s)\n", file, line, pos_ret, strerror(pos_ret));
   return pos_ret;
}

int _parallel_lua_error(lua_State *L, int ret, const char* file, int line) {
   int pos_ret;

   if (ret < 0) {
      pos_ret = -ret;
   } else {
      pos_ret = ret;
   }
   return luaL_error(L, "ERROR: (%s, %d): (%d, %s)\n", file, line, pos_ret, strerror(pos_ret));
}

int _parallel_lua_error_str(lua_State *L, const char *str, const char* file, int line) {
   return luaL_error(L, "ERROR: (%s, %d): (%s)\n", file, line, str);
}

double _parallel_seconds() {
   struct timeval tv;

   gettimeofday(&tv, NULL);
   return tv.tv_sec + tv.tv_usec / 1e6;
}
