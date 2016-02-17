# ipc.workqueue #

Creating an [ipc.workqueue](workqueue.md) allows a thread to communicate with a set of
worker threads created by [ipc.map](map.md). The queue is bidirectional between
an owner thread and the workers. All native Lua types can be quickly marshaled
by value across thread boundaries (a copy is made). Torch Tensor and Storage
objects are marshaled by reference (no copy is made).

The two main functions are __:write()__ and __:read(). Their usage depends
on the perspective of the caller. From the owner thread's perspective,
__:write()__ will put a *question* on the queue for one of the workers to process.
Whenever the owner thread would like get the *answer* it can call __:read()__.
From the perspective of the worker thread the functions are reversed.
A worker can call __:read()__ to get the next *question* off the queue to process and it
can call __:write()__ to return the answer to the owner thread.

```lua
   local ipc = require 'libipc'

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

The owner thread can also do non-blocking reads from the queue.
This is useful to poll for *answers* while doing other work.
Passing *true* into __:read(true)__ will check the queue for
an answer, if one is ready it will return it, else __:read(true)__
will return nil, indicating no *answer* is currently ready.

A more concrete example of combining [ipc.map](map.md) and [ipc.workqueue](workqueue.md)
can be found in [ipc.BackgroundTask](BackgroundTask.md)
