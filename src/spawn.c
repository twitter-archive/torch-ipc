#include "luaT.h"
#include "lualib.h"
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <spawn.h>
#include <string.h>
#include <sys/wait.h>
#include "error.h"

typedef struct spawn_t {
   pid_t pid;
   int fd[3][2];
   posix_spawn_file_actions_t file_actions;
   posix_spawnattr_t spawnattr;
   int exit_status;
} spawn_t;

void spawn_destroy(spawn_t *spawn) {
   for (int i = 0; i < 3; i++) {
      for (int j = 0; j < 2; j++) {
         if (spawn->fd[i][j]) {
            close(spawn->fd[i][j]);
            spawn->fd[i][j] = 0;
         }
      }
   }
   posix_spawn_file_actions_destroy(&spawn->file_actions);
   posix_spawnattr_destroy(&spawn->spawnattr);
   free(spawn);
}

int spawn_open(lua_State *L) {
   if (lua_gettop(L) != 1 || lua_type(L, 1) != LUA_TTABLE) return LUA_HANDLE_ERROR_STR(L, "expected a single table argument");

   lua_pushstring(L, "file");
   lua_gettable(L, 1);
   const char *file = lua_tostring(L, -1);
   lua_pop(L, 1);

   lua_pushstring(L, "args");
   lua_gettable(L, 1);
   size_t n = lua_objlen(L, -1);
   char **argv = alloca(sizeof(char *) * (n + 2));
   argv[0] = (char *)file;
   for (size_t i = 1; i <= n; i++) {
      lua_rawgeti(L, -1, i);
      argv[i] = (char *)lua_tostring(L, -1);
      lua_pop(L, 1);
   }
   argv[n + 1] = NULL;
   lua_pop(L, 1);

   lua_pushstring(L, "env");
   lua_gettable(L, 1);
   n = lua_objlen(L, -1);
   extern char **environ;
   char **envp = environ;
   if (n > 0) {
      envp = alloca(sizeof(char *) * (n + 1));
      for (size_t i = 1; i <= n; i++) {
         lua_rawgeti(L, -1, i);
         envp[i - 1] = (char *)lua_tostring(L, -1);
         lua_pop(L, 1);
      }
      envp[n] = NULL;
   }
   lua_pop(L, 1);

   spawn_t *spawn = calloc(sizeof(spawn_t), 1);

   int ret = posix_spawn_file_actions_init(&spawn->file_actions);
   if (ret) {
      spawn_destroy(spawn);
      return LUA_HANDLE_ERROR(L, errno);
   }
   for (int i = 0; i < 3; i++) {
      ret = pipe(spawn->fd[i]);
      if (ret) {
         spawn_destroy(spawn);
         return LUA_HANDLE_ERROR(L, errno);
      }
      int rw = i == 0 ? 0 : 1;
      ret = posix_spawn_file_actions_addclose(&spawn->file_actions, spawn->fd[i][!rw]);
      if (ret) {
         spawn_destroy(spawn);
         return LUA_HANDLE_ERROR(L, errno);
      }
      ret = posix_spawn_file_actions_adddup2(&spawn->file_actions, spawn->fd[i][rw], i);
      if (ret) {
         spawn_destroy(spawn);
         return LUA_HANDLE_ERROR(L, errno);
      }
      ret = posix_spawn_file_actions_addclose(&spawn->file_actions, spawn->fd[i][rw]);
      if (ret) {
         spawn_destroy(spawn);
         return LUA_HANDLE_ERROR(L, errno);
      }
   }

   ret = posix_spawnattr_init(&spawn->spawnattr);
   if (ret) {
      spawn_destroy(spawn);
      return LUA_HANDLE_ERROR(L, errno);
   }

   ret = posix_spawnp(&spawn->pid, file, &spawn->file_actions, &spawn->spawnattr, argv, envp);
   if (ret) {
      spawn_destroy(spawn);
      return LUA_HANDLE_ERROR(L, errno);
   }

   close(spawn->fd[0][0]);
   spawn->fd[0][0] = 0;
   close(spawn->fd[1][1]);
   spawn->fd[1][1] = 0;
   close(spawn->fd[2][1]);
   spawn->fd[2][1] = 0;

   spawn_t **uspawn = lua_newuserdata(L, sizeof(spawn_t *));
   *uspawn = spawn;
   luaL_getmetatable(L, "ipc.spawn");
   lua_setmetatable(L, -2);
   return 1;
}

int spawn_wait(lua_State *L) {
   spawn_t **uspawn = lua_touserdata(L, 1);
   if (!*uspawn) return LUA_HANDLE_ERROR_STR(L, "spawn was already closed");
   spawn_t *spawn = *uspawn;
   if (spawn->pid) {
      close(spawn->fd[0][1]);
      spawn->fd[0][1] = 0;
      int status;
      if (-1 == waitpid(spawn->pid, &status, 0)) return LUA_HANDLE_ERROR(L, errno);
      spawn->pid = 0;
      spawn->exit_status = WEXITSTATUS(status);
   }
   lua_pushnumber(L, spawn->exit_status);
   return 1;
}

static int close_file(lua_State *L) {
   FILE **file = lua_touserdata(L, 1);
   if (*file) {
      fclose(*file);
      *file = NULL;
   }
   return 0;
}

static int new_file(lua_State *L, int fd, const char *mode) {
   FILE **file = lua_newuserdata(L, sizeof(FILE**));
   *file = NULL;
   luaL_getmetatable(L, LUA_FILEHANDLE);
   lua_setmetatable(L, -2);

   lua_newtable(L);
   lua_pushstring(L, "__close");
   lua_pushcfunction(L, close_file);
   lua_settable(L, -3);
   lua_setfenv(L, -2);

   *file = fdopen(fd, mode);
   if (!*file) return LUA_HANDLE_ERROR(L, errno);
   return 1;
}

int spawn_stdin(lua_State *L) {
   spawn_t **uspawn = lua_touserdata(L, 1);
   if (!*uspawn) return LUA_HANDLE_ERROR_STR(L, "spawn was already closed");
   return new_file(L, (*uspawn)->fd[0][1], "a");
}

int spawn_stdout(lua_State *L) {
   spawn_t **uspawn = lua_touserdata(L, 1);
   if (!*uspawn) return LUA_HANDLE_ERROR_STR(L, "spawn was already closed");
   return new_file(L, (*uspawn)->fd[1][0], "r");
}

int spawn_stderr(lua_State *L) {
   spawn_t **uspawn = lua_touserdata(L, 1);
   if (!*uspawn) return LUA_HANDLE_ERROR_STR(L, "spawn was already closed");
   return new_file(L, (*uspawn)->fd[2][0], "r");
}

int spawn_close(lua_State *L) {
   spawn_wait(L);
   spawn_t **uspawn = lua_touserdata(L, 1);
   spawn_destroy(*uspawn);
   *uspawn = NULL;
   return 1;
}

int spawn_gc(lua_State *L) {
   spawn_t **uspawn = lua_touserdata(L, 1);
   if (*uspawn) {
      spawn_wait(L);
      lua_pop(L, 1);
      spawn_destroy(*uspawn);
   }
   return 0;
}
