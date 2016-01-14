#include "luaT.h"

int cliser_server(lua_State *L);
int cliser_server_close(lua_State *L);
int cliser_server_clients(lua_State *L);
int cliser_server_tag(lua_State *L);
int cliser_server_id(lua_State *L);
int cliser_server_client_close(lua_State *L);
int cliser_server_broadcast(lua_State *L);
int cliser_server_recv_any(lua_State *L);
int cliser_server_send(lua_State *L);
int cliser_server_recv(lua_State *L);
int cliser_server_net_stats(lua_State *L);

int cliser_client(lua_State *L);
int cliser_client_close(lua_State *L);
int cliser_client_send(lua_State *L);
int cliser_client_recv(lua_State *L);
int cliser_client_recv_async(lua_State *L);
int cliser_client_retain(lua_State *L);
int cliser_client_metatablename(lua_State *L);
int cliser_client_net_stats(lua_State *L);

void Lcliser_CharInit(lua_State *L);
void Lcliser_ByteInit(lua_State *L);
void Lcliser_ShortInit(lua_State *L);
void Lcliser_IntInit(lua_State *L);
void Lcliser_LongInit(lua_State *L);
void Lcliser_FloatInit(lua_State *L);
void Lcliser_DoubleInit(lua_State *L);
#ifdef USE_CUDA
void Lcliser_CudaInit(lua_State *L);
#endif

struct copy_context_t;

int sock_send_raw(lua_State *L, int sock, void *ptr, size_t len, struct copy_context_t *copy_context);
int sock_recv_raw(lua_State *L, int sock, void *ptr, size_t len, struct copy_context_t *copy_context);
