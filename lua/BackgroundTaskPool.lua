local ipc = require 'libipc'

local function BackgroundTaskPool(poolSize)

   -- Keep track of some stuff
   local numTasks = 0
   local numResults = 0
   local successes = { }
   local failures = { }

   -- Create a shared queue with a random name
   local name = os.tmpname()
   local q = ipc.workqueue(name)

   -- Create a pool of workers
   local m = ipc.map(poolSize, function(name)
      local ipc = require 'libipc'
      local q = ipc.workqueue(name)
      while true do
         local task = q:read()
         if task then
            local unpack = unpack or table.unpack
            local ok,ret = pcall(function() return {task.func(unpack(task.args))} end)
            if ok then
               q:write({ id = task.id, success = ret })
            else
               q:write({ id = task.id, failure = ret })
            end
         else
            break
         end
      end
   end, name)

   -- Is this task already complete?
   local function hasResult(id)
      return id and (successes[id] or failures[id])
   end

   -- Check if one or all the tasks are finished
   local function isDone(id, shouldBlock)
      -- Get all the completed tasks off the queue
      while numResults < numTasks and not hasResult(id) do
         local result = q:read(shouldBlock ~= true)
         if result then
            if result.failure then
               failures[result.id] = result.failure
            else
               successes[result.id] = result.success
            end
            numResults = numResults + 1
         else
            -- No more results pending
            break
         end
      end
      -- Did we see results for one or all?
      return numResults == numTasks or hasResult(id)
   end

   -- Add a task to the queue
   local function addTask(func, ...)
      assert(type(func) == 'function')
      local args = {...}
      -- Keep the queue moving by reading some results
      isDone()
      -- Add the new task
      numTasks = numTasks + 1
      q:write({ id = numTasks, func = func, args = args })
      -- Return the task's id
      return numTasks
   end

   -- Get the result for one of the tasks
   local function getResult(id)
      -- Make sure the task is done
      isDone(id, true)
      -- If it is the last result then cleanup
      if numResults == numTasks and m then
         -- Shutdown everything down
         for _ = 1,poolSize do
            q:write(nil)
         end
         m:join()
         m = nil
      end
      -- Return the task result
      if successes[id] then
         local unpack = unpack or table.unpack
         return unpack(successes[id])
      elseif failures[id] then
         error(failures[id])
      end
   end

   return {
      addTask = addTask,
      isDone = isDone,
      getResult = getResult,
   }
end

return BackgroundTaskPool
