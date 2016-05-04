local BackgroundTaskPool = require 'ipc.BackgroundTaskPool'

local function BackgroundTask(func, ...)
   local pool = BackgroundTaskPool(1, { closeOnLastTask = true })
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
