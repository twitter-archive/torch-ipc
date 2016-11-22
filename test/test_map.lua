local test = require 'regress'
local ipc = require 'libipc'

test {
   testSingle = function()
      local x = ipc.map(1, function(y) return y end, 42):join()
      test.mustBeTrue(x == 42, 'expected 42, saw '..x)
   end,

   testMultiple = function()
      local x,y,z = ipc.map(1, function(a, b, c) return a,b,c end, "hi", 42, {k=2}):join()
      test.mustBeTrue(x == 'hi', 'expected "hi", saw '..x)
      test.mustBeTrue(y == 42, 'expected 42, saw '..y)
      test.mustBeTrue(z.k == 2, 'expected 2')
   end,

   testSingleWithN = function()
      local x = {ipc.map(3, function(y) return y end, 42):join()}
      test.mustBeTrue(#x == 3, 'expected 3, saw '..#x)
      test.mustBeTrue(x[1] == 42, 'expected 42, saw '..x[1])
      test.mustBeTrue(x[2] == 42, 'expected 42, saw '..x[2])
      test.mustBeTrue(x[3] == 42, 'expected 42, saw '..x[3])
   end,

   testMultipleWithN = function()
      local x1,y1,z1,x2,y2,z2 = ipc.map(2, function(a, b, c) return a,b,c end, "hi", 42, {k=2}):join()
      test.mustBeTrue(x1 == 'hi', 'expected "hi", saw '..x1)
      test.mustBeTrue(y1 == 42, 'expected 42, saw '..y1)
      test.mustBeTrue(z1.k == 2, 'expected 2')
      test.mustBeTrue(x2 == 'hi', 'expected "hi", saw '..x2)
      test.mustBeTrue(y2 == 42, 'expected 42, saw '..y2)
      test.mustBeTrue(z2.k == 2, 'expected 2')
   end,

   testNil = function()
      local x, y = ipc.map(1, function(x, y) return nil, y end, nil, 42):join()
      test.mustBeTrue(x == nil, 'expected nil, saw '..(x or 'nil'))
      test.mustBeTrue(y == 42, 'expected 42, saw '..y)
   end,

   testErrors = function()
      local ok, msg = pcall(function() ipc.map(1, function() error('boom') end):join() end)
      test.mustBeTrue(ok == false, 'expected the join to fail')
      test.mustBeTrue(type(msg) == 'string', 'expected the error message to be a string')
   end,

   testMoreErrors = function()
      local ok, msg = pcall(function() ipc.map(2, function(idx)
         if idx == 1 then
            error('boom')
         else
            return 42
         end
      end):join() end)
      test.mustBeTrue(ok == false, 'expected the join to fail')
      test.mustBeTrue(type(msg) == 'string', 'expected the error message to be a string')
   end,

   testUpvalueError = function()
      Global4523463456345 = 32 -- set a global
      local function envIsSet()
         return Global4523463456345 == nil and 56
      end
      local name, value = debug.getupvalue(envIsSet, 1)
      local res = ipc.map(1, envIsSet):join()
      test.mustBeTrue(res == 56)

      local function envIsSet2()
         Global4523463456345 = 46
         local function envIsSet()
            return Global4523463456345
         end
         return envIsSet()
      end
      local res = ipc.map(1, envIsSet2):join()
      test.mustBeTrue(res == 46)

      Global4523463456345 = nil

      local i,j=1,2
      local function closure()
         return i+j+1
      end
      local ok, msg = pcall(function() ipc.map(1, closure):join() end)
      test.mustBeTrue(ok == false, 'expected the map to fail')
      test.mustBeTrue(type(msg) == 'string', 'expected the error message to be a string')
   end,

   testCheckErrors = function()
      local m = ipc.map(2, function(idx)
         local sys = require 'sys'
         if idx == 1 then
            error('boom')
         else
            sys.sleep(2)
            return 42
         end
      end)
      sys.sleep(0.2)
      local ok,msg = pcall(function() return m:checkErrors() end)
      test.mustBeTrue(ok == false, 'expected the checkErrors to fail')
      test.mustBeTrue(type(msg) == 'string', 'expected the error message to be a string')
      local ok,msg = pcall(function() return m:join() end)
      test.mustBeTrue(ok == true, 'expected the join to pass')
      test.mustBeTrue(type(msg) == 'number' and msg == 42, 'expected the result to be a number 42')
   end,

   testLuaCheckStack = function()
      -- OSX has low file ulimits that cause the require system to die
      local n = sys.uname() == 'macos' and 50 or 1000
      local ret = { ipc.map(n, function() return 1 end):join() }
      test.mustBeTrue(#ret == n, 'expected '..n..' elements, saw '..#ret)
   end,

   testLastArg = function()
      local ret = { ipc.map(4, function(s, id) return id end, "hi"):join() }
      test.mustBeTrue(#ret == 4, 'expected 4 elements, saw '..#ret)
      test.mustBeTrue(ret[1] == 1, 'expected 1 at 1')
      test.mustBeTrue(ret[2] == 2, 'expected 2 at 2')
      test.mustBeTrue(ret[3] == 3, 'expected 3 at 3')
      test.mustBeTrue(ret[4] == 4, 'expected 4 at 4')
   end,

   testBigArguments = function()
      local n = 1000
      local data = { }
      for i = 1,n do
        data[i] = i
      end
      ipc.map(3, function(data, mapid)
        return data[mapid]
      end, data):join()
   end,

   testBigReturns = function()
      ipc.map(3, function()
         local n = 1000
         local data = { }
         for i = 1,n do
           data[i] = i
         end
         return data
      end):join()
   end,

   testFunctionMapId = function()
      Global4523463456345 = 1
      Globalsadg234523 = 2

      local function run(opt, mapid)
         assert(torch.type(opt) == 'table')
         assert(torch.type(mapid) == 'number')
         -- do something with globals
         return (Global4523463456345 or 3) + (Globalsadg234523 or 5)
      end

      local largetable = {}
      for i=1,1000 do
         largetable["asdfasdfa"..i] = i
      end
      local mutex = ipc.mutex()
      ipc.map(3, run, {
         asts = largetable,
         verbose = 3232,
         mode = 'string',
         mutex = mutex,
         barrier = true,
         partitions = torch.Tensor(10),
         reportEvery = 200,
      }):join()

      Global4523463456345 = nil
      Globalsadg234523 = nil
   end,
}
