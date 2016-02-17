#ifndef TH_GENERIC_FILE
#define TH_GENERIC_FILE "src/generic/cliser.c"
#else

#ifdef ELEMENT_SIZE
   #undef ELEMENT_SIZE
#endif
#ifdef TH_REAL_IS_BYTE
   #define ELEMENT_SIZE (sizeof(uint8_t))
#endif
#ifdef TH_REAL_IS_CHAR
   #define ELEMENT_SIZE (sizeof(int8_t))
#endif
#ifdef TH_REAL_IS_SHORT
   #define ELEMENT_SIZE (sizeof(int16_t))
#endif
#ifdef TH_REAL_IS_INT
   #define ELEMENT_SIZE (sizeof(int32_t))
#endif
#ifdef TH_REAL_IS_LONG
   #define ELEMENT_SIZE (sizeof(int64_t))
#endif
#ifdef TH_REAL_IS_FLOAT
   #define ELEMENT_SIZE (sizeof(float))
#endif
#ifdef TH_REAL_IS_DOUBLE
   #define ELEMENT_SIZE (sizeof(double))
#endif

#ifdef CLISER_IS_CUDA
#define CUDA_BLOCK_SIZE (512*1024)
#define CUDA_BLOCK_COUNT (CUDA_BLOCK_SIZE/ELEMENT_SIZE)
#define CUDA_REMOTE_PTRS (256)

static THCState *getCutorchState(lua_State* L) {
   lua_getglobal(L, "cutorch");
   lua_getfield(L, -1, "getState");
   lua_call(L, 0, 1);
   THCState *state = (THCState *)lua_touserdata(L, -1);
   lua_pop(L, 2);
   return state;
}

static void Lcliser_(init_copy_context)(copy_context_t *copy_context) {
   if (!copy_context->buf[0]) {
      THCudaCheck(cudaEventCreateWithFlags(&copy_context->event, cudaEventBlockingSync));
      copy_context->buf[0] = malloc(2 * CUDA_BLOCK_SIZE);
      copy_context->buf[1] = ((uint8_t *)copy_context->buf[0]) + CUDA_BLOCK_SIZE;
      copy_context->max_remote_ptrs = CUDA_REMOTE_PTRS;
      copy_context->remote_ptrs = calloc(copy_context->max_remote_ptrs, sizeof(remote_ptr_t));
   }
}

static int Lcliser_(write_contiguous)(lua_State *L, int sock, real *ptr, size_t count, copy_context_t *copy_context) {
   Lcliser_(init_copy_context)(copy_context);
   if (copy_context->use_fastpath) {
      double t0 = cliser_profile_seconds();
      remote_ptr_t remote_ptr;
      remote_ptr.ptr = ptr;
      remote_ptr.count = count;
      THCudaCheck(cudaIpcGetMemHandle(&remote_ptr.handle, ptr));
      THCudaCheck(cudaDeviceSynchronize());
      int ret = sock_send_raw(L, sock, &remote_ptr, sizeof(remote_ptr_t), copy_context);
      if (ret) return ret;
      int not_used;
      ret = sock_recv_raw(L, sock, &not_used, sizeof(not_used), copy_context);
      if (ret) return ret;
      copy_context->tx.cuda_ipc_seconds += (cliser_profile_seconds() - t0);
      copy_context->tx.cuda_ipc_bytes += count * ELEMENT_SIZE;
   } else {
      int which = 0;
      size_t last_cb = 0;
      while (count > 0) {
         if (last_cb) {
            double t0 = cliser_profile_seconds();
            THCudaCheck(cudaEventSynchronize(copy_context->event));
            copy_context->tx.cuda_sync_seconds += (cliser_profile_seconds() - t0);
         }
         size_t cb = (count > CUDA_BLOCK_COUNT) ? CUDA_BLOCK_COUNT : count;
         THCudaCheck(cudaMemcpyAsync(copy_context->buf[which], ptr, cb * ELEMENT_SIZE, cudaMemcpyDeviceToHost, 0));
         THCudaCheck(cudaEventRecord(copy_context->event, 0));
         if (last_cb) {
            int ret = sock_send_raw(L, sock, copy_context->buf[which ^ 1], last_cb * ELEMENT_SIZE, copy_context);
            if (ret) return ret;
         }
         which ^= 1;
         count -= cb;
         ptr += cb;
         last_cb = cb;
      }
      if (last_cb) {
         double t0 = cliser_profile_seconds();
         THCudaCheck(cudaEventSynchronize(copy_context->event));
         copy_context->tx.cuda_sync_seconds += (cliser_profile_seconds() - t0);
         return sock_send_raw(L, sock, copy_context->buf[which ^ 1], last_cb * ELEMENT_SIZE, copy_context);
      }
   }
   return 0;
}

static int Lcliser_(has_overlap)(real *p0, size_t c0, real *p1, size_t c1) {
   real *ep0 = p0 + c0 - 1;
   real *ep1 = p1 + c1 - 1;
   if (p0 >= p1 && p0 <= ep1) return 1;
   if (ep0 >= p1 && ep0 <= ep1) return 1;
   if (p1 >= p0 && p1 <= ep0) return 1;
   if (ep1 >= p0 && ep1 <= ep0) return 1;
   return 0;
}

static int Lcliser_(read_contiguous)(lua_State *L, int sock, real *ptr, size_t count, copy_context_t *copy_context) {
   Lcliser_(init_copy_context)(copy_context);
   if (copy_context->use_fastpath) {
      double t0 = cliser_profile_seconds();
      remote_ptr_t remote_ptr;
      int ret = sock_recv_raw(L, sock, &remote_ptr, sizeof(remote_ptr_t), copy_context);
      if (ret) return ret;
      size_t cb = 0;
      while (cb < copy_context->num_remote_ptrs) {
         if (memcmp(&remote_ptr.handle, &copy_context->remote_ptrs[cb].handle, sizeof(cudaIpcMemHandle_t)) == 0) {
            break;
         }
         if (copy_context->remote_ptrs[cb].sock == sock) {
            ret = Lcliser_(has_overlap)(remote_ptr.ptr, remote_ptr.count, copy_context->remote_ptrs[cb].ptr, copy_context->remote_ptrs[cb].count);
            if (ret) {
               fprintf(stderr, "WARN: torch-ipc: CUDA IPC evicting %p due to ptr reuse, performance will drastically suffer\n", remote_ptr.ptr);
               THCudaCheck(cudaIpcCloseMemHandle(copy_context->remote_ptrs[cb].dev_ptr));
               memmove(copy_context->remote_ptrs + cb, copy_context->remote_ptrs + cb + 1, (copy_context->num_remote_ptrs - (cb + 1)) * sizeof(remote_ptr_t));
               copy_context->num_remote_ptrs--;
               continue;
            }
         }
         cb++;
      }
      if (cb == copy_context->num_remote_ptrs) {
         if (copy_context->num_remote_ptrs == copy_context->max_remote_ptrs) {
            copy_context->max_remote_ptrs += CUDA_REMOTE_PTRS;
            copy_context->remote_ptrs = realloc(copy_context->remote_ptrs, copy_context->max_remote_ptrs * sizeof(remote_ptr_t));
            memset(&copy_context->remote_ptrs[copy_context->num_remote_ptrs], 0, CUDA_REMOTE_PTRS * sizeof(remote_ptr_t));
         }
         THCudaCheck(cudaIpcOpenMemHandle(&copy_context->remote_ptrs[cb].dev_ptr, remote_ptr.handle, cudaIpcMemLazyEnablePeerAccess));
         memcpy(&copy_context->remote_ptrs[cb].handle, &remote_ptr.handle, sizeof(cudaIpcMemHandle_t));
         copy_context->remote_ptrs[cb].ptr = remote_ptr.ptr;
         copy_context->remote_ptrs[cb].count = remote_ptr.count;
         copy_context->remote_ptrs[cb].sock = sock;
         copy_context->num_remote_ptrs++;
      }
      THCudaCheck(cudaMemcpy(ptr, copy_context->remote_ptrs[cb].dev_ptr, count * ELEMENT_SIZE, cudaMemcpyDefault));
      THCudaCheck(cudaDeviceSynchronize());
      int not_used = 1;
      ret = sock_send_raw(L, sock, &not_used, sizeof(not_used), copy_context);
      if (ret) return ret;
      copy_context->rx.cuda_ipc_seconds += (cliser_profile_seconds() - t0);
      copy_context->rx.cuda_ipc_bytes += count * ELEMENT_SIZE;
   } else {
      int which = 0;
      size_t last_cb = 0;
      if (count > 0) {
         THCudaCheck(cudaEventRecord(copy_context->event, 0));
      }
      while (count > 0) {
         if (last_cb) {
            double t0 = cliser_profile_seconds();
            THCudaCheck(cudaEventSynchronize(copy_context->event));
            copy_context->rx.cuda_sync_seconds += (cliser_profile_seconds() - t0);
            THCudaCheck(cudaMemcpyAsync(ptr, copy_context->buf[which ^ 1], last_cb * ELEMENT_SIZE, cudaMemcpyHostToDevice, 0));
            THCudaCheck(cudaEventRecord(copy_context->event, 0));
            ptr += last_cb;
         }
         size_t cb = (count > CUDA_BLOCK_COUNT) ? CUDA_BLOCK_COUNT : count;
         int ret = sock_recv_raw(L, sock, copy_context->buf[which], cb * ELEMENT_SIZE, copy_context);
         if (ret) return ret;
         which ^= 1;
         count -= cb;
         last_cb = cb;
      }
      if (last_cb) {
         double t0 = cliser_profile_seconds();
         THCudaCheck(cudaEventSynchronize(copy_context->event));
         copy_context->rx.cuda_sync_seconds += (cliser_profile_seconds() - t0);
         THCudaCheck(cudaMemcpy(ptr, copy_context->buf[which ^ 1], last_cb * ELEMENT_SIZE, cudaMemcpyHostToDevice));
      }
   }
   return 0;
}

#else

static void Lcliser_(init_copy_context)(copy_context_t *copy_context) {
   (void)copy_context;
}

static int Lcliser_(write_contiguous)(lua_State *L, int sock, real *ptr, size_t count, copy_context_t *copy_context) {
   return sock_send_raw(L, sock, ptr, count * ELEMENT_SIZE, copy_context);
}

static int Lcliser_(read_contiguous)(lua_State *L, int sock, real *ptr, size_t count, copy_context_t *copy_context) {
   return sock_recv_raw(L, sock, ptr, count * ELEMENT_SIZE, copy_context);
}

#endif

static int Lcliser_(storage_write)(lua_State *L) {
   THStorage *storage = luaT_checkudata(L, 1, torch_Storage);
   int sock = luaL_checkinteger(L, 2);
   copy_context_t *copy_context = (copy_context_t *)lua_touserdata(L, 3);
   long header[2];
   header[0] = ELEMENT_SIZE;
   header[1] = storage->size;
   int ret = sock_send_raw(L, sock, header, sizeof(header), copy_context);
   if (ret) return ret;
   return Lcliser_(write_contiguous)(L, sock, storage->data, storage->size, copy_context);
}

static int Lcliser_(storage_read)(lua_State *L) {
   THStorage *storage = luaT_checkudata(L, 1, torch_Storage);
   int sock = luaL_checkinteger(L, 2);
   copy_context_t *copy_context = (copy_context_t *)lua_touserdata(L, 3);
   long header[2];
   sock_recv_raw(L, sock, header, sizeof(header), copy_context);
   if (header[0] != ELEMENT_SIZE) return luaL_error(L, "local (%ld) and remote (%ld) storage ELEMENT_SIZE do not match", ELEMENT_SIZE, header[0]);
   if (header[1] != storage->size) return luaL_error(L, "local (%ld) and remote (%ld) storage size do not match", storage->size, header[1]);
   return Lcliser_(read_contiguous)(L, sock, storage->data, storage->size, copy_context);
}

static int Lcliser_(tensor_write_noncontiguous_rcsv)(lua_State *L, int sock, THTensor *tensor, int dim, int nDim, long nDimStride, real *ptr, copy_context_t *copy_context) {
   if (dim == nDim) {
      for (long i = 0; i < tensor->size[dim]; i++) {
         int ret = Lcliser_(write_contiguous)(L, sock, ptr, nDimStride, copy_context);
         if (ret) return ret;
         ptr += tensor->stride[dim];
      }
   } else {
      for (long i = 0; i < tensor->size[dim]; i++) {
         int ret = Lcliser_(tensor_write_noncontiguous_rcsv)(L, sock, tensor, dim + 1, nDim, nDimStride, ptr, copy_context);
         if (ret) return ret;
         ptr += tensor->stride[dim];
      }
   }
   return 0;
}

static int Lcliser_(tensor_read_noncontiguous_rcsv)(lua_State *L, int sock, THTensor *tensor, int dim, int nDim, long nDimStride, real *ptr, copy_context_t *copy_context) {
   if (dim == nDim) {
      for (long i = 0; i < tensor->size[dim]; i++) {
         int ret = Lcliser_(read_contiguous)(L, sock, ptr, nDimStride, copy_context);
         if (ret) return ret;
         ptr += tensor->stride[dim];
      }
   } else {
      for (long i = 0; i < tensor->size[dim]; i++) {
         int ret = Lcliser_(tensor_read_noncontiguous_rcsv)(L, sock, tensor, dim + 1, nDim, nDimStride, ptr, copy_context);
         if (ret) return ret;
         ptr += tensor->stride[dim];
      }
   }
   return 0;
}

static int Lcliser_(tensor_write)(lua_State *L) {
   THTensor *tensor = luaT_checkudata(L, 1, torch_Tensor);
   int sock = luaL_checkinteger(L, 2);
   copy_context_t *copy_context = (copy_context_t *)lua_touserdata(L, 3);
#ifdef CLISER_IS_CUDA
   THCState *thc = getCutorchState(L);
   long bc = (THTensor_(isContiguous)(thc, tensor));
   long ne = THTensor_(nElement)(thc, tensor);
#else
   long bc = (THTensor_(isContiguous)(tensor));
   long ne = THTensor_(nElement)(tensor);
#endif
   long i = sizeof(long) * ((2 * tensor->nDimension) + 1);
   long *header = alloca(i);
   header[0] = (bc & 0x1) | ((copy_context->use_fastpath << 1) & 0x2) | ((ELEMENT_SIZE << 4) & 0xF0);
   for (long j = 0; j < tensor->nDimension; j++) {
      header[(2 * j) + 1] = tensor->size[j];
      header[(2 * j) + 2] = tensor->stride[j];
   }
   int ret = sock_send_raw(L, sock, header, i, copy_context);
   if (ret) return ret;
   if (bc) {
      if (tensor->storage) {
         return Lcliser_(write_contiguous)(L, sock, tensor->storage->data + tensor->storageOffset, ne, copy_context);
      } else {
         return 0;
      }
   } else {
      if (tensor->nDimension < 2) return luaL_error(L, "not implemented");
      if (tensor->stride[tensor->nDimension - 1] != 1) return luaL_error(L, "not implemented");
      bc = tensor->size[tensor->nDimension - 1];
      i = tensor->nDimension - 2;
      while (i >= 0 && bc == tensor->stride[i]) {
         bc *= tensor->size[i];
         i--;
      }
      if (i < 0) return luaL_error(L, "unreachable");
      ret = Lcliser_(tensor_write_noncontiguous_rcsv)(L, sock, tensor, 0, i, bc, tensor->storage->data + tensor->storageOffset, copy_context);
      if (ret) return ret;
   }
   return 0;
}

static int Lcliser_(tensor_read)(lua_State *L) {
   THTensor *tensor = luaT_checkudata(L, 1, torch_Tensor);
   int sock = luaL_checkinteger(L, 2);
   copy_context_t *copy_context = (copy_context_t *)lua_touserdata(L, 3);
#ifdef CLISER_IS_CUDA
   THCState *thc = getCutorchState(L);
   long bc = (THTensor_(isContiguous)(thc, tensor));
   long ne = THTensor_(nElement)(thc, tensor);
#else
   long bc = (THTensor_(isContiguous)(tensor));
   long ne = THTensor_(nElement)(tensor);
#endif
   long i = sizeof(long) * ((2 * tensor->nDimension) + 1);
   long *header = alloca(i);
   sock_recv_raw(L, sock, header, i, copy_context);
   if ((header[0] & 0x1) != bc) return luaL_error(L, "local(%ld) and remote(%ld) isContiguous mismatch", bc, header[0] & 0xF);
   if (((header[0] & 0x2) >> 1) != copy_context->use_fastpath) return luaL_error(L, "local(%ld) and remote(%ld) use_fastpath mismatch", bc, header[0] & 0xF);
   if (((header[0] & 0xF0) >> 4) != ELEMENT_SIZE) return luaL_error(L, "local(%ld) and remote(%ld) ELEMENT_SIZE mismatch", ELEMENT_SIZE, ((header[0] & 0xF0) >> 4));
   for (i = 0; i < tensor->nDimension; i++) {
      if (header[(2 * i) + 1] != tensor->size[i]) return luaL_error(L, "local(%ld) and remote(%ld) size of dimension(%d) mismatch", tensor->size[i], header[(2 * i) + 1], i);
      if (header[(2 * i) + 2] != tensor->stride[i]) return luaL_error(L, "local(%ld) and remote(%ld) stride of dimension(%d) mismatch", tensor->size[i], header[(2 * i) + 2], i);
   }
   if (bc) {
      if (tensor->storage) {
         return Lcliser_(read_contiguous)(L, sock, tensor->storage->data + tensor->storageOffset, ne, copy_context);
      }
   } else {
      if (tensor->nDimension < 2) return luaL_error(L, "not implemented");
      if (tensor->stride[tensor->nDimension - 1] != 1) return luaL_error(L, "not implemented");
      bc = tensor->size[tensor->nDimension - 1];
      i = tensor->nDimension - 2;
      while (i >= 0 && bc == tensor->stride[i]) {
         bc *= tensor->size[i];
         i--;
      }
      if (i < 0) return luaL_error(L, "unreachable");
      Lcliser_(tensor_read_noncontiguous_rcsv)(L, sock, tensor, 0, i, bc, tensor->storage->data + tensor->storageOffset, copy_context);
   }
   return 0;
}

void Lcliser_(Init)(lua_State *L) {
   if (luaT_pushmetatable(L, torch_Storage)) {
      lua_pushcfunction(L, Lcliser_(storage_read));
      lua_setfield(L, -2, "_cliser_read");
      lua_pushcfunction(L, Lcliser_(storage_write));
      lua_setfield(L, -2, "_cliser_write");
      lua_pop(L, 1);
   }
   if (luaT_pushmetatable(L, torch_Tensor)) {
      lua_pushcfunction(L, Lcliser_(tensor_read));
      lua_setfield(L, -2, "_cliser_read");
      lua_pushcfunction(L, Lcliser_(tensor_write));
      lua_setfield(L, -2, "_cliser_write");
      lua_pop(L, 1);
   }
}

#endif
