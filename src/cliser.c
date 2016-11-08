#include "TH.h"
#ifdef USE_CUDA
#include "THC/THC.h"
#endif
#include "luaT.h"
#include <errno.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <arpa/inet.h>
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
#define LEN_INVALID 0xFFFFFFFFFFFFFFFFULL

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
   size_t max_remote_ptrs;
   void *buf[2];
#endif
   net_stats_t tx;
   net_stats_t rx;
   int use_fastpath;
} copy_context_t;

typedef struct client_t {
   int sock;
   struct client_t *next;
   struct client_t *prev;
   ringbuffer_t *send_rb;
   ringbuffer_t *recv_rb;
   copy_context_t copy_context;
   char *tag;
   int id;
   int ref_count;
} client_t;

typedef struct server_t {
   int sock;
   client_t *clients;
   uint32_t num_clients;
   copy_context_t copy_context;
   uint32_t ip_address;
} server_t;

typedef struct server_client_t {
   server_t *server;
   client_t *client;
} server_client_t;

static double cliser_profile_seconds() {
   struct timeval tv;
   gettimeofday(&tv, NULL);
   return tv.tv_sec + tv.tv_usec / 1e6;
}

static void insert_client(server_t *server, client_t *client) {
   if (server->clients) {
      server->clients->prev = client;
      client->next = server->clients;
   }
   server->clients = client;
   server->num_clients++;
}

static void remove_client(server_t *server, client_t *client) {
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
}

static void destroy_copy_context(copy_context_t *copy_context) {
#ifdef USE_CUDA
   if (copy_context->event) {
      THCudaCheck(cudaEventDestroy(copy_context->event));
      copy_context->event = NULL;
   }
   if (copy_context->buf[0]) {
      free(copy_context->buf[0]);
      copy_context->buf[0] = NULL;
      copy_context->buf[1] = NULL;
   }
#else
   (void)copy_context;
#endif
}

static int destroy_client(lua_State *L, client_t *client) {
   if (client->sock) {
      size_t msg = LEN_INVALID;
      send(client->sock, &msg, sizeof(msg), 0);
      int ret = close(client->sock);
      if (ret) return LUA_HANDLE_ERROR(L, errno);
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
   return 0;
}

static int destroy_server(lua_State *L, server_t *server) {
   if (server->sock) {
      int ret = close(server->sock);
      if (ret) return LUA_HANDLE_ERROR(L, errno);
      server->sock = 0;
   }
   client_t *client = server->clients;
   while (client) {
      client_t *next = client->next;
      destroy_client(L, client);
      client = next;
   }
   server->clients = NULL;
   server->num_clients = 0;
   destroy_copy_context(&server->copy_context);
   return 0;
}

static int get_sockaddr(lua_State *L, const char *host, const char *port, struct sockaddr *addr, socklen_t *addrlen) {
   struct addrinfo hints;
   memset(&hints, 0, sizeof(hints));
   hints.ai_family = AF_INET;
   hints.ai_socktype = SOCK_STREAM;
   hints.ai_protocol = IPPROTO_TCP;
   struct addrinfo *ai;
   int ret = getaddrinfo(host, port, &hints, &ai);
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

static void configure_socket(int sock) {
   int value = 1;
   setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(int));
#ifndef __APPLE__
   int idle = 60;
   int interval = 30;
   int count = 8;
   setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &value, sizeof(int));
   setsockopt(sock, SOL_TCP, TCP_KEEPIDLE, &idle, sizeof(int));
   setsockopt(sock, SOL_TCP, TCP_KEEPINTVL, &interval, sizeof(int));
   setsockopt(sock, SOL_TCP, TCP_KEEPCNT, &count, sizeof(int));
#endif
}

int cliser_server(lua_State *L) {
#ifdef USE_CUDA
   // sometimes cutorch is loaded late, this is a good spot to try and register...
   Lcliser_CudaInit(L);
#endif
   const char *host = luaL_optstring(L, 1, DEFAULT_HOST);
   int port = luaL_optinteger(L, 2, 0);
   char port_str[16];
   snprintf(port_str, 16, "%d", port);
   struct sockaddr addr;
   socklen_t addrlen = sizeof(struct sockaddr);
   int ret = get_sockaddr(L, host, port != 0 ? port_str : NULL, &addr, &addrlen);
   if (ret) return ret;
   ret = socket(PF_INET, SOCK_STREAM, 0);
   if (ret <= 0) return LUA_HANDLE_ERROR(L, errno);
   int sock = ret;
   ret = bind(sock, &addr, addrlen);
   if (ret) {
      close(sock);
      return LUA_HANDLE_ERROR(L, errno);
   }
   ret = listen(sock, 1024);
   if (ret) {
      close(sock);
      return LUA_HANDLE_ERROR(L, errno);
   }
   struct sockaddr_in sin;
   addrlen = sizeof(struct sockaddr_in);
   ret = getsockname(sock, (struct sockaddr *)&sin, &addrlen);
   if (ret) {
      close(sock);
      return LUA_HANDLE_ERROR(L, errno);
   }
   port = ntohs(sin.sin_port);
   server_t *server = (server_t *)lua_newuserdata(L, sizeof(server_t));
   memset(server, 0, sizeof(server_t));
   server->sock = sock;
   server->ip_address = sin.sin_addr.s_addr;
   luaL_getmetatable(L, "ipc.server");
   lua_setmetatable(L, -2);
   lua_pushinteger(L, port);
   return 2;
}

static int can_use_fastpath(lua_State *L, int sock, uint32_t bind_addr, uint32_t addr) {
#if defined(USE_CUDA) && !defined(__APPLE__)
   if (bind_addr == addr) {
      int device;
      cudaError_t err = cudaGetDevice(&device);
      if (err != cudaSuccess) {
         if (err == cudaErrorNoDevice) {
            cudaGetLastError();
            return 0;
         } else {
            THCudaCheck(err);
         }
      }
      int ret = send(sock, &device, sizeof(device), 0);
      if (ret < 0) {
         close(sock);
         return LUA_HANDLE_ERROR(L, errno);
      }
      int remote_device;
      ret = recv(sock, &remote_device, sizeof(remote_device), 0);
      if (ret <= 0) {
         close(sock);
         return LUA_HANDLE_ERROR(L, errno);
      }
      if (device != remote_device) {
         int can;
         THCudaCheck(cudaDeviceCanAccessPeer(&can, device, remote_device));
         if (can) {
            cudaError_t err = cudaDeviceEnablePeerAccess(remote_device, 0);
            if (err == cudaSuccess || err == cudaErrorPeerAccessAlreadyEnabled) {
               if (err == cudaErrorPeerAccessAlreadyEnabled) cudaGetLastError();
               fprintf(stderr, "INFO: torch-ipc: CUDA IPC enabled between GPU%d and GPU%d\n", device, remote_device);
               return 1;
            } else {
               fprintf(stderr, "WARN: torch-ipc: CUDA IPC disabled between GPU%d and GPU%d: %s\n", device, remote_device, cudaGetErrorString(err));
            }
         } else {
            fprintf(stderr, "INFO: torch-ipc: CUDA IPC not possible between GPU%d and GPU%d\n", device, remote_device);
         }
      }
   }
#else
   (void)L;
   (void)sock;
   (void)bind_addr;
   (void)addr;
#endif
   return 0;
}

int cliser_client(lua_State *L) {
#ifdef USE_CUDA
   // sometimes cutorch is loaded late, this is a good spot to try and register...
   Lcliser_CudaInit(L);
#endif
   const char *host;
   const char *port;
   if (lua_type(L, 1) == LUA_TSTRING) {
      host = lua_tostring(L, 1);
      port = lua_tostring(L, 2);
   } else {
      host = DEFAULT_HOST;
      port = lua_tostring(L, 1);
   }
   socklen_t addrlen = sizeof(struct sockaddr);
   struct sockaddr addr;
   int ret = get_sockaddr(L, host, port, &addr, &addrlen);
   if (ret) return ret;
   struct timeval tv;
   gettimeofday(&tv, NULL);
   time_t t = tv.tv_sec + DEFAULT_TIMEOUT_SECONDS;
   int sock = -1;
   while (tv.tv_sec < t) {
      ret = socket(PF_INET, SOCK_STREAM, 0);
      if (ret <= 0) return LUA_HANDLE_ERROR(L, errno);
      sock = ret;
      configure_socket(ret);
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
   struct sockaddr bind_addr;
   ret = getsockname(sock, &bind_addr, &addrlen);
   if (ret) {
      close(sock);
      return LUA_HANDLE_ERROR(L, errno);
   }
   int use_fastpath = can_use_fastpath(L, sock, ((struct sockaddr_in *)&bind_addr)->sin_addr.s_addr, ((struct sockaddr_in *)&addr)->sin_addr.s_addr);
   client_t *client = (client_t *)calloc(1, sizeof(client_t));
   client->sock = sock;
   client->send_rb = ringbuffer_create(SEND_RECV_SIZE);
   client->recv_rb = ringbuffer_create(SEND_RECV_SIZE);
   client->ref_count = 1;
   client->copy_context.use_fastpath = use_fastpath;
   client_t **clientp = (client_t **)lua_newuserdata(L, sizeof(client_t *));
   *clientp = client;
   luaL_getmetatable(L, "ipc.client");
   lua_setmetatable(L, -2);
   return 1;
}

int cliser_server_close(lua_State *L) {
   server_t *server = (server_t *)lua_touserdata(L, 1);
   return destroy_server(L, server);
}

int cliser_client_close(lua_State *L) {
   client_t **client = (client_t **)lua_touserdata(L, 1);
   if (*client) {
      (*client)->ref_count--;
      if ((*client)->ref_count == 0) {
         destroy_client(L, *client);
      }
      (*client) = NULL;
   }
   return 0;
}

int cliser_client_retain(lua_State *L) {
   client_t *client = *(client_t **)lua_touserdata(L, 1);
   client->ref_count++;
   return 0;
}

int cliser_client_metatablename(lua_State *L) {
   lua_pushstring(L, "ipc.client");
   return 1;
}

static int compare_clients(const void *a, const void *b) {
   return (*(const client_t**)a)->id - (*(const client_t**)b)->id;
}

static int compare_clients_inverted(const void *a, const void *b) {
   return (*(const client_t**)b)->id - (*(const client_t**)a)->id;
}

int cliser_server_clients(lua_State *L) {
   server_t *server = (server_t *)lua_touserdata(L, 1);
   uint32_t wait;
   const char *tag;
   int fi;
   int invert_order;
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
   struct timeval tv;
   gettimeofday(&tv, NULL);
   uint32_t t = tv.tv_sec + DEFAULT_TIMEOUT_SECONDS;
   while (wait > server->num_clients) {
      fd_set acceptfds;
      FD_ZERO(&acceptfds);
      FD_SET(server->sock, &acceptfds);
      struct timeval accepttv;
      accepttv.tv_sec = 30;
      accepttv.tv_usec = 0;
      int ret = select(server->sock + 1, &acceptfds, NULL, NULL, &accepttv);
      if (ret > 0 && FD_ISSET(server->sock, &acceptfds)) {
         struct sockaddr addr;
         socklen_t addrlen = sizeof(addr);
         int ret = accept(server->sock, &addr, &addrlen);
         if (ret <= 0) return LUA_HANDLE_ERROR(L, errno);
         int sock = ret;
         configure_socket(ret);
         int use_fastpath = can_use_fastpath(L, sock, server->ip_address, ((struct sockaddr_in *)&addr)->sin_addr.s_addr);
         client_t *client = (client_t *)calloc(1, sizeof(client_t));
         client->sock = sock;
         client->send_rb = ringbuffer_create(SEND_RECV_SIZE);
         client->recv_rb = ringbuffer_create(SEND_RECV_SIZE);
         client->copy_context.use_fastpath = use_fastpath;
         insert_client(server, client);
      }
      gettimeofday(&tv, NULL);
      if (tv.tv_sec > t) {
         return LUA_HANDLE_ERROR_STR(L, "server timed out waiting for clients to connect");
      }
   }
   client_t **clients = alloca(server->num_clients * sizeof(client_t*));
   client_t *client = server->clients;
   uint32_t i = 0;
   while (client) {
      if (!tag || (client->tag && strcmp(tag, client->tag) == 0)) {
         clients[i] = client;
         i++;
      }
      client = client->next;
   }
   if (invert_order) {
      qsort(clients, i, sizeof(client_t*), compare_clients_inverted);
   } else {
      qsort(clients, i, sizeof(client_t*), compare_clients);
   }
   for (uint32_t j = 0; j < i; j++) {
      lua_pushvalue(L, fi);
      server_client_t *server_client = (server_client_t *)lua_newuserdata(L, sizeof(server_client_t));
      server_client->server = server;
      server_client->client = clients[j];
      luaL_getmetatable(L, "ipc.server.client");

      lua_setmetatable(L, -2);
      lua_call(L, 1, 0);
      server_client->server = NULL;
      server_client->client = NULL;
   }
   lua_pushinteger(L, i);
   return 1;
}

int cliser_server_tag(lua_State *L) {
   server_client_t *server_client = (server_client_t *)lua_touserdata(L, 1);
   if (server_client->client == NULL) return LUA_HANDLE_ERROR_STR(L, "server client is invalid, either closed or used outside of server function scope");
   const char *tag = luaL_optstring(L, 2, NULL);
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
   server_client_t *server_client = (server_client_t *)lua_touserdata(L, 1);
   if (server_client->client == NULL) return LUA_HANDLE_ERROR_STR(L, "server client is invalid, either closed or used outside of server function scope");
   if (lua_gettop(L) > 1) {
      server_client->client->id = lua_tointeger(L, 2);
      return 0;
   }
   lua_pushinteger(L, server_client->client->id);
   return 1;
}

int cliser_server_client_close(lua_State *L) {
   server_client_t *server_client = (server_client_t *)lua_touserdata(L, 1);
   if (server_client->client == NULL) return LUA_HANDLE_ERROR_STR(L, "server client is invalid, either closed or used outside of server function scope");
   remove_client(server_client->server, server_client->client);
   destroy_client(L, server_client->client);
   server_client->server = NULL;
   server_client->client = NULL;
   return 0;
}

char* sock_address(int sock) {
   socklen_t len = sizeof(struct sockaddr_in);
   struct sockaddr_in addr;
   getpeername(sock, (struct sockaddr*)&addr, &len);
   char *ip = inet_ntoa(((struct sockaddr_in*)&addr)->sin_addr);
   return ip;
}

int cliser_server_client_address(lua_State *L) {
   server_client_t *server_client = (server_client_t *)lua_touserdata(L, 1);
   lua_pushstring(L, sock_address(server_client->client->sock));
   return 1;
}

static size_t sock_send(int sock, void *ptr, size_t len, copy_context_t *copy_context) {
   size_t rem = len;
   while (rem > 0) {
      double t0 = cliser_profile_seconds();
      ssize_t ret = send(sock, ptr, rem, 0);
      copy_context->tx.system_seconds += (cliser_profile_seconds() - t0);
      copy_context->tx.num_system_calls++;
      if (ret < 0) {
         return 0;
      }
      rem -= (size_t)ret;
      copy_context->tx.num_bytes += ret;
      ptr = ((uint8_t *)ptr) + ret;
   }
   return len;
}

static size_t sock_recv(int sock, void *ptr, size_t len, copy_context_t *copy_context) {
   size_t rem = len;
   while (rem > 0) {
      double t0 = cliser_profile_seconds();
      ssize_t ret = recv(sock, ptr, rem, 0);
      copy_context->rx.system_seconds += (cliser_profile_seconds() - t0);
      copy_context->rx.num_system_calls++;
      if (ret < 0) {
         return 0;
      }
      rem -= (size_t)ret;
      copy_context->rx.num_bytes += ret;
      ptr = ((uint8_t *)ptr) + ret;
   }
   return len;
}

static int sock_send_raw(lua_State *L, int sock, void *ptr, size_t len, copy_context_t *copy_context) {
   int ret = sock_send(sock, ptr, len, copy_context);
   if (ret < 0) return LUA_HANDLE_ERROR(L, errno);
   if ((size_t)ret != len) return LUA_HANDLE_ERROR_STR(L, "failed to send the correct number of bytes");
   return 0;
}

static int sock_recv_raw(lua_State *L, int sock, void *ptr, size_t len, copy_context_t *copy_context) {
   int ret = sock_recv(sock, ptr, len, copy_context);
   if (ret < 0) return LUA_HANDLE_ERROR(L, errno);
   if ((size_t)ret != len) return LUA_HANDLE_ERROR_STR(L, "failed to recv the correct number of bytes");
   return 0;
}

static int sock_send_msg(lua_State *L, int index, int sock, ringbuffer_t *rb, copy_context_t *copy_context) {
   ringbuffer_push_write_pos(rb);
   int ret = rb_save(L, index, rb, 1, 0);
   if (ret) return LUA_HANDLE_ERROR(L, ret);
   size_t len = ringbuffer_peek(rb);
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

static int sock_recv_msg(lua_State *L, int sock, ringbuffer_t *rb, copy_context_t *copy_context) {
   size_t len;
   int ret = sock_recv(sock, &len, sizeof(len), copy_context);
   if (len == LEN_INVALID) {
      return LUA_HANDLE_ERROR_STR(L, "remote peer disconnected\n");
   }
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

static int sock_recv_msg_peek(lua_State *L, int sock, ringbuffer_t *rb) {
   size_t len;
   int ret = recv(sock, &len, sizeof(len), MSG_PEEK | MSG_DONTWAIT);
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

static int sock_send_userdata(lua_State *L, int index, int sock, copy_context_t *copy_context) {
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

static int sock_recv_userdata(lua_State *L, int index, int sock, copy_context_t *copy_context) {
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
   double t0 = cliser_profile_seconds();
   server_client_t *server_client = (server_client_t *)lua_touserdata(L, 1);
   if (server_client->client == NULL) return LUA_HANDLE_ERROR_STR(L, "server client is invalid, either closed or used outside of server function scope");
   int ret;
   if (lua_type(L, 2) == LUA_TUSERDATA) {
      server_client->server->copy_context.use_fastpath = server_client->client->copy_context.use_fastpath;
      ret = sock_send_userdata(L, 2, server_client->client->sock, &server_client->server->copy_context);
   } else {
      ret = sock_send_msg(L, 2, server_client->client->sock, server_client->client->send_rb, &server_client->server->copy_context);
   }
   server_client->server->copy_context.tx.total_seconds += (cliser_profile_seconds() - t0);
   server_client->server->copy_context.tx.num_calls++;
   return ret;
}

int cliser_server_recv(lua_State *L) {
   double t0 = cliser_profile_seconds();
   server_client_t *server_client = (server_client_t *)lua_touserdata(L, 1);
   if (server_client->client == NULL) return LUA_HANDLE_ERROR_STR(L, "server client is invalid, either closed or used outside of server function scope");
   int ret;
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
   server_client->server->copy_context.rx.total_seconds += (cliser_profile_seconds() - t0);
   server_client->server->copy_context.rx.num_calls++;
   return ret;
}

int cliser_server_broadcast(lua_State *L) {
   double t0 = cliser_profile_seconds();
   server_t *server = (server_t *)lua_touserdata(L, 1);
   const char *tag = luaL_optstring(L, 3, NULL);
   client_t **clients = alloca(server->num_clients * sizeof(client_t*));
   client_t *client = server->clients;
   int i = 0;
   while (client) {
      if (!tag || (client->tag && strcmp(tag, client->tag) == 0)) {
         clients[i] = client;
         i++;
      }
      client = client->next;
   }
   qsort(clients, i, sizeof(client_t*), compare_clients);
   int ret = 0;
   for (int j = 0; j < i; j++) {
      client = clients[j];
      if (lua_type(L, 2) == LUA_TUSERDATA) {
         ret = sock_send_userdata(L, 2, client->sock, &server->copy_context);
      } else {
         ret = sock_send_msg(L, 2, client->sock, client->send_rb, &server->copy_context);
      }
      if (ret) break;
   }
   server->copy_context.tx.total_seconds += (cliser_profile_seconds() - t0);
   server->copy_context.tx.num_calls++;
   return ret;
}

int cliser_server_recv_any(lua_State *L) {
   double t0 = cliser_profile_seconds();
   server_t *server = (server_t *)lua_touserdata(L, 1);
   const char *tag = luaL_optstring(L, 2, NULL);
   fd_set fds;
   FD_ZERO(&fds);
   int highest = -1;
   client_t *client = server->clients;
   while (client) {
      if (!tag || (client->tag && strcmp(tag, client->tag) == 0)) {
         FD_SET(client->sock, &fds);
         if (client->sock > highest) {
            highest = client->sock;
         }
      }
      client = client->next;
   }
   int ret = select(highest + 1, &fds, NULL, NULL, NULL);
   if (ret < 0) return LUA_HANDLE_ERROR(L, errno);
   client = server->clients;
   while (client) {
      if (!tag || (client->tag && strcmp(tag, client->tag) == 0)) {
         if (FD_ISSET(client->sock, &fds)) {
            ret = sock_recv_msg(L, client->sock, client->recv_rb, &server->copy_context);
            if (ret == 1) {
               server_client_t *server_client = (server_client_t *)lua_newuserdata(L, sizeof(server_client_t));
               server_client->server = server;
               server_client->client = client;
               luaL_getmetatable(L, "ipc.server.client");
               lua_setmetatable(L, -2);
               ret = 2;
            }
            break;
         }
      }
      client = client->next;
   }
   server->copy_context.rx.total_seconds += (cliser_profile_seconds() - t0);
   server->copy_context.rx.num_calls++;
   return ret;
}

int cliser_client_send(lua_State *L) {
   double t0 = cliser_profile_seconds();
   client_t *client = *(client_t **)lua_touserdata(L, 1);
   int ret;
   if (lua_type(L, 2) == LUA_TUSERDATA) {
      ret = sock_send_userdata(L, 2, client->sock, &client->copy_context);
   } else {
      ret = sock_send_msg(L, 2, client->sock, client->send_rb, &client->copy_context);
   }
   client->copy_context.tx.total_seconds += (cliser_profile_seconds() - t0);
   client->copy_context.tx.num_calls++;
   return ret;
}

int cliser_client_recv(lua_State *L) {
   double t0 = cliser_profile_seconds();
   client_t *client = *(client_t **)lua_touserdata(L, 1);
   int ret;
   if (lua_type(L, 2) == LUA_TUSERDATA) {
      ret = sock_recv_userdata(L, 2, client->sock, &client->copy_context);
      if (ret == 0) {
         lua_pushvalue(L, 2);
         ret = 1;
      }
   } else {
      ret = sock_recv_msg(L, client->sock, client->recv_rb, &client->copy_context);
   }
   client->copy_context.rx.total_seconds += (cliser_profile_seconds() - t0);
   client->copy_context.rx.num_calls++;
   return ret;
}

int cliser_client_recv_async(lua_State *L) {
   double t0 = cliser_profile_seconds();
   client_t *client = *(client_t **)lua_touserdata(L, 1);
   int ret = sock_recv_msg_peek(L, client->sock, client->recv_rb);
   if (ret > 0) {
      ret = sock_recv_msg(L, client->sock, client->recv_rb, &client->copy_context);
   }
   client->copy_context.rx.total_seconds += (cliser_profile_seconds() - t0);
   client->copy_context.rx.num_calls++;
   return ret;
}

int cliser_net_stats_inner(lua_State *L, net_stats_t *net_stats) {
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
   lua_pushstring(L, "cudacliser_profile_seconds");
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

int cliser_net_stats(lua_State *L, copy_context_t *copy_context) {
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
   server_t *server = (server_t *)lua_touserdata(L, 1);
   return cliser_net_stats(L, &server->copy_context);
}

int cliser_client_net_stats(lua_State *L) {
   client_t *client = *(client_t **)lua_touserdata(L, 1);
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
