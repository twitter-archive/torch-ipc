local test = require 'regress'
local torch = require 'torch'
local ipc = require 'libipc'

local q = ipc.workqueue('test')

local echo = ipc.map(1, function()
   local ipc = require 'libipc'
   local q = ipc.workqueue('test')
   while true do
      local msg = q:read()
      if msg == nil then
         break
      elseif torch.typename(msg) then
         if msg:size(1) == 1 then
            msg:fill(13)
         end
      elseif type(msg) == 'table' and torch.typename(msg.f) then
         msg.f:fill(42)
      end
      q:write(msg)
   end
   q:close()
end)

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

   testFunctions = function()
      local f = function(a, b, c) return math.sqrt((a * a) + (b * b) + (c * c)) end
      q:write(f)
      local f1 = q:read()
      local n = f(1, 2, 3)
      local n1 = f1(1, 2, 3)
      test.mustBeTrue(n == n1, 'Function serialization failed '..n..' ~= '..n1)
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
}

q:write(nil)
echo:join()
