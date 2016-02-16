local ipc = require 'libipc'

local function BackgroundTask(func, ...)
   assert(type(func) == 'function')
   local args = {...}
   local name = os.tmpname()
   local q = ipc.workqueue(name)
   local m = ipc.map(1, function(name)
      local ipc = require 'libipc'
      local q = ipc.workqueue(name)
      local task = q:read()
      q:write({task.func(unpack(task.args))})
   end, name)
   q:write({ func = func, args = args })
   local failed = false
   local results
   local function isDone()
      failed = true
      m:checkErrors()
      failed = false
      if results then
         return true
      end
      results = q:read(true)
      return results ~= nil
   end
   local function getResults()
      m:join()
      if not failed then
         if not results then
            results = q:read()
         end
         return unpack(results)
      end
   end
   return {
      isDone = isDone,
      getResults = getResults,
   }
end

return BackgroundTask
