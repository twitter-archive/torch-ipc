#include "serialize.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#define EXTRA_LUA_TINTEGER 127

#define RB_WRITE(L, rb, in, cb) \
   { if (ringbuffer_write((rb), (in), (cb)) != (cb)) return -ENOMEM; }

#define RB_WRITE_POP(L, rb, in, cb, n) \
   { if (ringbuffer_write((rb), (in), (cb)) != (cb)) { lua_pop((L), (n)); return -ENOMEM; }}

#define RB_READ(L, rb, out, cb) \
   { if (ringbuffer_read((rb), (out), (cb)) != (cb)) return -ENOMEM; }

#define RB_READ_POP(L, rb, out, cb, n) \
   { if (ringbuffer_read((rb), (out), (cb)) != (cb)) { lua_pop((L), (n)); return -ENOMEM; }}

#define MIN(a, b) (a) < (b) ? (a) : (b)
#define CHUNK_SIZE (8192)

typedef struct chunked_t {
   ringbuffer_t *rb;
   uint8_t buf[CHUNK_SIZE];
   uint8_t read_last;
} chunked_t;

static int rb_lua_writer(lua_State *L, const void* in, size_t cb, void* param) {
   (void)L;
   ringbuffer_t *rb = (ringbuffer_t *)param;
   while (cb > 0) {
      size_t ccb = MIN(CHUNK_SIZE, cb);
      RB_WRITE(L, rb, &ccb, sizeof(size_t));
      RB_WRITE(L, rb, in, ccb);
      in = (((const uint8_t *)in) + ccb);
      cb -= ccb;
   }
   return 0;
}

static const char *rb_lua_reader(lua_State *L, void *param, size_t *size) {
   (void)L;
   chunked_t *chunked = (chunked_t *)param;
   if (ringbuffer_read(chunked->rb, size, sizeof(size_t)) != sizeof(size_t)) {
      *size = 0;
      return NULL;
   }
   if (*size) {
      if (ringbuffer_read(chunked->rb, chunked->buf, *size) != *size) {
         *size = 0;
         return NULL;
      }
      return (const char *)chunked->buf;
   }
   chunked->read_last = 1;
   return NULL;
}

inline static void rb_stackCheck(lua_State *L, int startsize, const char* loc) {
   if (lua_gettop(L) != startsize) {
      luaL_error(L, "Stack wasn't correctly reset in %s\n", loc);
   }
}

int rb_save(lua_State *L, int index, ringbuffer_t *rb, int oop, int upval) {
   char type = lua_type(L, index);
   switch (type) {
      case LUA_TNIL: {
         RB_WRITE(L, rb, &type, sizeof(char));
         return 0;
      }
      case LUA_TBOOLEAN: {
         RB_WRITE(L, rb, &type, sizeof(char));
         type = lua_toboolean(L, index);
         RB_WRITE(L, rb, &type, sizeof(char));
         return 0;
      }
      case LUA_TNUMBER: {
#if LUA_VERSION_NUM >= 503
         if (lua_isinteger(L, index)) {
            type = EXTRA_LUA_TINTEGER;
            RB_WRITE(L, rb, &type, sizeof(char));
            lua_Integer n = lua_tointeger(L, index);
            RB_WRITE(L, rb, &n, sizeof(n));
         } else
#endif
         {
            RB_WRITE(L, rb, &type, sizeof(char));
            lua_Number n = lua_tonumber(L, index);
            RB_WRITE(L, rb, &n, sizeof(n));
         }
         return 0;
      }
      case LUA_TSTRING: {
         RB_WRITE(L, rb, &type, sizeof(char));
         size_t str_len;
         const char *str = lua_tolstring(L, index, &str_len);
         RB_WRITE(L, rb, &str_len, sizeof(str_len));
         RB_WRITE(L, rb, str, str_len);
         return 0;
      }
      case LUA_TTABLE: {
         int startsize = lua_gettop(L);
         RB_WRITE(L, rb, &type, sizeof(char));
         int top = lua_gettop(L);
         int ret;

         lua_pushnil(L);
         while (lua_next(L, index) != 0) {
            ret = rb_save(L, top + 1, rb, oop, upval); // key
            if (ret) {
               lua_pop(L, 2);
               rb_stackCheck(L, startsize, "write table key");
               return ret;
            }
            ret = rb_save(L, top + 2, rb, oop, upval); // value
            if (ret) {
               lua_pop(L, 2);
               rb_stackCheck(L, startsize, "write table value");
               return ret;
            }
            lua_pop(L, 1);
         }
         type = LUA_TNIL;
         RB_WRITE(L, rb, &type, sizeof(char)); // nil breaks the read loop

         // the typename identifies the metatable
         const char *str = luaT_typename(L, index);
         if (!str) {
            if (luaL_callmeta(L, index, "metatablename")) {
               str = lua_tostring(L, lua_gettop(L));
               lua_pop(L, 1);
            } else {
               str = "";
            }
         }
         size_t str_len = strlen(str);
         RB_WRITE(L, rb, &str_len, sizeof(str_len));
         RB_WRITE(L, rb, str, str_len);
         rb_stackCheck(L, startsize, "write table return");
         return 0;
      }
      case LUA_TFUNCTION: {
         int startsize = lua_gettop(L);
         RB_WRITE(L, rb, &type, sizeof(char));
         int pushed = 0;
         if (index != startsize) {
            lua_pushvalue(L, index);
            pushed = 1;
         }
         lua_Debug ar;
         lua_pushvalue(L, -1);
         lua_getinfo(L, ">nuS", &ar);
         if (ar.what[0] != 'L') {
             luaL_error(L, "attempt to persist a C function '%s'", ar.name);
         }
         rb_stackCheck(L, startsize+pushed, "get debug");

         // this returns different things under LuaJIT vs Lua
#if LUA_VERSION_NUM >= 503
         int ret = lua_dump(L, rb_lua_writer, rb, 0);
#else
         int ret = lua_dump(L, rb_lua_writer, rb);
#endif
         if (ret){
            lua_pop(L, pushed);
            rb_stackCheck(L, startsize, "lua_dump write");
            return ret;
         }

         size_t str_len = 0;
         RB_WRITE_POP(L, rb, &str_len, sizeof(size_t), pushed); // zero-terminated

         const char *name; // will hold the name of an upvalue

         int env = 0; // true if function has only one upvalue and it is _ENV
         if ((upval == 0) && (ar.nups == 1)) {
            name = lua_getupvalue(L, -1, 1);
            if (strcmp(name, "_ENV") == 0) {
               env = 1;
            }
            lua_pop(L, 1);
            // since it is only _ENV (which is used for global variables), go through the motions of an (upval=1)
            rb_stackCheck(L, startsize+pushed, "write _ENV");
            RB_WRITE_POP(L, rb, &env, sizeof(int), pushed);
         } else {
            // does the serialization accept upvalues?
            RB_WRITE_POP(L, rb, &upval, sizeof(int), pushed);
         }

         // upvalues
         if ((upval == 1) || (env == 1)) {
            lua_newtable(L);
            int envIdx = -1;
            for (int i=1; i <= ar.nups; i++) {
               name = lua_getupvalue(L, -2, i);
               if (strcmp(name, "_ENV") != 0) {
                  lua_rawseti(L, -2, i);
               } else { // ignore _ENV as we assume that this is the global _G variable
                  lua_pop(L, 1);
                  envIdx = i;
               }
            }
            // write upvalue index of _ENV
            RB_WRITE_POP(L, rb, &envIdx, sizeof(int), 1+pushed);

            // write upvalue table
            int ret = rb_save(L, lua_gettop(L), rb, oop, upval);
            if (ret) {
               lua_pop(L, 1);
               rb_stackCheck(L, startsize+pushed, "write function upvalue");
               return ret;
            }
            lua_pop(L, 1);
         } else if (ar.nups > 0) {
            name = lua_getupvalue(L, -1, 1);
            lua_pop(L, 1);
            luaL_error(L, "Attempt to serialize closure '%s:%d' with %d upvalues; first is: '%s'.\n"
                          "Consider using ipc.workqueue.writeup() instead.", ar.short_src, ar.linedefined, ar.nups, name);
         }

         if (index != lua_gettop(L)) {
            lua_pop(L, 1);
         }
         rb_stackCheck(L, startsize, "write function return");
         return 0;
      }
      case LUA_TUSERDATA: {
         if (oop) return -EPERM;
         const char *str = luaT_typename(L, index);
         if (!str) {
            if (luaL_callmeta(L, index, "metatablename")) {
               str = lua_tostring(L, lua_gettop(L));
               lua_pop(L, 1);
               type = -type;
            } else {
               return -EINVAL;
            }
         }
         RB_WRITE(L, rb, &type, sizeof(char));
         size_t str_len = strlen(str);
         RB_WRITE(L, rb, &str_len, sizeof(str_len));
         RB_WRITE(L, rb, str, str_len);
         void *ptr = lua_touserdata(L, index);
         RB_WRITE(L, rb, ptr, sizeof(void *));
         if (luaL_callmeta(L, index, "retain")) {
            lua_pop(L, 1);
         } else {
            return -EINVAL;
         }
         return 0;
      }
      default:
         return -EPERM;
   }
}

static int rb_load_rcsv(lua_State *L, ringbuffer_t *rb, int is_key) {
   char type;
   lua_Number n;
   lua_Integer ni;
   char *str;
   size_t str_len;
   int ret;
   chunked_t chunked;
   void *ptr, **pptr;
   int startsize;

   if (!lua_checkstack(L, 1)) return -ENOMEM;
   RB_READ(L, rb, &type, sizeof(type));
   switch (type) {
      case LUA_TNIL:
         if (is_key) return 0;
         lua_pushnil(L);
         return 1;
      case LUA_TBOOLEAN:
         RB_READ(L, rb, &type, sizeof(char));
         lua_pushboolean(L, type);
         return 1;
      case LUA_TNUMBER:
         RB_READ(L, rb, &n, sizeof(n));
         lua_pushnumber(L, n);
         return 1;
      case EXTRA_LUA_TINTEGER:
         RB_READ(L, rb, &ni, sizeof(ni));
         lua_pushinteger(L, ni);
         return 1;
      case LUA_TSTRING:
         RB_READ(L, rb, &str_len, sizeof(str_len));
         str = alloca(str_len);
         RB_READ(L, rb, str, str_len);
         lua_pushlstring(L, str, str_len);
         return 1;
      case LUA_TTABLE:
         startsize = lua_gettop(L);
         lua_newtable(L);
         while (1) {
            ret = rb_load_rcsv(L, rb, 1); // key
            if (!ret) break;
            rb_load_rcsv(L, rb, 0); // value
            lua_settable(L, -3);
         }

         // read typename
         RB_READ(L, rb, &str_len, sizeof(str_len));
         str = alloca(str_len + 1);
         RB_READ(L, rb, str, str_len);
         str[str_len] = 0;

         if (str_len > 0) {
            luaL_getmetatable(L, str);
            lua_setmetatable(L, -2);
         }

         rb_stackCheck(L, startsize+1, "error loading table");
         return 1;
      case LUA_TFUNCTION:
         chunked.rb = rb;
         chunked.read_last = 0;
         startsize = lua_gettop(L);
#if !defined(LUA_VERSION_NUM) || LUA_VERSION_NUM < 502
         ret = lua_load(L, rb_lua_reader, &chunked, NULL);
#else
         ret = lua_load(L, rb_lua_reader, &chunked, NULL, NULL);
#endif

         if (ret) return -EINVAL;
         // LuaJIT reads the last 0 marker, regular Lua does not, even it out.
         if (!chunked.read_last) {
            RB_READ(L, rb, &str_len, sizeof(str_len));
         }
         rb_stackCheck(L, startsize+1, "error loading function");

         // are we expecting upvalues?
         int upval;
         RB_READ(L, rb, &upval, sizeof(int));

         if (upval == 1) {
            // read upvalue index of _ENV
            int envIdx;
            RB_READ(L, rb, &envIdx, sizeof(int));

            // read table of upvalues
            rb_load_rcsv(L, rb, 0);

            // set _ENV
            if (envIdx > 0) {
               lua_getglobal(L, "_G");
               lua_setupvalue(L, -3, envIdx);
            }

            // set function upvalues
            int top = lua_gettop(L);
            lua_pushnil(L);
            while (lua_next(L, top) != 0) {
               lua_Integer n = lua_tointeger(L, top + 1); // key
               lua_setupvalue(L, -4, n);
            }

            lua_pop(L, 1);
            rb_stackCheck(L, startsize+1, "error loading upvalues");
         }

         return 1;
      case LUA_TUSERDATA:
      case -LUA_TUSERDATA:
         RB_READ(L, rb, &str_len, sizeof(str_len));
         str = alloca(str_len + 1);
         RB_READ(L, rb, str, str_len);
         RB_READ(L, rb, &ptr, sizeof(void *));
         str[str_len] = 0;
         if (type >= 0) {
            luaT_pushudata(L, ptr, str);
         } else {
            pptr = (void **)lua_newuserdata(L, sizeof(void **));
            *pptr = ptr;
            luaL_getmetatable(L, str);
            lua_setmetatable(L, -2);
         }
         return 1;
      default:
         return -EINVAL;
   }
}

int rb_load(lua_State *L, ringbuffer_t *rb) {
   return rb_load_rcsv(L, rb, 0);
}
