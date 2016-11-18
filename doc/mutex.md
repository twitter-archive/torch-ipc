# ipc.mutex #

```lua
local mutex = ipc.mutex()
```
A `mutex` is used to lock resources and create barriers.
It is used to coordinate and synchronize threads.

## Locking

For locking, consider the following example:

```lua
local mutex = ipc.mutex()
local shared = torch.FloatTensor(10)
shared:fill(0)
local m = ipc.map(3, function(mutex, shared, mapid)
   local ipc = require 'libipc'
   mutex:lock()
   shared:fill(mapid)
   mutex:unlock()
end, mutex, shared)
```

In the above example, the 3 worker threads uses the mutex to protect access to the `shared` tensor.
Only one thread can call `:lock()` at any given time.
The remaining threads will block on that call until the locking thread calls `:unlock()` on that same `mutex`.
The last thread to call `:lock()` will by fill the `shared` tensor with its `mapid`, i.e. the id of the thread.

## Barrier

Barriers are used to synchronize threads.
A call to `mutex:barrier(nThread)` blocks until `nThread` have called `:barrier()`.
When the call returns, all threads are synchronized at this point in the code.

Consider the following example:

```lua
local shared = torch.FloatTensor(1)
shared:fill(0)
local m = ipc.map(3, function(mutex, shared, mapid)
   local ipc = require 'libipc'
   local sys = require 'sys'
   assert(shared[1] == 0)
   mutex:barrier(4) -- first barrier
   -- main thread updates shared[1]
   mutex:barrier(4) -- second barrier
   assert(shared[1] ~= 0)
end, mutex, shared)

assert(shared[1] == 0)
mutex:barrier(4) -- first barrier
shared[1] = 1000
mutex:barrier(4) -- second barrier
assert(shared[1] ~= 0)
m:join()
```

The example uses 3 worker threads. With the main thread, we have a total of 4 threads.
We all 4 threads to synchronize using a 2 barriers.
The first `:barrier()` is to make certain everyone executed `assert(shared[1] == 0)`
Afterwhich, the main thread updates `shared[1] = 1000`.
After the second `:barrier()` returns, everyone executes `assert(shared[1] ~= 1000)`.

