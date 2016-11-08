local test = require 'regress'
local ipc = require 'libipc'

test {
   testCount = function()
      local t = {2,3,4}
      local t2 = ipc.sharedtable(t)
      assert(#t == #t2)
   end,

   testMove = function()
      local t = {2,3,4}
      local t2 = ipc.sharedtable(t, true)
      assert(#t == 0)
      assert(#t2 == 3)
      for i=1,3 do
         assert(i+1 == t2[i])
      end
   end,

   testEmpty = function()
      local t = ipc.sharedtable()
      assert(#t == 0)
      local t = ipc.sharedtable(nil)
      assert(#t == 0)
   end,

   testRead = function()
      local t = {2,3,4}
      local t2 = ipc.sharedtable(t)

      for i = 1,#t do
         assert(t[i] == t2[i])
      end
   end,

   testPairs = function()
      local t = {2,3,4}
      local t2 = ipc.sharedtable(t)

      if _VERSION == 'Lua 5.1' then
         -- pairs not supported for userdata in 5.1
         return
      end
      for k, v in pairs(t2) do
         assert(t[k] == v)
      end
   end,

   testWrite = function()
      local t = {2,3,4}
      local t2 = ipc.sharedtable(t)

      for i = 1,#t do
         t2[i] = t2[i]+1
         assert(t[i]+1 == t2[i])
      end
   end,

   testWriteOnOther = function()
      local t = ipc.sharedtable({0})
      ipc.map(1, function(tab)
         for i=1,100 do
            tab[1] = tab[1]+1
         end
      end, t):join()
      assert(t[1] == 100)

      local t = ipc.sharedtable()
      ipc.map(100, function(tab, i)
         tab[i] = i
      end, t):join()
      assert(#t == 100)
      for i=1,100 do
         assert(t[i] == i)
      end
   end,

   testMultipleWrites = function()
      local t = ipc.sharedtable()
      local m = ipc.mutex()
      ipc.map(10, function(tab, mutex)
         for i=1,100 do
            mutex:lock()
            tab[i] = (tab[i] or 0) + 1
            mutex:unlock()
         end
      end, t, m):join()
      assert(#t == 100)
      for i=1,100 do
         assert(t[i] == 10)
      end
   end,

   testExternalType = function()
      local t = ipc.sharedtable()
      t.mutex = ipc.mutex()
      ipc.map(10, function(tab)
         for i=1,100 do
            tab.mutex:lock()
            tab[i] = (tab[i] or torch.LongTensor({0})) + 1
            tab.mutex:unlock()
         end
      end, t):join()
      for i=1,100 do
         assert(t[i][1] == 10)
      end
   end,

   testTableIncrement = function()
      local t = ipc.sharedtable()
      local t2 = ipc.sharedtable()
      local m = ipc.mutex()
      ipc.map(10, function(tab, tab2, mutex)
         local ipc = require 'libipc'
         for i=1,100 do
            mutex:lock()
            tab[i] = (tab[i] or ipc.sharedtable({0}))
            tab[i][1] = tab[i][1]+1
            tab2[i] = (tab2[i] or ipc.sharedtable({0}))
            tab2[i][1] = tab2[i][1]+1
            mutex:unlock()
         end
      end, t, t2, m):join()
      assert(#t == 100)
      assert(#t2 == 100)
      for i=1,100 do
         assert(t[i][1] == 10)
         assert(t2[i][1] == 10)
      end
   end,

   testTableExpand = function()
      local t = ipc.sharedtable()
      local t2 = ipc.sharedtable()
      local m = ipc.mutex()
      ipc.map(10, function(tab, tab2, mutex)
         local ipc = require 'libipc'
         for i=1,100 do
            mutex:lock()
            tab[i] = (tab[i] or ipc.sharedtable())
            local l = #tab[i]+1
            tab[i][l] = l
            tab2[i] = (tab2[i] or ipc.sharedtable())
            local l = #tab2[i]+1
            tab2[i][l] = l
            mutex:unlock()
         end
      end, t, t2, m):join()
      assert(#t == 100)
      assert(#t2 == 100)
      for i=1,100 do
         assert(#t[i] == 10)
         assert(#t2[i] == 10)
         for j=1,10 do
            assert(t[i][j] == j)
            assert(t2[i][j] == j)
         end
      end
   end,

   testSize = function()
      local t = ipc.sharedtable()
      local size1 = ipc.sharedtable_size(t)
      local t2 = {}
      for i=1,1000 do
         t2[i] = i
      end
      for i=1,1000 do
         t[i] = t2
      end
      local size2 = ipc.sharedtable_size(t)
      assert(size2 > size1)
   end,
}
