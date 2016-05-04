#include "luaT.h"
#include "lualib.h"
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <spawn.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <signal.h>
#include "error.h"

typedef struct spawn_t {
   pid_t pid;
   int fd[2][2];
   posix_spawn_file_actions_t file_actions;
   posix_spawnattr_t spawnattr;
} spawn_t;

void spawn_destroy(spawn_t *spawn) {
   for (int i = 0; i < 2; i++) {
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
   if (lua_type(L, -1) != LUA_TSTRING) {
      return LUA_HANDLE_ERROR_STR(L, "file: expected a string");
   }
   const char *file = lua_tostring(L, -1);
   lua_pop(L, 1);

   lua_pushstring(L, "args");
   lua_gettable(L, 1);
   if (lua_type(L, -1) != LUA_TNIL && lua_type(L, -1) != LUA_TTABLE) {
      return LUA_HANDLE_ERROR_STR(L, "args: expected a table, or nil");
   }
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

   for (int i = 0; i < 2; i++) {
      ret = pipe(spawn->fd[i]);
      if (ret) {
         spawn_destroy(spawn);
         return LUA_HANDLE_ERROR(L, errno);
      }
      int rw = i == 0 ? 0 : 1;
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
      ret = posix_spawn_file_actions_addclose(&spawn->file_actions, spawn->fd[i][!rw]);
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

   ret = close(spawn->fd[0][0]);
   if (ret) {
      spawn_destroy(spawn);
      return LUA_HANDLE_ERROR(L, errno);
   }
   spawn->fd[0][0] = 0;
   ret = close(spawn->fd[1][1]);
   if (ret) {
      spawn_destroy(spawn);
      return LUA_HANDLE_ERROR(L, errno);
   }
   spawn->fd[1][1] = 0;

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
   // optional signal to send to child process
   const char *signame = lua_tostring(L, 2);
   if (signame) {
      int which;
      if (strcmp(signame, "KILL") == 0) {
         which = SIGKILL;
      } else if (strcmp(signame, "TERM") == 0) {
         which = SIGTERM;
      } else {
         return LUA_HANDLE_ERROR_STR(L, "unknown signal");
      }
      int ret = kill(spawn->pid, which);
      if (ret) return LUA_HANDLE_ERROR(L, errno);
   }
   // close stdin
   if (spawn->fd[0][1]) {
      int ret = close(spawn->fd[0][1]);
      if (ret) return LUA_HANDLE_ERROR(L, errno);
      spawn->fd[0][1] = 0;
   }
   if (signame) {
      // just close stdout, we dont care at this point
      close((*uspawn)->fd[1][0]);
      (*uspawn)->fd[1][0] = 0;
   } else {
      // read whatever is left on stdout, we dont want the process to be stalled on us
      char buff[1024];
      while (1) {
         ssize_t x = read((*uspawn)->fd[1][0], buff, 1024);
         if (x < 0) {
            return LUA_HANDLE_ERROR(L, errno);
         } else if (x == 0) {
            break;
         }
      }
   }
   // wait for exit
   int status;
   do {
      int ret = waitpid(spawn->pid, &status, WUNTRACED | WCONTINUED);
      if (ret < 0) return LUA_HANDLE_ERROR(L, errno);
   } while (!WIFEXITED(status) && !WIFSIGNALED(status));
   // clean up
   spawn_destroy(spawn);
   *uspawn = NULL;
   // return the exit code
   lua_pushnumber(L, WEXITSTATUS(status));
   return 1;
}

int spawn_stdin(lua_State *L) {
   spawn_t **uspawn = lua_touserdata(L, 1);
   if (!*uspawn) return LUA_HANDLE_ERROR_STR(L, "spawn was already closed");
   if (lua_gettop(L) == 1) {
      // close stdin
      int ret = close((*uspawn)->fd[0][1]);
      if (ret) return LUA_HANDLE_ERROR(L, errno);
      (*uspawn)->fd[0][1] = 0;
      return 0;
   }
   // write to stdin
   size_t str_len;
   const char *str = lua_tolstring(L, 2, &str_len);
   size_t cb = 0;
   while (cb < str_len) {
      ssize_t x = write((*uspawn)->fd[0][1], str + cb, str_len - cb);
      if (x < 0) {
         return LUA_HANDLE_ERROR(L, errno);
      } else {
         cb += x;
      }
   }
   return 0;
}

int spawn_stdout(lua_State *L) {
   spawn_t **uspawn = lua_touserdata(L, 1);
   if (!*uspawn) return LUA_HANDLE_ERROR_STR(L, "spawn was already closed");
   int type = lua_type(L, 2);
   if (type == LUA_TNUMBER) {
      // read some number of bytes from stdout
      size_t cb = lua_tonumber(L, 2);
      char *buff = calloc(cb + 1, 1);
      ssize_t n = read((*uspawn)->fd[1][0], buff, cb);
      if (n < 0) {
         free(buff);
         return LUA_HANDLE_ERROR(L, errno);
      }
      if (n > 0) {
         lua_pushlstring(L, buff, n);
         free(buff);
         return 1;
      } else {
         free(buff);
         return 0;
      }
   }
   const char *arg = luaL_optstring(L, 2, "*l");
   if (strncmp(arg, "*l", 2) == 0) {
      // read stdout until EOL
      size_t cb = 0;
      size_t max_cb = 1024;
      char *buff = realloc(NULL, max_cb);
      while (1) {
         ssize_t x = read((*uspawn)->fd[1][0], buff + cb, 1);
         if (x < 0) {
            free(buff);
            return LUA_HANDLE_ERROR(L, errno);
         } else if (x == 0) {
            if (cb > 0) {
               buff[cb] = 0;
               lua_pushlstring(L, buff, cb);
               free(buff);
               return 1;
            } else {
               free(buff);
               return 0;
            }
         } else {
            if (buff[cb] == '\n') {
               buff[cb] = 0;
               lua_pushlstring(L, buff, cb);
               free(buff);
               return 1;
            }
            cb++;
            if (cb + 1 == max_cb) {
               max_cb += 1024;
               buff = realloc(buff, max_cb);
            }
         }
      }
   } else {
      // read stdout until EOF
      size_t cb = 0;
      size_t max_cb = 1024;
      char *buff = realloc(NULL, max_cb);
      while (1) {
         ssize_t x = read((*uspawn)->fd[1][0], buff + cb, max_cb - cb - 1);
         if (x < 0) {
            free(buff);
            return LUA_HANDLE_ERROR(L, errno);
         } else if (x == 0) {
            if (cb > 0) {
               buff[cb] = 0;
               lua_pushlstring(L, buff, cb);
               free(buff);
               return 1;
            } else {
               free(buff);
               return 0;
            }
         } else {
            cb += x;
            if (cb + 1 == max_cb) {
               max_cb += 1024;
               buff = realloc(buff, max_cb);
            }
         }
      }
   }
}

int spawn_stdout_file_id(lua_State *L) {
   spawn_t **uspawn = lua_touserdata(L, 1);
   if (!*uspawn) return LUA_HANDLE_ERROR_STR(L, "spawn was already closed");
   lua_pushinteger(L, (*uspawn)->fd[1][0]);
   return 1;
}

int spawn_pid(lua_State *L) {
   spawn_t **uspawn = lua_touserdata(L, 1);
   if (!*uspawn) return LUA_HANDLE_ERROR_STR(L, "spawn was already closed");
   lua_pushnumber(L, (*uspawn)->pid);
   return 1;
}

int spawn_running(lua_State *L) {
   spawn_t **uspawn = lua_touserdata(L, 1);
   if (!*uspawn) return LUA_HANDLE_ERROR_STR(L, "spawn was already closed");
   siginfo_t si;
   memset(&si, 0, sizeof(si));
   int ret = waitid(P_PID, (*uspawn)->pid, &si, WEXITED | WNOHANG | WNOWAIT);
   if (ret) return LUA_HANDLE_ERROR(L, errno);
   lua_pushboolean(L, si.si_pid == 0);
   return 1;
}

int spawn_gc(lua_State *L) {
   spawn_t **uspawn = lua_touserdata(L, 1);
   if (*uspawn) {
      fprintf(stderr, "ipc.spawn being garbage collected before wait was called, sending SIGTERM to child process");
      int ret = kill((*uspawn)->pid, SIGTERM);
      if (ret) return LUA_HANDLE_ERROR(L, errno);
      spawn_wait(L);
      lua_pop(L, 1);
   }
   return 0;
}
