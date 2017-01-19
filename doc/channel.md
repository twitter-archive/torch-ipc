# ipc.channel #
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

``` lua
local c = ipc.channel([])
```

The constructor does not take any arguments. However, this will most
likely change in the near-future as additional functionality is added.

The following methods are defined on the channel.
* __:write()__
* __:read()__
* __:num_items()__
* __:close()__
* __:closed()__
* __:drained()__

## Lifecycle of channels
Channels can be in one of three states: `ipc.channel.OPEN`,
`ipc.channel.CLOSED` or `ipc.channel.DRAINED`. A newly-created channel
is open and will accept reads and writes. A channel can be closed
using the __:close()__ method. A closed channel will no longer accept
writes, but any items that remain on the channel can be read. Once all
of the items on a closed channel are read, that channel becomes
drained. Empty channels that are closed will go into the drained state
immediately.

The state of the channel is returned as a `status` return variable in
calls to __:write()__ and __:read()__. The state of the channel can
also be queried using the __:closed()__ and __:drained()__ methods.

## Reading and writing from channels
Any thread can write values into a channel and any thread can read
those values out of the channel. Writes onto an open channel should
always succeed, assuming that no errors occurred. Reads on an empty
and non-drained channel can either cause the thread to block (for
blocking reads) or return nil (for non-blocking reads). Reads on a
drained channel return immediately with the `ipc.channel.DRAINED`
status.

### Producer-consumer example
The following example illustrates using a channel to send items from
one group of threads to another. A group of producer threads and a
group of consumer threads are set up to communicate via a channel. The
main thread tears the entire setup down by closing the channel.

``` lua
local ipc = require 'libipc'
local c = ipc.channel() -- create channel

-- Spawn producer threads that write items to channel and checks the
-- returned status. If the status is not ipc.channel.OPEN, then the
-- channel has been closed and the producers should terminate.
local nproducers = 3
local producers = ipc.map(nproducers, function(c, tid)
    local ipc = require 'libipc'
    local sys = require 'sys'
    while true do
        local x = {tid, math.floor(torch.random(10))} -- generate item
        local status = c:write(x) -- write item onto channel
        if status ~= ipc.channel.OPEN then
            break -- channel is no longer open, so terminate
        end
        sys.sleep(0.1) -- don't generate too fast
    end
end, c)

-- Spawn consumer threads that read items from the channel and checks
-- the returned status. If the status is ipc.channel.DRAINED, then
-- there will not be any more items to read and the consumers should
-- terminate.
local consumers = ipc.map(1, function(c)
    local ipc = require 'libipc'
    local nonblocking = false
    while true do
        local status, item = c:read(nonblocking) -- read item from channel
        if status == ipc.channel.DRAINED then
            break -- channel has been drained, so terminate
        else
            print('tid: '..item[1]..' r: '..item[2]) -- do the thing
        end
    end
end, c)

-- It is possible to write to the channel from any thread, including
-- this one.
c:write({0, 'from main thread'})

sys.sleep(5) -- wait 5 secs so producers and consumers can run

-- Close the channel so producers and consumers will terminate.
c:close()
producers:join()
consumers:join()
assert(c:num_items() == 0)
```

### Multi-write example
The following example shows how to write multiple values into a
channel with a single __:write()__ call. __:write()__ can accept
multiple arguments. Each of these arguments is written to the channel.

``` lua
      local c = ipc.channel()
      local data = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,}
      local unpack = unpack or table.unpack
      local status = c:write(unpack(data))
      assert(status == ipc.channel.OPEN)
      assert(c:num_items() == 12, 'number of items in channel is incorrect')
      local nonblocking = false
      for i=1,#data do
         local status, readData = c:read(nonblocking)
         assert(status == ipc.channel.OPEN)
         assert(readData == i)
      end
```

## Closing and draining channels
Open channels can be closed, which changes its state from
`ipc.channel.OPEN` to `ipc.channel.CLOSED`. Closed channels will no
longer accept writes, but any items that are still on the channel can
be read.

Once all of the items on a closed channel are read, its state is
changed from `ipc.channel.CLOSED` to `ipc.channel.DRAINED`. Further
reads will return immediately with the drained status. An empty
channel which is closed will immediately be put into the drained
state.

Closing and draining channels can be used to signal to downstream
threads that there will no longer be anything to read on the
channel. These threads might close the channels that they are writing
to, resulting in a cascading teardown of channels further downstream.

See the [Producer-consumer example](### Producer-consumer example) to
see an example of threads checking statuses of __:read()__ and
__:write()__ calls to determine whether they should terminate and the
main thread closing a channel to teardown a collection of threads
operating on a channel.

## Behaviors not yet implemented
1. It is not possible to specify the max number of items that can be
   written to a channel. The channel just grows to allow the write,
   instead of blocking on write. Therefore, just
   like [ipc.workqueue](workqueue.md), there is no backpressure
   mechanism.
2. There is no select call to select between a number of channels.
3. The __:read()__ call, when called in non-blocking mode, does not
   allow one to distinguish between reading a nil from the channel and
   not reading an item at all.

## Examples
The
[ipc.channel unit tests](../test/test_channel.lua)
provide a rich set of examples, in addition to the
[local model parallelism for forward inference example](../examples/model-parallelism.lua).

Two examples are described in detail here.

### Building workqueues with channels
The `channelsAsWorkQueue` unit test shows how to build a workqueue as
described in [ipc.workqueue](workqueue.md) using channels, while
allowing for more than one owner thread.

Multiple threads can write onto the channel that is used to send work
items to the workers. They can write onto this channel until it is
closed. Multiple workers read from the work item channel. Each worker
only knows how to read a work item from the channel, process it and
then write it into a results channel. Each worker terminates as soon
as it sees that either the work item channel has been drained or the
results channel has been closed.

### Local model parallelism for forward inference
The
[local model parallelism example](../examples/model-parallelism.lua)
shows how to set up a `nn.Sequential`-based model so that each of its
submodules can execute forward inference in parallel.

The code runs forward inference on the following model:
``` lua
[nn.Sequential {
  [input -> (1) -> (2) -> (3) -> (4) -> (5) -> (6) -> (7) -> output]
  (1): nn.TemporalConvolution(20 -> 10, 5)
  (2): nn.Tanh
  (3): nn.TemporalConvolution(10 -> 5, 5)
  (4): nn.Tanh
  (5): nn.TemporalConvolution(5 -> 1, 5)
  (6): nn.Tanh
  (7): nn.Max
}
```

The unit test first measures the time taken to perform forward
inference on a single thread. Then the unit test measures the time
taken to perform forward inference when the model is split and run in
parallel across multiple threads.

Each model is distributed across multiple threads as follows:

1. For i = `1` to `3`, submodules `2i-1` (`nn.TemporalConvolution`)
   and `2i` (`nn.Tanh`) are instantiated on thread `i`.
2. The `nn.Max` submodule is instantiated on thread `4`.

Each thread has an input channel and an output channel. The output
tensor of the previous submodule will become available on the input
channel. The thread reads this tensor from the input channel and then
executes the __:forward()__ call on its layer with the tensor as input
and writes the resulting output tensor onto its output channel.

The unit test shows that splitting up the model and running each part
of it in parallel on separate threads is faster across the generated
workload than running the entire model sequentially on a single
thread. However, multiple threads running their own copies of the
entire model (data-parallel) should be as fast as the model-parallel
version.

## Background on channels
The semantics of [ipc.channel](channel.md) is based on the following
resources:

1. https://gobyexample.com/channels
2. https://tour.golang.org/concurrency/2
3. https://github.com/clojure/core.async/blob/master/examples/walkthrough.clj

Channels are a simplification of the workqueues provided
by [ipc.workqueue](workqueue.md). A workqueue has two queues - one
that is used to send items to workers and the other that is used to
send back results to the single owner thread. A channel just has a
single queue that any thread can enqueue or dequeue from.
