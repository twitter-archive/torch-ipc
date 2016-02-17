local test = require 'regress'
local sys = require 'sys'
local BackgroundTask = require 'ipc.BackgroundTask'

test {
   testSimple = function()
      local task = BackgroundTask(function(t)
         local sys = require 'sys'
         sys.sleep(t)
         return 'done!', 42, true
      end, 0.7)
      local s,n,b = task.getResults()
      assert(s == 'done!')
      assert(n == 42)
      assert(b == true)
   end,

   testPolling = function()
      local task = BackgroundTask(function(t)
         local sys = require 'sys'
         sys.sleep(t)
         return 'done!', 42, true
      end, 1.3)
      local x = 0
      while not task.isDone() do
         x = x + 1
         sys.sleep(0.1)
      end
      assert(x > 9 and x < 16)
      local s,n,b = task.getResults()
      assert(s == 'done!')
      assert(n == 42)
      assert(b == true)
   end,

   testNoReturns = function()
      local task = BackgroundTask(function(t)
         local sys = require 'sys'
         sys.sleep(t)
      end, 0.4)
      local y = task.getResults()
      assert(y == nil)
   end,

   testError = function()
      local task = BackgroundTask(function(t)
         local sys = require 'sys'
         sys.sleep(t)
         error('die')
         return 43
      end, 0.4)
      assert(pcall(function() return task.getResults() end) == false)
   end,

   testErrorPolling = function()
      local task = BackgroundTask(function(t)
         local sys = require 'sys'
         sys.sleep(t)
         error('die')
         return 43
      end, 0.4)
      sys.sleep(0.6)
      assert(pcall(function() return task.isDone() end) == false)
      assert(task.getResults() == nil)
   end,
}

