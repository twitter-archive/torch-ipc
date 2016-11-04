# ipc.BackgroundTaskPool #

BackgroundTaskPool combines [ipc.map](map.md) and [ipc.workqueue](workqueue.md) to implement
a very simple, pollable, way to run a set of arbitrary Lua functions as background tasks.
You construct a BackgroundTaskPool with the size of the thread pool
you would like to run your tasks on.

```lua
   local BackgroundTaskPool = require 'ipc.BackgroundTaskPool'
   local pool = BackgroundTaskPool(13) -- create a pool with 13 worker threads
```

Call addTask with a function and a set of arguments to add a task into the pool.

```lua
   for i = 1,42 do
      pool.addTask(function(i)
         return math.sqrt(i)
      end, i)
   end
```
You can optionally poll for the completion of all tasks or just one task.

```lua
   -- is the task with id 7 done?
   if pool.isDone(7) then
      print('the 7th is done!')
   end
   -- are all the tasks done?
   if pool.isDone() then
      print('all tasks are done!')
   end
```

When you want the result for a task, call getResult with the task id.
If the task function threw an error then the getResult call will throw that same
error.

```lua
   for i = 1,42 do
      assert(pool.getResult(i) == math.sqrt(i))
   end
```

Note that __ipc.BackgroundTaskPool__ will throw an error when attempting to serialize closures/upvalues.
However [ipc.workqueue](workqueue.md) provides __:writeup()__ for serializing closures/upvalues.
