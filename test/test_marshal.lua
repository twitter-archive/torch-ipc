local test = require 'regress'
local ipc = require 'libipc'

test {
   testWriteRead = function()
      local m = ipc.marshal(23)
      test.mustBeTrue(torch.type(m) == 'userdata', "expecting userdata, got "..torch.type(m))
      local res = m:read()
      test.mustBeTrue(res == 23, 'expected 23, saw '..res)
      local res2 = m:read()
      test.mustBeTrue(res2 == 23, 'expected 23, saw '..res2)
      local res3 = m:read()
      test.mustBeTrue(res3 == 23, 'expected 23, saw '..res3)
   end,

   testMap = function()
      local m = ipc.marshal(23)
      local m, val, m2, val2 = ipc.map(2, function(m)
         local val = m:read()
         assert(val == 23, "Expecting 23")
         return m, val
      end, m):join()

      local res = m:read()
      test.mustBeTrue(res == 23, 'expected 23, saw '..res)
      local res = m:read()
      test.mustBeTrue(res == 23, 'expected 23, saw '..res)
   end,

   testWorkqueue = function()
      local m = ipc.marshal(43)
      local q = ipc.workqueue("marshal")
      local map = ipc.map(2, function()
         local ipc = require 'libipc'
         local q = ipc.workqueue("marshal")
         while true do
            local m = q:read()
            if m == nil then
               break
            end
            local res = m:read()
            assert(res == 43, "Expecting 43")
            q:write(m)
         end
      end)

      for i=1,5 do
         q:write(m)
      end
      for i=1,2 do
         q:write(nil)
      end

      map:join()

      for i=1,5 do
         local m2 = q:read()
         local res2 = m2:read()
         test.mustBeTrue(res2 == 43, 'expected 43, saw '..res2)
      end
   end,

   testTable = function()
      local obj = {1,2,3,v=4,g=5,t=function() return 6 end}
      local m = ipc.marshal(obj)

      local obj2 = m:read()
      test.mustBeTrue(torch.type(obj2) == 'table', 'expected tabe, saw '..torch.type(obj2))
      test.mustBeTrue(obj2[1] == 1 and obj2[2] == 2 and obj2[3] == 3 and obj2.v == 4 and obj2.g == 5 and obj2.t() == 6, 'error in obj')

      local obj2 = m:read()
      test.mustBeTrue(torch.type(obj2) == 'table', 'expected tabe, saw '..torch.type(obj2))
      test.mustBeTrue(obj2[1] == 1 and obj2[2] == 2 and obj2[3] == 3 and obj2.v == 4 and obj2.g == 5 and obj2.t() == 6, 'error in obj')

      local obj2 = m:read()
      test.mustBeTrue(torch.type(obj2) == 'table', 'expected tabe, saw '..torch.type(obj2))
      test.mustBeTrue(obj2[1] == 1 and obj2[2] == 2 and obj2[3] == 3 and obj2.v == 4 and obj2.g == 5 and obj2.t() == 6, 'error in obj')
   end,

   testClosure = function()
      local upval = 6
      local obj = {1,2,3,v=4,g=5,t=function() return upval end}
      local success, m = pcall(function() return ipc.marshal(obj) end)
      test.mustBeTrue(not success, "Expecting upval marshalling error")

      local m = ipc.marshal(obj, true)

      local obj2 = m:read()
      test.mustBeTrue(torch.type(obj2) == 'table', 'expected tabe, saw '..torch.type(obj2))
      test.mustBeTrue(obj2[1] == 1 and obj2[2] == 2 and obj2[3] == 3 and obj2.v == 4 and obj2.g == 5 and obj2.t() == 6, 'error in obj')

      local obj2 = m:read()
      test.mustBeTrue(torch.type(obj2) == 'table', 'expected tabe, saw '..torch.type(obj2))
      test.mustBeTrue(obj2[1] == 1 and obj2[2] == 2 and obj2[3] == 3 and obj2.v == 4 and obj2.g == 5 and obj2.t() == 6, 'error in obj')

      local obj2 = m:read()
      test.mustBeTrue(torch.type(obj2) == 'table', 'expected tabe, saw '..torch.type(obj2))
      test.mustBeTrue(obj2[1] == 1 and obj2[2] == 2 and obj2[3] == 3 and obj2.v == 4 and obj2.g == 5 and obj2.t() == 6, 'error in obj')
   end,

   testSize= function()
      local m = ipc.marshal(23, 10, 10)
      test.mustBeTrue(torch.type(m) == 'userdata', "expecting userdata, got "..torch.type(m))
      local res = m:read()
      test.mustBeTrue(res == 23, 'expected 23, saw '..res)
      local res2 = m:read()
      test.mustBeTrue(res2 == 23, 'expected 23, saw '..res2)
      local res3 = m:read()
      test.mustBeTrue(res3 == 23, 'expected 23, saw '..res3)
   end,
}


