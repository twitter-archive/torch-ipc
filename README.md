Parallel
========

A set of primitives that extend Torch for high performance
parallel computation across thread and process boundaries.

Tree
----

Implements an AllReduce style binary tree of connected processes
on top of Client-Server nodes. This enables AllReduce style operations
on Tensors across a set of machines. The Tree is topology aware
such that it creates an optimal binary tree of processes
where each link is the fastest possible communication means
available. Allows user code to use one abstraction for
parallel Tensor computation and get high performance without
coding specifically for the underlying topology.

```lua
-- Every node will end up with the sum of all nodes
tree.allReduce(grads, function(a, b)
   return a:add(b)
end)
```

Client-Server
-------------

A classic client-server implementation over TCP sockets, used for IPC.
Can transfer all Lua primitives as well as Torch Tensor and Storage
types across the network. Includes a very fast implementation for
sending CUDA Tensors between machines and an even faster implementation
for passing CUDA Tensors between GPUs on the same machine using
the PCI-E bus via CUDA IPC.

The implementation is not tied to any specific cluster or discovery
mechanism. All you need to do is ensure your nodes can reach each
other over TCP.

```lua
-- Create a server
local server = parallel.server('127.0.0.1', 8080)
-- Create a client and connect to the server
local client = parallel.client('127.0.0.1', 8080)
-- Say hello
client:send('hi')
-- Listen for any client to say something
local msg = server:recvAny()
assert(msg == 'hi')
-- Disconnect and shutdown
client:close()
server:close()
```

Map
---

A map function to spawn a set of worker threads and run a
computation in the background. This is very handy for doing
IO off of the main thread, as IO is usually blocked on a
file or socket descriptor.

```lua
-- See examples/map.lua for the complete listing
-- Load 3 files in parallel
local t1,t2,t3 = parallel.map(3, function(fileNames, mapid)
   return torch.load(fileNames[mapid])
end, {'f1.t7', 'f2.t7', 'f3.t7'}):join()
```

Workqueue
---------

A simple single writer multiple reader command queue. Really useful when
combined with map to keep a bunch of background threads grabbing work
off the queue, processing it and then returning answers back to the main
thread.

```lua
-- See examples/workqueue.lua for the complete listing
-- Create a named workqueue
local q = parallel.workqueue('my queue')

-- Create 2 background workers that read from the named workqueue
local workers = parallel.map(2, function()
   -- This function is not a closure, its a totally clean Lua environment
   local parallel = require 'libparallel'
   -- Open the queue by name (the main thread already created it)
   local q = parallel.workqueue('my queue')
   repeat
      -- Read the next file name off the workqueue
      local fileName = q:read()
      if fileName then
         -- Load the file and write its contents back into the workqueue
         q:write(torch.load(fileName))
      end
   until fileName == nil
end)

-- Write the file names into the workqueue
q:write('f1.t7')
q:write('f2.t7')
q:write('f3.t7')

-- Read back the 3 answers and print them
print(q:read())
print(q:read())
print(q:read())
```

Examples
--------

Simple scripts you can run locally can be found [here](examples/).
See the unit tests for a ton more detailed examples.

License
-------

Licensed under the Apache License, Version 2.0.
[See LICENSE file](LICENSE).
