# ipc.map #

Maps a Lua function onto a set of threads. The function is run in
an entirely new and clean Lua environment. No upvalues of the function
are marshaled into the thread. Torch Tensor and Storage objects are
shared across the thread boundary, no copies are made. That said,
we also do not provide thread safe access to those Tensors. It is
up to the programmer to implement their own lockless system.

You can pass as many arguments as you wish to the function and
the function can return as many values as it wants.

```lua
   local ipc = require 'libipc'
   local m = ipc.map(2, function(a, b, c)
      return math.sqrt(a*a + b*b + c*c), "hi"
   end, 1, 2, 3)
   local p1,s1,p2,s2 = m:join()
   print(p1) -- will print 3.7416573867739
   print(s1) -- will print "hi"
   print(p2) -- will print 3.7416573867739
   print(s2) -- will print "hi"
```

Similar to posix threads, when you want to wait for all the child
threads to end and get their return values you must call __:join()__.
If any child threads had errors these will bubble up via a call
to __:join()__. You can wrap this in a pcall if you think the
error is recoverable (generally, it is not).

At any point the parent thread can check if any of the child
threads have died. The __:checkErrors()__ function will bubble
up an errors. You can wrap this in a pcall if you think the
error is recoverable (generally, it is not).

Most often [ipc.map](map.md) is combined with an [ipc.workqueue](workqueue.md)
in order to distribute work across the threads.
A more concrete example of combining [ipc.map](map.md) and [ipc.workqueue](workqueue.md)
can be found in [ipc.BackgroundTask](BackgroundTask.md)

