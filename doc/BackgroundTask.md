# ipc.BackgroundTask #

BackgroundTask combines [ipc.map](map.md) and [ipc.workqueue](workqueue.md) to implement
a very simple, pollable, way to run a Lua function as background task.
Internally it is a [ipc.BackgroundTaskPool](BackgroundTaskPool.md) with a pool size of 1
and a single task added to the pool.

You construct a BackgroundTask with a function and a set of arguments
to be passed to the function. At any point you can check if the task
is done running with __isDone__ or to get all the return values using
__getResult__.

BackgroundTask is very useful for doing periodic backups during long
running jobs. For example, saving out your Torch model every so often
during a training run that takes many hours or days. It is quite common
for data centers to only provide ephemeral storage on compute nodes, that
means if a job dies its results are lost forever. Saving to something
more persistent like S3 or HDFS is slow, so we can use a BackgroundTask
to hide that from the main training loop. In this example below we
use curl to upload the saved file to a hypothetical website.

```lua
   local BackgroundTask = require 'ipc.BackgroundTask'

   -- Make a random "model" to save
   local model = { x = torch.randn(13, 42), y = math.random() }

   -- Write it out to a temp file
   -- This is quick and the easiest way to marshal
   -- the model into a background task
   local tempFilename = os.tmpname()
   torch.save(tempFilename, model)
   print('temporarily saved model to '..tempFilename)

   -- Create a background task to upload the model
   local background = BackgroundTask(function(filename, url)
      -- This function is a clean Lua environment
      local ipc = require 'libipc'
      local sys = require 'sys'
      -- Time the upload
      sys.tic()
      -- Spawn curl to upload the file
      local p = ipc.spawn({
         file = 'curl'
         args = {
            '-i',
            '-F name=saveme',
            '-F filedata=@'..filename,
            url,
         }
      })
      -- Wait on the upload, return curl's exit code and how long it took
      return p:wait(), sys.toc()
   end, tempFilename, "http://yourserver.com/here")

   -- Do something while the save happens
   while not background.isDone() do
      print('still saving...')
      sys.sleep(1)
   end

   -- Its done so check check the results
   local ret,t = background.getResults()
   print('curl returned  '..ret..' in '..t..' seconds')
```

Note that __ipc.BackgroundTask__ will throw an error when attempting to serialize closures/upvalues.
However [ipc.workqueue](workqueue.md) provides __:writeup()__ for serializing closures/upvalues.

