local BackgroundTaskPool = require 'ipc.BackgroundTaskPool'

local function BackgroundTask(func, ...)
   local pool = BackgroundTaskPool(1)
   pool.addTask(func, ...)
   local function getResult()
      return pool.getResult(1)
   end
   return {
      isDone = pool.isDone,
      getResult = getResult,
   }
end

return BackgroundTask
