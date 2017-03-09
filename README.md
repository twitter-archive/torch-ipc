IPC
===

A set of [primitives](doc/index.md) that extend Torch for high performance
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

See the [AllReduce example](examples/allreduce.lua) to try it out.

SlurmTree
---------

An implementation of Tree that integrates with the [Slurm cluster manager](https://slurm.schedmd.com/).
It builds the communication tree by reading in the slurm variables which are
specified via [SBATCH directives](https://slurm.schedmd.com/sbatch.html)
(i.e. --nodes, --tasks-per-node, etc...) and minimizing the inter node
communication (when there are more than one tasks per node)

SlurmTree takes two optional arguments:
1. File path - For the file that coordinates the initial connection
of processes. The file location has to be shared across nodes.
(By default '~/.torch')
2. Tasks per gpu - Used to calculate the gpu id property (By default 1)

See the [slurm script](examples/allreduce.slurm) for an example of how to
start the processes.

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
local server = ipc.server('127.0.0.1', 8080)
-- Create a client and connect to the server
local client = ipc.client('127.0.0.1', 8080)
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
local t1,t2,t3 = ipc.map(3, function(fileNames, mapid)
   return torch.load(fileNames[mapid])
end, {'f1.t7', 'f2.t7', 'f3.t7'}):join()
```

Read more complete documentation on [ipc.map](doc/map.md).

Workqueue
---------

A simple single writer multiple reader command queue. Really useful when
combined with map to keep a bunch of background threads grabbing work
off the queue, processing it and then returning answers back to the main
thread.

```lua
-- See examples/workqueue.lua for the complete listing
-- Create a named workqueue
local q = ipc.workqueue('my queue')

-- Create 2 background workers that read from the named workqueue
local workers = ipc.map(2, function()
   -- This function is not a closure, it is a totally clean Lua environment
   local ipc = require 'libipc'
   -- Open the queue by name (the main thread already created it)
   local q = ipc.workqueue('my queue')
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

Read more complete documentation on [ipc.workqueue](doc/workqueue.md).

Channels
--------

Channels are a thread synchronization primitive based on
message-passing. Threads communicate via channels by writing messages
onto them and reading messages out of them, in FIFO order. There is no
restriction on which threads or how many threads can read or write
from a channel. This allows one to define concurrent workflows easily.

Channels can also be closed, which prevents further writes to it. Once
all items are read from a closed channel, that channel becomes drained
and nothing further can be read from it. DAGs of computation made up
of channels can be shut down via cascading closing/draining of
channels.

The
[producer-consumer example](doc/channel.md/#producer-consumer-example)
shows a group of producer threads and a group of consumer threads
being set up to communicate via a channel. The main thread tears the
entire setup down by closing the channel.

The
[local model parallelism for forward inference example](examples/model-parallelism.lua) shows
how to set up a `nn.Sequential`-based model so that each of its
submodules can execute forward inference in parallel.

The full documentation can be found at [ipc.channel](doc/channel.md).

Examples
--------

Simple scripts you can run locally can be found [here](examples/).
See the unit tests for a ton more detailed examples.

Documentation
-------------

Full API documentation can be found [here](doc/index.md).

License
-------

Licensed under the Apache License, Version 2.0.
[See LICENSE file](LICENSE).
