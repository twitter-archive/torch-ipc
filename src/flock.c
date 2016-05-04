#include "flock.h"
#include <errno.h>
#include <sys/file.h>
#include <unistd.h>
#include "error.h"

#ifndef O_CLOEXEC
#define O_CLOEXEC (0)
#endif

int flock_open(lua_State *L) {
   const char *file_name = lua_tostring(L, 1);
   int no_block = luaT_optboolean(L, 2, 0);
   int flags = O_CLOEXEC | O_RDWR;
   if (!no_block) {
      flags |= O_CREAT;
   }
   int fd = open(file_name, flags, S_IRUSR | S_IWUSR);
   if (fd < 0) {
      if (errno == ENOENT || errno == EACCES) return 0;
      return LUA_HANDLE_ERROR(L, errno);
   }
   flags = LOCK_EX;
   if (no_block) {
      flags |= LOCK_NB;
   }
   int ret = flock(fd, flags);
   if (ret < 0) {
      close(fd);
      if ((flags & LOCK_NB) && (errno == EWOULDBLOCK)) return 0;
      return LUA_HANDLE_ERROR(L, errno);
   }
   int *handle = lua_newuserdata(L, sizeof(int));
   *handle = fd;
   luaL_getmetatable(L, "ipc.flock");
   lua_setmetatable(L, -2);
   return 1;
}

int flock_close(lua_State *L) {
   int *handle = lua_touserdata(L, 1);
   int fd = *handle;
   if (fd) {
      int ret = flock(fd, LOCK_UN);
      close(fd);
      *handle = 0;
      if (ret < 0) return LUA_HANDLE_ERROR(L, errno);
   }
   return 0;
}
