local test = require 'regress'
local sys = require 'sys'
local BackgroundTaskPool = require 'ipc.BackgroundTaskPool'

test {
   testSimple = function()
      local pool = BackgroundTaskPool(20, { closeOnLastTask = true })
      for _ = 1,1000 do
         pool.addTask(function(t)
            local sys = require 'sys'
            sys.sleep(t)
            return math.random()
         end, math.random(1, 1000) / 1000)
      end
      for i = 1,1000 do
         assert(type(pool.getResult(i)) == 'number')
      end
   end,

   testPolling = function()
      local pool = BackgroundTaskPool(13, { closeOnLastTask = true })
      for _ = 1,42 do
         pool.addTask(function(t)
            local sys = require 'sys'
            sys.sleep(t)
            return math.random()
         end, math.random(1, 1000) / 1000)
      end
      local x = 0
      while not pool.isDone() do
         x = x + 1
         sys.sleep(0.1)
      end
      assert(x > 10 and x < 40, x)
      for i = 1,42 do
         assert(type(pool.getResult(i)) == 'number')
      end
   end,

   testError = function()
      local pool = BackgroundTaskPool(7, { closeOnLastTask = true })
      for i = 1,42 do
         pool.addTask(function(t, i)
            local sys = require 'sys'
            sys.sleep(t)
            if i % 8 == 1 then
               error('die')
            end
            return math.random()
         end, math.random(1, 1000) / 1000, i)
      end
      for i = 1,42 do
         if i % 8 == 1 then
            assert(pcall(function() return pool.getResult(i) end) == false)
         else
            assert(type(pool.getResult(i)) == 'number')
         end
      end
   end,

   testErrorPolling = function()
      local pool = BackgroundTaskPool(13, { closeOnLastTask = true })
      for i = 1,42 do
         pool.addTask(function(t, i)
            local sys = require 'sys'
            sys.sleep(t)
            if i % 27 == 1 then
               error('die')
            end
            return math.random()
         end, math.random(1, 1000) / 1000, i)
      end
      local x = 0
      while not pool.isDone() do
         x = x + 1
         sys.sleep(0.1)
      end
      assert(x > 10 and x < 40, x)
      for i = 1,42 do
         if i % 27 == 1 then
            assert(pcall(function() return pool.getResult(i) end) == false)
         else
            assert(type(pool.getResult(i)) == 'number')
         end
      end
   end,
}
