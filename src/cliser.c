#include "TH.h"
#ifdef USE_CUDA
#include "THC/THC.h"
#endif
#include "luaT.h"
#include <errno.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/time.h>
#include "ringbuffer.h"
#include "serialize.h"
#include "cliser.h"
#include "error.h"

#define SEND_RECV_SIZE (16*1024)
#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_TIMEOUT_SECONDS (5*60)

typedef struct net_stats_t {
   uint64_t num_bytes;
   uint64_t num_regions;
   uint64_t num_calls;
   uint64_t num_system_calls;
   double total_seconds;
   double system_seconds;
   double cuda_sync_seconds;
   double cuda_ipc_seconds;
   uint64_t cuda_ipc_bytes;
} net_stats_t;

#ifdef USE_CUDA
typedef struct remote_ptr_t {
   cudaIpcMemHandle_t handle;
   void *ptr;
   void *dev_ptr;
   size_t count;
   int sock;
} remote_ptr_t;
#endif

typedef struct copy_context_t {
#ifdef USE_CUDA
   cudaEvent_t event;
   remote_ptr_t *remote_ptrs;
   size_t num_remote_ptrs;
#endif
   void *buf[2];
   struct net_stats_t tx;
   struct net_stats_t rx;
   int use_fastpath;
} copy_context_t;

typedef struct client_t {
   int sock;
   struct client_t *next;
   struct client_t *prev;
   struct ringbuffer_t *send_rb;
   struct ringbuffer_t *recv_rb;
   uint32_t timeout_seconds;
   struct copy_context_t copy_context;
   char *tag;
   int id;
   int ref_count;
} client_t;

typedef struct server_t {
   int sock;
   pthread_t listen_thread;
   struct client_t *clients;
   uint32_t num_clients;
   pthread_mutex_t clients_mutex;
   pthread_cond_t clients_cond;
   uint32_t timeout_seconds;
   struct copy_context_t copy_context;
   uint32_t ip_address;
} server_t;

typedef struct server_client_t {
   struct server_t *server;
   struct client_t *client;
} server_client_t;

static void insert_client(struct server_t *server, struct client_t *client) {
   pthread_mutex_lock(&server->clients_mutex);
   if (server->clients) {
      server->clients->prev = client;
      client->next = server->clients;
   }
   server->clients = client;
   server->num_clients++;
   pthread_cond_signal(&server->clients_cond);
   pthread_mutex_unlock(&server->clients_mutex);
}

static void remove_client(struct server_t *server, struct client_t *client) {
   pthread_mutex_lock(&server->clients_mutex);
   if (server->clients == client) {
      server->clients = client->next;
   }
   if (client->next) {
      client->next->prev = client->prev;
   }
   if (client->prev) {
      client->prev->next = client->next;
   }
   server->num_clients--;
   pthread_mutex_unlock(&server->clients_mutex);
}

static void destroy_copy_context(struct copy_context_t *copy_context) {
#ifdef USE_CUDA
   if (copy_context->event) {
      THCudaCheck(cudaEventDestroy(copy_context->event));
      copy_context->event = NULL;
   }
#endif
   if (copy_context->buf[0]) {
      free(copy_context->buf[0]);
      copy_context->buf[0] = NULL;
      copy_context->buf[1] = NULL;
   }
}

static void destroy_client(struct client_t *client) {
   int ret;

   if (client->sock) {
      // don't know how to do this nicely...
      //ret = shutdown(client->sock, SHUT_RDWR);
      //if (ret) HANDLE_ERROR(errno);
      ret = close(client->sock);
      if (ret) HANDLE_ERROR(errno);
      client->sock = 0;
   }
   if (client->send_rb) {
      ringbuffer_destroy(client->send_rb);
      client->send_rb = NULL;
   }
   if (client->recv_rb) {
      ringbuffer_destroy(client->recv_rb);
      client->recv_rb = NULL;
   }
   destroy_copy_context(&client->copy_context);
   if (client->tag) {
      free(client->tag);
      client->tag = NULL;
   }
   free(client);
}

static void destroy_server(struct server_t *server) {
   int ret;
   struct client_t *client;
   struct client_t *next;

   if (server->sock) {
      // on linux we need this to wake up the listen thread
      ret = shutdown(server->sock, SHUT_RDWR);
      // but on osx it causes an error, so we ignore
      //if (ret) HANDLE_ERROR(errno);
      ret = close(server->sock);
      if (ret) HANDLE_ERROR(errno);
      server->sock = 0;
   }
   if (server->listen_thread) {
      ret = pthread_join(server->listen_thread, NULL);
      if (ret) HANDLE_ERROR(ret);
      server->listen_thread = 0;
   }
   client = server->clients;
   while (client) {
      next = client->next;
      destroy_client(client);
      client = next;
   }
   server->clients = NULL;
   server->num_clients = 0;
   ret = pthread_mutex_destroy(&server->clients_mutex);
   if (ret) HANDLE_ERROR(ret);
   ret = pthread_cond_destroy(&server->clients_cond);
   if (ret) HANDLE_ERROR(ret);
   destroy_copy_context(&server->copy_context);
}

static void* listen_thread(void *arg) {
   struct server_t *server;
   int ret;
   struct sockaddr addr;
   socklen_t addrlen;
   struct client_t *client;
   int sock;
   int value;

   server = (struct server_t *)arg;
   ret = listen(server->sock, 1024);
   if (ret) {
      HANDLE_ERROR(errno);
      pthread_exit(0);
   }
   while (1) {
      addrlen = sizeof(addr);
      ret = accept(server->sock, &addr, &addrlen);
      if (ret > 0) {
         sock = ret;
         value = 1;
         ret = setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value));
         if (ret) HANDLE_ERROR(errno);
         client = (struct client_t *)calloc(1, sizeof(client_t));
         client->sock = sock;
         client->send_rb = ringbuffer_create(SEND_RECV_SIZE);
         client->recv_rb = ringbuffer_create(SEND_RECV_SIZE);
#ifndef __APPLE__
         if (server->ip_address == ((struct sockaddr_in *)&addr)->sin_addr.s_addr) {
            client->copy_context.use_fastpath = 1;
         }
#endif
         insert_client(server, client);
      } else {
         pthread_exit(0);
      }
   }
   return 0;
}

static int get_sockaddr(lua_State *L, const char *host, const char *port, struct sockaddr *addr, socklen_t *addrlen) {
   int ret;
   struct addrinfo *ai;
   struct addrinfo hints;

   memset(&hints, 0, sizeof(hints));
   hints.ai_family = AF_INET;
   hints.ai_socktype = SOCK_STREAM;
   hints.ai_protocol = IPPROTO_TCP;
   ret = getaddrinfo(host, port, &hints, &ai);
   if (ret) return LUA_HANDLE_ERROR_STR(L, gai_strerror(ret));
   if (*addrlen < ai->ai_addrlen) {
      freeaddrinfo(ai);
      return LUA_HANDLE_ERROR(L, ENOMEM);
   }
   memcpy(addr, ai->ai_addr, ai->ai_addrlen);
   *addrlen = ai->ai_addrlen;
   freeaddrinfo(ai);
   return 0;
}

int cliser_server(lua_State *L) {
   struct server_t *server;
   int ret;
   pthread_mutexattr_t mutex_attr;
   const char *host;
   int port;
   char port_str[16];
   struct sockaddr addr;
   socklen_t addrlen;
   int sock;
   struct sockaddr_in sin;

#ifdef USE_CUDA
   // sometimes cutorch is loaded late, this is a good spot to try and register...
   Lcliser_CudaInit(L);
#endif
   host = luaL_optstring(L, 1, DEFAULT_HOST);
   port = luaL_optinteger(L, 2, 0);
   sprintf(port_str, "%d", port);
   addrlen = sizeof(struct sockaddr);
   ret = get_sockaddr(L, host, port != 0 ? port_str : NULL, &addr, &addrlen);
   if (ret) return ret;
   ret = socket(PF_INET, SOCK_STREAM, 0);
   if (ret <= 0) return LUA_HANDLE_ERROR(L, errno);
   sock = ret;
   ret = bind(sock, &addr, addrlen);
   if (ret) {
      close(sock);
      return LUA_HANDLE_ERROR(L, errno);
   }
   addrlen = sizeof(sin);
   ret = getsockname(sock, (struct sockaddr *)&sin, &addrlen);
   if (ret) return LUA_HANDLE_ERROR(L, errno);
   port = ntohs(sin.sin_port);
   server = (server_t *)lua_newuserdata(L, sizeof(server_t));
   memset(server, 0, sizeof(server_t));
   server->sock = sock;
   server->ip_address = sin.sin_addr.s_addr;
   server->timeout_seconds = DEFAULT_TIMEOUT_SECONDS;
   ret = pthread_mutexattr_init(&mutex_attr);
   if (ret) {
      destroy_server(server);
      return LUA_HANDLE_ERROR(L, ret);
   }
   ret = pthread_mutexattr_settype(&mutex_attr, PTHREAD_MUTEX_RECURSIVE);
   if (ret) {
      destroy_server(server);
      return LUA_HANDLE_ERROR(L, ret);
   }
   ret = pthread_mutex_init(&server->clients_mutex, &mutex_attr);
   if (ret) {
      destroy_server(server);
      return LUA_HANDLE_ERROR(L, ret);
   }
   ret = pthread_cond_init(&server->clients_cond, NULL);
   if (ret) {
      destroy_server(server);
      return LUA_HANDLE_ERROR(L, ret);
   }
   ret = pthread_create(&server->listen_thread, NULL, listen_thread, server);
   if (ret) {
      destroy_server(server);
      return LUA_HANDLE_ERROR(L, ret);
   }
   luaL_getmetatable(L, "parallel.server");
   lua_setmetatable(L, -2);
   lua_pushinteger(L, port);
   return 2;
}

int cliser_client(lua_State *L) {
   struct client_t *client;
   struct client_t **clientp;
   const char *host;
   const char *port;
   int ret;
   int sock;
   struct sockaddr addr;
   struct sockaddr bind_addr;
   socklen_t addrlen;
   struct timeval tv;
   uint32_t t;
   int value;

#ifdef USE_CUDA
   // sometimes cutorch is loaded late, this is a good spot to try and register...
   Lcliser_CudaInit(L);
#endif
   if (lua_type(L, 1) == LUA_TSTRING) {
      host = lua_tostring(L, 1);
      port = lua_tostring(L, 2);
   } else {
      host = DEFAULT_HOST;
      port = lua_tostring(L, 1);
   }
   addrlen = sizeof(struct sockaddr);
   ret = get_sockaddr(L, host, port, &addr, &addrlen);
   if (ret) return ret;
   gettimeofday(&tv, NULL);
   t = tv.tv_sec + DEFAULT_TIMEOUT_SECONDS;
   sock = -1;
   while (tv.tv_sec < t) {
      ret = socket(PF_INET, SOCK_STREAM, 0);
      if (ret <= 0) return LUA_HANDLE_ERROR(L, errno);
      sock = ret;
      value = 1;
      ret = setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value));
      if (ret) break;
      ret = connect(sock, &addr, addrlen);
      if (!ret) break;
      close(sock);
      sleep(1);
      gettimeofday(&tv, NULL);
   }
   if (ret) {
      close(sock);
      return LUA_HANDLE_ERROR(L, errno);
   }
   addrlen = sizeof(struct sockaddr);
   ret = getsockname(sock, &bind_addr, &addrlen);
   if (ret) {
      close(sock);
      return LUA_HANDLE_ERROR(L, errno);
   }
   client = (client_t *)calloc(1, sizeof(client_t));
   client->sock = sock;
#ifndef __APPLE__
   if (((struct sockaddr_in *)&bind_addr)->sin_addr.s_addr == ((struct sockaddr_in *)&addr)->sin_addr.s_addr) {
      client->copy_context.use_fastpath = 1;
   }
#endif
   client->send_rb = ringbuffer_create(SEND_RECV_SIZE);
   client->recv_rb = ringbuffer_create(SEND_RECV_SIZE);
   client->timeout_seconds = DEFAULT_TIMEOUT_SECONDS;
   client->ref_count = 1;
   clientp = (client_t **)lua_newuserdata(L, sizeof(client_t *));
   *clientp = client;
   luaL_getmetatable(L, "parallel.client");
   lua_setmetatable(L, -2);
   return 1;
}

int cliser_server_close(lua_State *L) {
   struct server_t *server;

   server = (server_t *)lua_touserdata(L, 1);
   destroy_server(server);
   return 0;
}

int cliser_client_close(lua_State *L) {
   struct client_t **client;

   client = (client_t **)lua_touserdata(L, 1);
   if (*client) {
      (*client)->ref_count--;
      if ((*client)->ref_count == 0) {
         destroy_client(*client);
      }
      (*client) = NULL;
   }
   return 0;
}

int cliser_client_retain(lua_State *L) {
   struct client_t *client;

   client = *(client_t **)lua_touserdata(L, 1);
   client->ref_count++;
   return 0;
}

int cliser_client_metatablename(lua_State *L) {
   lua_pushstring(L, "parallel.client");
   return 1;
}

static int compare_clients(const void *a, const void *b) {
   return (*(const struct client_t**)a)->id - (*(const struct client_t**)b)->id;
}

static int compare_clients_inverted(const void *a, const void *b) {
   return (*(const struct client_t**)b)->id - (*(const struct client_t**)a)->id;
}

int cliser_server_clients(lua_State *L) {
   struct server_t *server;
   struct client_t *client;
   struct client_t **clients;
   struct server_client_t *server_client;
   uint32_t wait;
   struct timeval tv;
   struct timespec ts;
   int ret;
   int fi;
   uint32_t i, j;
   const char *tag;
   int invert_order;

   server = (server_t *)lua_touserdata(L, 1);
   if (lua_type(L, 2) == LUA_TFUNCTION) {
      wait = 0;
      fi = 2;
      if (lua_type(L, 3) == LUA_TSTRING) {
         tag = luaL_optstring(L, 3, NULL);
         invert_order = luaL_optinteger(L, 4, 0);
      } else {
         tag = NULL;
         invert_order = luaL_optinteger(L, 3, 0);
      }
   } else {
      wait = luaL_checkint(L, 2);
      if (lua_type(L, 3) != LUA_TFUNCTION) return LUA_HANDLE_ERROR_STR(L, "expected a callback function as argument #3");
      fi = 3;
      tag = NULL;
      invert_order = 0;
   }
   gettimeofday(&tv, NULL);
   ts.tv_sec = tv.tv_sec + server->timeout_seconds;
   ts.tv_nsec = 0;
   pthread_mutex_lock(&server->clients_mutex);
   while (wait > server->num_clients) {
      ret = pthread_cond_timedwait(&server->clients_cond, &server->clients_mutex, &ts);
      if (ret) {
         pthread_mutex_unlock(&server->clients_mutex);
         return LUA_HANDLE_ERROR(L, ret);
      }
   }
   clients = alloca(server->num_clients * sizeof(struct client_t*));
   client = server->clients;
   i = 0;
   while (client) {
      if (!tag || (client->tag && strcmp(tag, client->tag) == 0)) {
         clients[i] = client;
         i++;
      }
      client = client->next;
   }
   pthread_mutex_unlock(&server->clients_mutex);
   if (invert_order) {
      qsort(clients, i, sizeof(struct client_t*), compare_clients_inverted);
   } else {
      qsort(clients, i, sizeof(struct client_t*), compare_clients);
   }
   for (j = 0; j < i; j++) {
      lua_pushvalue(L, fi);
      server_client = (struct server_client_t *)lua_newuserdata(L, sizeof(struct server_client_t));
      server_client->server = server;
      server_client->client = clients[j];
      luaL_getmetatable(L, "parallel.server.client");
      lua_setmetatable(L, -2);
      lua_call(L, 1, 0);
      server_client->server = NULL;
      server_client->client = NULL;
   }
   lua_pushinteger(L, i);
   return 1;
}

int cliser_server_tag(lua_State *L) {
   struct server_client_t *server_client;
   const char *tag;

   server_client = (struct server_client_t *)lua_touserdata(L, 1);
   if (server_client->client == NULL) return LUA_HANDLE_ERROR_STR(L, "server client is invalid, either closed or used outside of server function scope");
   tag = luaL_optstring(L, 2, NULL);
   if (!tag) {
      if (server_client->client->tag) {
         lua_pushstring(L, server_client->client->tag);
         return 1;
      }
   } else {
      if (server_client->client->tag) {
         free(server_client->client->tag);
      }
      server_client->client->tag = strdup(tag);
   }
   return 0;
}

int cliser_server_id(lua_State *L) {
   struct server_client_t *server_client;

   server_client = (struct server_client_t *)lua_touserdata(L, 1);
   if (server_client->client == NULL) return LUA_HANDLE_ERROR_STR(L, "server client is invalid, either closed or used outside of server function scope");
   if (lua_gettop(L) > 1) {
      server_client->client->id = lua_tointeger(L, 2);
      return 0;
   }
   lua_pushinteger(L, server_client->client->id);
   return 1;
}

int cliser_server_client_close(lua_State *L) {
   struct server_client_t *server_client;

   server_client = (struct server_client_t *)lua_touserdata(L, 1);
   if (server_client->client == NULL) return LUA_HANDLE_ERROR_STR(L, "server client is invalid, either closed or used outside of server function scope");
   remove_client(server_client->server, server_client->client);
   destroy_client(server_client->client);
   server_client->server = NULL;
   server_client->client = NULL;
   return 0;
}

static int sock_send(int sock, void *ptr, size_t len, struct copy_context_t *copy_context) {
   int ret;
   double t0;

   t0 = _parallel_seconds();
   ret = send(sock, ptr, len, 0);
   copy_context->tx.system_seconds += (_parallel_seconds() - t0);
   copy_context->tx.num_bytes += len;
   copy_context->tx.num_system_calls++;
   return ret;
}

static int sock_recv(int sock, void *ptr, size_t len, struct copy_context_t *copy_context) {
   int ret;
   int rem;
   double t0;

   rem = len;
   while (rem > 0) {
      t0 = _parallel_seconds();
      ret = recv(sock, ptr, rem, 0);
      copy_context->rx.system_seconds += (_parallel_seconds() - t0);
      copy_context->rx.num_system_calls++;
      if (ret <= 0) return ret;
      rem -= ret;
      copy_context->rx.num_bytes += ret;
      ptr = ((uint8_t *)ptr) + ret;
   }
   return len;
}

int sock_send_raw(lua_State *L, int sock, void *ptr, size_t len, struct copy_context_t *copy_context) {
   int ret;

   ret = sock_send(sock, ptr, len, copy_context);
   if (ret < 0) return LUA_HANDLE_ERROR(L, errno);
   if ((size_t)ret != len) return LUA_HANDLE_ERROR_STR(L, "failed to send the correct number of bytes");
   return 0;
}

int sock_recv_raw(lua_State *L, int sock, void *ptr, size_t len, struct copy_context_t *copy_context) {
   int ret;

   ret = sock_recv(sock, ptr, len, copy_context);
   if (ret < 0) return LUA_HANDLE_ERROR(L, errno);
   if ((size_t)ret != len) return LUA_HANDLE_ERROR_STR(L, "failed to recv the correct number of bytes");
   return 0;
}

static int sock_send_msg(lua_State *L, int index, int sock, struct ringbuffer_t *rb, struct copy_context_t *copy_context) {
   int ret;
   size_t len;

   ringbuffer_push_write_pos(rb);
   ret = rb_save(L, index, rb, 1);
   if (ret) return LUA_HANDLE_ERROR(L, ret);
   len = ringbuffer_peek(rb);
   ringbuffer_pop_write_pos(rb);
   if (ret) return ret;
   ret = sock_send(sock, &len, sizeof(len), copy_context);
   if (ret < 0) return LUA_HANDLE_ERROR(L, errno);
   if (ret != sizeof(len)) return LUA_HANDLE_ERROR_STR(L, "failed to send the correct number of bytes");
   ret = sock_send(sock, ringbuffer_buf_ptr(rb), len, copy_context);
   if (ret < 0) return LUA_HANDLE_ERROR(L, errno);
   if ((size_t)ret != len) return LUA_HANDLE_ERROR_STR(L, "failed to send the correct number of bytes");
   return 0;
}

static int sock_recv_msg(lua_State *L, int sock, struct ringbuffer_t *rb, struct copy_context_t *copy_context) {
   int ret;
   size_t len;

   ret = sock_recv(sock, &len, sizeof(len), copy_context);
   if (ret < 0) return LUA_HANDLE_ERROR(L, errno);
   if (ret != sizeof(len)) return LUA_HANDLE_ERROR_STR(L, "failed to recv the correct number of bytes");
   if (len > SEND_RECV_SIZE) return LUA_HANDLE_ERROR_STR(L, "message size is too large");
   ret = sock_recv(sock, ringbuffer_buf_ptr(rb), len, copy_context);
   if (ret < 0) return LUA_HANDLE_ERROR(L, errno);
   if ((size_t)ret != len) return LUA_HANDLE_ERROR_STR(L, "failed to recv the correct number of bytes");
   ringbuffer_reset_read_pos(rb);
   ringbuffer_push_write_pos(rb);
   if (ringbuffer_write(rb, NULL, ret) != (size_t)ret) {
      ringbuffer_pop_write_pos(rb);
      return LUA_HANDLE_ERROR_STR(L, "failed to write the correct number of bytes into the ringbuffer");
   }
   ret = rb_load(L, rb);
   if (ret < 0) return LUA_HANDLE_ERROR(L, ret);
   return ret;
}

static int sock_recv_msg_peek(lua_State *L, int sock, struct ringbuffer_t *rb) {
   int ret;
   size_t len;

   ret = recv(sock, &len, sizeof(len), MSG_PEEK | MSG_DONTWAIT);
   if (ret < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
      return LUA_HANDLE_ERROR(L, errno);
   }
   if (ret != sizeof(len)) return 0;
   if (len > SEND_RECV_SIZE) return LUA_HANDLE_ERROR_STR(L, "message size is too large");
   ret = recv(sock, ringbuffer_buf_ptr(rb), len, MSG_PEEK | MSG_DONTWAIT);
   if (ret < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
      return LUA_HANDLE_ERROR(L, errno);
   }
   if ((size_t)ret != len) return 0;
   return ret;
}

static int sock_send_userdata(lua_State *L, int index, int sock, struct copy_context_t *copy_context) {
   if (!luaL_getmetafield(L, index, "_cliser_write")) return LUA_HANDLE_ERROR_STR(L, "could not find _cliser_write function in metatable");
   lua_pushvalue(L, index);
   lua_pushinteger(L, sock);
   lua_pushlightuserdata(L, copy_context);
   if (lua_type(L, index + 1) == LUA_TUSERDATA) {
      lua_pushvalue(L, index + 1);
      lua_call(L, 4, 0);
   } else {
      lua_call(L, 3, 0);
   }
   return 0;
}

static int sock_recv_userdata(lua_State *L, int index, int sock, struct copy_context_t *copy_context) {
   if (!luaL_getmetafield(L, index, "_cliser_read")) return LUA_HANDLE_ERROR_STR(L, "could not find _cliser_read function in metatable");
   lua_pushvalue(L, index);
   lua_pushinteger(L, sock);
   lua_pushlightuserdata(L, copy_context);
   if (lua_type(L, index + 1) == LUA_TUSERDATA) {
      lua_pushvalue(L, index + 1);
      lua_call(L, 4, 0);
   } else {
      lua_call(L, 3, 0);
   }
   return 0;
}

int cliser_server_send(lua_State *L) {
   struct server_client_t *server_client;
   int ret;
   double t0;

   t0 = _parallel_seconds();
   server_client = (server_client_t *)lua_touserdata(L, 1);
   if (server_client->client == NULL) return LUA_HANDLE_ERROR_STR(L, "server client is invalid, either closed or used outside of server function scope");
   if (lua_type(L, 2) == LUA_TUSERDATA) {
      server_client->server->copy_context.use_fastpath = server_client->client->copy_context.use_fastpath;
      ret = sock_send_userdata(L, 2, server_client->client->sock, &server_client->server->copy_context);
   } else {
      ret = sock_send_msg(L, 2, server_client->client->sock, server_client->client->send_rb, &server_client->server->copy_context);
   }
   server_client->server->copy_context.tx.total_seconds += (_parallel_seconds() - t0);
   server_client->server->copy_context.tx.num_calls++;
   return ret;
}

int cliser_server_recv(lua_State *L) {
   struct server_client_t *server_client;
   int ret;
   double t0;

   t0 = _parallel_seconds();
   server_client = (server_client_t *)lua_touserdata(L, 1);
   if (server_client->client == NULL) return LUA_HANDLE_ERROR_STR(L, "server client is invalid, either closed or used outside of server function scope");
   if (lua_type(L, 2) == LUA_TUSERDATA) {
      server_client->server->copy_context.use_fastpath = server_client->client->copy_context.use_fastpath;
      ret = sock_recv_userdata(L, 2, server_client->client->sock, &server_client->server->copy_context);
      if (ret == 0) {
         lua_pushvalue(L, 2);
         ret = 1;
      }
   } else {
      ret = sock_recv_msg(L, server_client->client->sock, server_client->client->recv_rb, &server_client->server->copy_context);
   }
   server_client->server->copy_context.rx.total_seconds += (_parallel_seconds() - t0);
   server_client->server->copy_context.rx.num_calls++;
   return ret;
}

int cliser_server_broadcast(lua_State *L) {
   struct server_t *server;
   struct client_t *client;
   struct client_t **clients;
   int i, j;
   const char *tag;
   int ret;
   double t0;

   t0 = _parallel_seconds();
   server = (server_t *)lua_touserdata(L, 1);
   tag = luaL_optstring(L, 3, NULL);
   pthread_mutex_lock(&server->clients_mutex);
   clients = alloca(server->num_clients * sizeof(struct client_t*));
   client = server->clients;
   i = 0;
   while (client) {
      if (!tag || (client->tag && strcmp(tag, client->tag) == 0)) {
         clients[i] = client;
         i++;
      }
      client = client->next;
   }
   pthread_mutex_unlock(&server->clients_mutex);
   qsort(clients, i, sizeof(struct client_t*), compare_clients);
   ret = 0;
   for (j = 0; j < i; j++) {
      client = clients[j];
      if (lua_type(L, 2) == LUA_TUSERDATA) {
         ret = sock_send_userdata(L, 2, client->sock, &server->copy_context);
      } else {
         ret = sock_send_msg(L, 2, client->sock, client->send_rb, &server->copy_context);
      }
      if (ret) break;
   }
   server->copy_context.tx.total_seconds += (_parallel_seconds() - t0);
   server->copy_context.tx.num_calls++;
   return ret;
}

int cliser_server_recv_any(lua_State *L) {
   struct server_t *server;
   struct client_t *client;
   struct server_client_t *server_client;
   const char *tag;
   fd_set fds;
   int ret;
   int highest;
   double t0;

   t0 = _parallel_seconds();
   server = (server_t *)lua_touserdata(L, 1);
   tag = luaL_optstring(L, 2, NULL);
   FD_ZERO(&fds);
   pthread_mutex_lock(&server->clients_mutex);
   highest = -1;
   client = server->clients;
   while (client) {
      if (!tag || (client->tag && strcmp(tag, client->tag) == 0)) {
         FD_SET(client->sock, &fds);
         if (client->sock > highest) {
            highest = client->sock;
         }
      }
      client = client->next;
   }
   ret = select(highest + 1, &fds, NULL, NULL, NULL);
   if (ret < 0) {
      pthread_mutex_unlock(&server->clients_mutex);
      return LUA_HANDLE_ERROR(L, errno);
   }
   client = server->clients;
   while (client) {
      if (!tag || (client->tag && strcmp(tag, client->tag) == 0)) {
         if (FD_ISSET(client->sock, &fds)) {
            ret = sock_recv_msg(L, client->sock, client->recv_rb, &server->copy_context);
            if (ret == 1) {
               server_client = (struct server_client_t *)lua_newuserdata(L, sizeof(struct server_client_t));
               server_client->server = server;
               server_client->client = client;
               luaL_getmetatable(L, "parallel.server.client");
               lua_setmetatable(L, -2);
               ret = 2;
            }
            break;
         }
      }
      client = client->next;
   }
   pthread_mutex_unlock(&server->clients_mutex);
   server->copy_context.rx.total_seconds += (_parallel_seconds() - t0);
   server->copy_context.rx.num_calls++;
   return ret;
}

int cliser_client_send(lua_State *L) {
   struct client_t *client;
   int ret;
   double t0;

   t0 = _parallel_seconds();
   client = *(client_t **)lua_touserdata(L, 1);
   if (lua_type(L, 2) == LUA_TUSERDATA) {
      ret = sock_send_userdata(L, 2, client->sock, &client->copy_context);
   } else {
      ret = sock_send_msg(L, 2, client->sock, client->send_rb, &client->copy_context);
   }
   client->copy_context.tx.total_seconds += (_parallel_seconds() - t0);
   client->copy_context.tx.num_calls++;
   return ret;
}

int cliser_client_recv(lua_State *L) {
   struct client_t *client;
   int ret;
   double t0;

   t0 = _parallel_seconds();
   client = *(client_t **)lua_touserdata(L, 1);
   if (lua_type(L, 2) == LUA_TUSERDATA) {
      ret = sock_recv_userdata(L, 2, client->sock, &client->copy_context);
      if (ret == 0) {
         lua_pushvalue(L, 2);
         ret = 1;
      }
   } else {
      ret = sock_recv_msg(L, client->sock, client->recv_rb, &client->copy_context);
   }
   client->copy_context.rx.total_seconds += (_parallel_seconds() - t0);
   client->copy_context.rx.num_calls++;
   return ret;
}

int cliser_client_recv_async(lua_State *L) {
   struct client_t *client;
   int ret;
   double t0;

   t0 = _parallel_seconds();
   client = *(client_t **)lua_touserdata(L, 1);
   ret = sock_recv_msg_peek(L, client->sock, client->recv_rb);
   if (ret > 0) {
      ret = sock_recv_msg(L, client->sock, client->recv_rb, &client->copy_context);
   }
   client->copy_context.rx.total_seconds += (_parallel_seconds() - t0);
   client->copy_context.rx.num_calls++;
   return ret;
}

int cliser_net_stats_inner(lua_State *L, struct net_stats_t *net_stats) {
   lua_newtable(L);
   lua_pushstring(L, "num_bytes");
   lua_pushnumber(L, net_stats->num_bytes);
   lua_settable(L, -3);
   lua_pushstring(L, "num_regions");
   lua_pushnumber(L, net_stats->num_regions);
   lua_settable(L, -3);
   lua_pushstring(L, "num_calls");
   lua_pushnumber(L, net_stats->num_calls);
   lua_settable(L, -3);
   lua_pushstring(L, "num_system_calls");
   lua_pushnumber(L, net_stats->num_system_calls);
   lua_settable(L, -3);
   lua_pushstring(L, "total_seconds");
   lua_pushnumber(L, net_stats->total_seconds);
   lua_settable(L, -3);
   lua_pushstring(L, "system_seconds");
   lua_pushnumber(L, net_stats->system_seconds);
   lua_settable(L, -3);
   lua_pushstring(L, "cuda_sync_seconds");
   lua_pushnumber(L, net_stats->cuda_sync_seconds);
   lua_settable(L, -3);
   lua_pushstring(L, "cuda_ipc_seconds");
   lua_pushnumber(L, net_stats->cuda_ipc_seconds);
   lua_settable(L, -3);
   lua_pushstring(L, "cuda_ipc_bytes");
   lua_pushnumber(L, net_stats->cuda_ipc_bytes);
   lua_settable(L, -3);
   lua_pushstring(L, "NETWORK MB/s");
   lua_pushnumber(L, ((double)net_stats->num_bytes / (1024.0*1024.0)) / net_stats->total_seconds);
   lua_settable(L, -3);
   lua_pushstring(L, "CUDA IPC MB/s");
   lua_pushnumber(L, ((double)net_stats->cuda_ipc_bytes / (1024.0*1024.0)) / net_stats->cuda_ipc_seconds);
   lua_settable(L, -3);
   return 1;
}

int cliser_net_stats(lua_State *L, struct copy_context_t *copy_context) {
   lua_newtable(L);
   lua_pushstring(L, "tx");
   cliser_net_stats_inner(L, &copy_context->tx);
   lua_settable(L, -3);
   lua_pushstring(L, "rx");
   cliser_net_stats_inner(L, &copy_context->rx);
   lua_settable(L, -3);
   return 1;
}

int cliser_server_net_stats(lua_State *L) {
   struct server_t *server;

   server = (server_t *)lua_touserdata(L, 1);
   return cliser_net_stats(L, &server->copy_context);
}

int cliser_client_net_stats(lua_State *L) {
   struct client_t *client;

   client = *(client_t **)lua_touserdata(L, 1);
   return cliser_net_stats(L, &client->copy_context);
}

#define torch_(NAME) TH_CONCAT_3(torch_, Real, NAME)
#define torch_Storage TH_CONCAT_STRING_3(torch., Real, Storage)
#define torch_Tensor TH_CONCAT_STRING_3(torch., Real, Tensor)
#define Lcliser_(NAME) TH_CONCAT_3(Lcliser_, Real, NAME)

#include "src/generic/cliser.c"
#include "THGenerateAllTypes.h"

#ifdef USE_CUDA
#define CLISER_IS_CUDA
#include "src/generic/cliser.c"
#define real float
#define accreal float
#define Real Cuda
#define TH_REAL_IS_FLOAT
#line 1 TH_GENERIC_FILE
#include TH_GENERIC_FILE
#undef real
#undef accreal
#undef Real
#undef TH_REAL_IS_FLOAT
#endif
