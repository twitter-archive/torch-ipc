local test = require 'regress'
local ipc = require 'libipc'

test {
   testLockingAndBarrier = function()
      local beforeWriteMutex = ipc.mutex()
      local afterWriteMutex = ipc.mutex()
      local shared = torch.FloatTensor(10000)
      shared:fill(0)

      local m = ipc.map(3, function(beforeWriteMutex, afterWriteMutex, shared, mapid)
         local ipc = require 'libipc'
         local sys = require 'sys'

         assert(shared[1] == 0)
         beforeWriteMutex:barrier(4)

         afterWriteMutex:barrier(4)
         assert(shared[1] ~= 0)

         afterWriteMutex:lock()
         for i = 1,shared:size(1) do
            shared[i] = mapid
         end
         afterWriteMutex:unlock()
      end, beforeWriteMutex, afterWriteMutex, shared)

      -- `beforeWriteMutex:barrier(4)` guarantees `assert(shared[1] == 0)` to succeed:
      -- the assignment `shard[1] = 1000` won't happen until all 3 threads finish the above assert.
      beforeWriteMutex:barrier(4)

      shared[1] = 1000

      -- afterWriteMutex:barrier(4) guarantees `assert(shared[1] ~= 0)` to succeed:
      -- the assignment `shard[1] = 1000` is guaranteed to happen before the above asserts.
      afterWriteMutex:barrier(4)

      m:join()
      local first = shared[1]
      for i = 2,shared:size(1) do
         assert(shared[i] == first)
      end
   end,
}
