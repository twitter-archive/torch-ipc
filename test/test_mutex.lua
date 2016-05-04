local test = require 'regress'
local ipc = require 'libipc'

test {
   testLockingAndBarrier = function()
      local mutex = ipc.mutex()
      local shared = torch.FloatTensor(10000)
      shared:fill(0)
      local m = ipc.map(3, function(mutex, shared, mapid)
         local ipc = require 'libipc'
         local sys = require 'sys'
         assert(shared[1] == 0)
         mutex:barrier(4)
         assert(shared[1] ~= 0)
         mutex:lock()
         for i = 1,shared:size(1) do
            shared[i] = mapid
            sys.sleep(math.random(1000)/1e8)
         end
         mutex:unlock()
      end, mutex, shared)
      sys.sleep(1)
      shared[1] = 1000
      mutex:barrier(4)
      m:join()
      local first = shared[1]
      for i = 2,shared:size(1) do
         assert(shared[i] == first)
      end
   end,
}
