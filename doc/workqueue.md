# ipc.workqueue #

Creating an [ipc.workqueue](workqueue.md) allows a thread to communicate with a set of
worker threads created by [ipc.map](map.md). The queue is bidirectional between
an owner thread and the workers. All native Lua types can be quickly marshaled
by value across thread boundaries (a copy is made). 
Torch [Tensor](https://github.com/torch/torch7/blob/master/doc/tensor.md#tensor) 
and Storage objects are marshaled by reference (no copy is made).

The two main functions are __:write()__ and __:read()__. Their usage depends
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

### `writeup()`

Lua supports closures. These are functions with upvalues, i.e. non-global variables outside the scope of the function:

```lua
local upvalue = 1
local closure = function()
   return upvalue
end
```

By default __:write()__ doesn't support closures. Calling __:write(closure)__ will throw an error.
The reason for this is that we want to discourage users from serializing upvalues unless they absolutely need to.
However, we do provide __:writeup()__ for serializing closures/upvalues. 
So calling __:writeup(closure)__ will not fail.

In Lua, almost everything is an upvalues. 
For example, the `nn` variable is an upvalue in the following `closure()`:

```lua
local nn = require 'nn'

local heavyclosure = function(input)
   return nn.Linear:forward(input)
end
```

Calling __:writeup(heavyclosure)__ will attempt to serialize the entire `nn` package. 
To avoid this kind of mistake, we recommend calling require from inside the closure:

```lua
local lightfunction = function(input)
   local nn = require 'nn'
   return nn.Linear:forward(input)
end
```

Calling __:write(lightfunction)__ will be much more efficient.

As a final note on __:writeup()__, for the powerusers out there, note that we do not serialize the `_ENV` upvalue of closures.
Typically `_ENV = _G` in the writing thread, which would be too heavy to serialize. So instead we set the deserialized `_ENV` to the reading threads `_G`.

### multi-write

The __:write()__ and __:writeup()__ methods can be used to write multiple objects into the queue at once.
For example, `q:write(1, 2, 3)` is equivalent to `q:write(1);q:write(2);q:write(3)`.
As such, each argument passed to __:write()__  and __:writeup()__ will require their own `q:read()` to be read.


A more concrete example of combining [ipc.map](map.md) and [ipc.workqueue](workqueue.md)
can be found in [ipc.BackgroundTask](BackgroundTask.md)
