local test = require 'regress'
local ipc = require 'libipc'

local name = 'test'
local q = ipc.workqueue(name)

local echo = ipc.map(1, function(name)
   local ipc = require 'libipc'
   local lib = {}
   local class = torch.class("lib.class", lib)
   local q = ipc.workqueue(name)
   while true do
      local msg = q:read()
      if torch.type(msg) == 'table' and msg.closure then
         q:writeup(msg[1])
      else
         if msg == nil then
            break
         elseif torch.type(msg) == 'lib.class' then
            msg.reply = true
         elseif torch.isTensor(msg) then
            if msg:size(1) == 1 then
               msg:fill(13)
            end
         elseif type(msg) == 'table' and torch.typename(msg.f) then
            msg.f:fill(42)
         end
         q:write(msg)
      end
   end
   q:close()
end, "test")

local function tableEq(t0, t1)
   for k,v in pairs(t0) do
      if type(v) == 'table' then
         local e,msg = tableEq(v, t1[k])
         if not e then
            return e,msg
         end
      elseif t1[k] ~= v then
         return false, "for key '"..k.."' "..v.." ~= "..t1[k]
      end
   end
   for k,_ in pairs(t1) do
      if t0[k] == nil then
         return false, "extra right hand side key '"..k.."'"
      end
   end
   return true, ""
end

test {
   testBooleans = function()
      local n = true
      q:write(n)
      local n1 = q:read()
      test.mustBeTrue(n == n1, 'Boolean serialization failed '..tostring(n)..' ~= '..tostring(n1))
      local n = false
      q:write(n)
      local n1 = q:read()
      test.mustBeTrue(n == n1, 'Boolean serialization failed '..tostring(n)..' ~= '..tostring(n1))
   end,

   testNumbers = function()
      local n = 42.13
      q:write(n)
      local n1 = q:read()
      test.mustBeTrue(n == n1, 'Number serialization failed '..n..' ~= '..n1)
   end,

   testStrings = function()
      local n = "hey man what's up with that code?"
      q:write(n)
      local n1 = q:read()
      test.mustBeTrue(n == n1, 'String serialization failed '..n..' ~= '..n1)
   end,

   testArrays = function()
      local n = { 1, 2, 73.86, 'hello', true, false, 'good bye', 42.13 }
      q:write(n)
      local n1 = q:read()
      local e, msg = tableEq(n, n1)
      test.mustBeTrue(e, 'Table serialization failed '..msg)
   end,

   testTables = function()
      local n = {
         k0 = true,
         k1 = 23.45,
         k2 = "hey",
         k3 = { 1, 2, "yo" },
         k4 = { a = 1 },
         k5 = { a = { b = 2 } },
      }
      q:write(n)
      local n1 = q:read()
      local e, msg = tableEq(n, n1)
      test.mustBeTrue(e, 'Table serialization failed '..msg)
   end,

   testMetaTables = function()
      -- local module class
      local lib = {}
      local class = torch.class('lib.class', lib)
      local cmd = lib.class()
      q:write(cmd)
      local cmd1 = q:read()
      test.mustBeTrue(torch.typename(cmd) == torch.typename(cmd1), "local Metatable serialization failed")
      test.mustBeTrue(cmd1.reply, "local Metatable table serialize fail")
      -- global module class
      local cmd = torch.CmdLine()
      cmd.id = 1234
      cmd.cmd = torch.CmdLine()
      q:write(cmd)
      local cmd1 = q:read()
      test.mustBeTrue(torch.typename(cmd) == torch.typename(cmd1), "global Metatable serialization failed")
      test.mustBeTrue(torch.typename(cmd.cmd) == torch.typename(cmd1.cmd), "global Metatable nested serialization failed")
      test.mustBeTrue(cmd.id == 1234, "global Metatable table serialize fail")
      local e, msg = tableEq(cmd, cmd1)
      test.mustBeTrue(e, 'Table serialization failed '..msg)
   end,

   testFunctions = function()
      local f = function(a, b, c) return math.sqrt((a * a) + (b * b) + (c * c)) end
      q:write(f)
      local f1 = q:read()
      local n = f(1, 2, 3)
      local n1 = f1(1, 2, 3)
      test.mustBeTrue(n == n1, 'Function serialization failed '..n..' ~= '..n1)
   end,

   testClosures = function()
      if f3rtwertwert534 ~= nil then
         return
      end
      -- global function with an unlikely name
      f3rtwertwert534 = function() return 534 end
      local bias1, bias2, bias3 = 0, 1, 2
      local f0 = function() return f3rtwertwert534() + bias3 end
      local f = function(a, b, c) return f0() + bias2 + bias1 + math.sqrt((a * a) + (b * b) + (c * c)) end
      q:writeup({f,closure=true}) -- writeup
      local f1 = q:read()
      local n = f(1, 2, 3)
      local n1 = f1(1, 2, 3)
      test.mustBeTrue(n == n1, 'Function serialization failed '..n..' ~= '..n1)
      f3rtwertwert534 = nil
   end,

   testTensors = function()
      local f = torch.randn(10)
      q:write(f)
      local f1 = q:read()
      for i = 1,10 do
         test.mustBeTrue(f[i] == f1[i], 'Tensor serialization failed '..f[i]..' ~= '..f1[i])
      end
   end,

   testTensorsTwoWay = function()
      local f = torch.FloatTensor(1)
      f:fill(0)
      q:write(f)
      local f1 = q:read()
      test.mustBeTrue(f1[1] == 13, 'Tensor serialization failed '..f1[1]..' ~= 13')
   end,

   testTensorsInTable = function()
      local t = {
         f = torch.FloatTensor(1)
      }
      t.f:fill(0)
      q:write(t)
      local t1 = q:read()
      test.mustBeTrue(t1.f[1] == 42, 'Tensor serialization failed '..t1.f[1]..' ~= 42')
   end,

   testDrain = function()
      for i = 1,13 do
         q:write(i)
      end
      q:drain()
      for i = 1,13 do
         local r = q:read(true)
         test.mustBeTrue(r == i, 'Expected '..r..' to be '..i..' after drain')
      end
      local f = q:read(true)
      test.mustBeTrue(f == nil, 'Expected to read nil after draining')
   end,

   testMultiWrite = function()
      local a = { }
      for i = 1,13 do
         a[i] = i
      end
      q:write((unpack or table.unpack)(a))
      for i = 1,13 do
         test.mustBeTrue(i == q:read())
      end
   end,

   testDualStalls = function()
      local sq = ipc.workqueue('ds', 16)
      local m = ipc.map(1, function()
         local ipc = require 'libipc'
         local sq = ipc.workqueue('ds')
         local count = 0
         while true do
            local n = sq:read()
            if n == nil then
               break
            end
            sq:write(n)
            count = count + n
         end
         return count
      end)
      local expected = 0
      for i = 1,100 do
         sq:write(i)
         expected = expected + i
      end
      sq:write(nil)
      local final = m:join()
      assert(final == expected)
   end,

   testAnon = function()
      local q = ipc.workqueue()
      local m = ipc.mutex()
      local w = ipc.map(1, function(q, m)
         local data = q:read()
         q:write(data+1)
         q:read()
         m:barrier(2)
         q:write(true)
      end, q, m)

      q:write(0)
      assert(q:read() == 1)
      q:write(false)
      q:close()
      q = nil
      m:barrier(2)
      w:join()
   end,

   testCheckCreator = function()
      local q, creator = ipc.workqueue('test name')
      assert(creator == 1)
      local w = ipc.map(1, function()
         local ipc = require 'libipc'
         local q, creator = ipc.workqueue('test name')
         q:write(creator)
      end)
      assert(q:read() == 0)
      w:join()
   end,
}

q:write(nil)
echo:join()
