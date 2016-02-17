local test = require 'regress'
local ipc = require 'libipc'
local Tree = require 'ipc.Tree'

local function testAllReduce(njobs, base, makeValue, reduce)
   local server, port = ipc.server('127.0.0.1')
   local m = ipc.map(njobs - 1, function(njobs, base, port, makeValue, reduce, mapid)
      local ipc = require 'libipc'
      local Tree = require 'ipc.Tree'
      local client = ipc.client('127.0.0.1', port)
      local jobid = mapid + 1
      local tree = Tree(jobid, njobs, base, nil, client, '127.0.0.1')
      local value = makeValue(jobid)
      local value = tree.allReduce(value, reduce)
      return value
   end, njobs, base, port, makeValue, reduce)
   server:clients(njobs - 1, function(client) end)
   local tree = Tree(1, njobs, base, server, nil, '127.0.0.1', port)
   local value = makeValue(1)
   local final = tree.allReduce(value, reduce)
   local ret = { m:join() }
   table.insert(ret, 1, final)
   return ret
end

test {
   testTreeNumbersBase2 = function()
      local ret = testAllReduce(8, 2,
         function(jobid) return jobid end,
         function(a, b) return a + b end)
      test.mustBeTrue(#ret == 8, 'expected 8 results, not '..#ret)
      for _,rv in ipairs(ret) do
         test.mustBeTrue(rv == 36, 'expected final value of 36, not '..rv)
      end
   end,

   testTreeNumbersArrayBase2 = function()
      local ret = testAllReduce(4, 2,
         function(jobid) return { jobid, 2 * jobid } end,
         function(a, b) return a + b end)
      test.mustBeTrue(#ret == 4, 'expected 4 results, not '..#ret)
      for _,rv in ipairs(ret) do
         test.mustBeTrue(rv[1] == 10, 'expected final value of 10, not '..rv[1])
         test.mustBeTrue(rv[2] == 20, 'expected final value of 20, not '..rv[2])
      end
   end,

   testTreeTensorsBase2 = function()
      local ret = testAllReduce(2, 2,
         function(jobid)
            return torch.Tensor(10):fill(jobid)
         end,
         function(a, b)
            a:add(b)
            return a
         end)
      test.mustBeTrue(#ret == 2, 'expected 8 results, not '..#ret)
      for _,rv in ipairs(ret) do
         test.mustBeTrue(rv:sum() == 30, 'expected final value of 360, not '..rv:sum())
      end
   end,

   testTreeTensorsBase4 = function()
      local ret = testAllReduce(8, 4,
         function(jobid)
            return torch.Tensor(10):fill(jobid)
         end,
         function(a, b) return a:add(b) end)
      test.mustBeTrue(#ret == 8, 'expected 8 results, not '..#ret)
      for _,rv in ipairs(ret) do
         test.mustBeTrue(rv:sum() == 360, 'expected final value of 360, not '..rv:sum())
      end
   end,

   testTreeTensorsBase8 = function()
      local ret = testAllReduce(8, 8,
         function(jobid)
            return torch.Tensor(10):fill(jobid)
         end,
         function(a, b) return a + b end)
      test.mustBeTrue(#ret == 8, 'expected 8 results, not '..#ret)
      for _,rv in ipairs(ret) do
         test.mustBeTrue(rv:sum() == 360, 'expected final value of 360, not '..rv:sum())
      end
   end,

   testUnevenNumberOfSteps = function()
      local function expected(n, ni)
         local c = 0
         for i = 1,ni do
            for j = i,n do
               c = c + j
            end
         end
         return c
      end
      local function reduce(a, b) return a + b end
      local function zero() return 0 end
      local function loop(njobs, jobid, tree, reduce, zero)
         local value = 0
         for i = 1,jobid do
            value = value + tree.allReduce(jobid, reduce)
         end
         value = value + tree.allReduce(nil, reduce, zero)
         return value
      end
      local njobs = 4
      local base = 2
      local server, port = ipc.server('127.0.0.1')
      local m = ipc.map(njobs - 1, function(njobs, base, port, reduce, zero, loop, mapid)
         local ipc = require 'libipc'
         local Tree = require 'ipc.Tree'
         local client = ipc.client('127.0.0.1', port)
         local jobid = mapid + 1
         local tree = Tree(jobid, njobs, base, nil, client, '127.0.0.1')
         return loop(njobs, jobid, tree, reduce, zero)
      end, njobs, base, port, reduce, zero, loop)
      server:clients(njobs - 1, function(client) end)
      local tree = Tree(1, njobs, base, server, nil, '127.0.0.1')
      local final = loop(njobs, 1, tree, reduce, zero)
      local ret = { m:join() }
      table.insert(ret, 1, final)
      test.mustBeTrue(#ret == njobs, 'expected '..njobs..' results, not '..#ret)
      for i,rv in ipairs(ret) do
         local e = expected(njobs, i)
         test.mustBeTrue(rv == e, 'expected final value of '..e..', not '..rv)
      end
   end,

   testTreeMultipleTensors = function()
      local ret = testAllReduce(8, 2,
         function(jobid)
            return { torch.Tensor(10):fill(jobid), torch.Tensor(10):fill(jobid - 1) }
         end,
         function(a, b)
            a:add(b)
            return a
         end)
      test.mustBeTrue(#ret == 8, 'expected 8 results, not '..#ret)
      for _,rv in ipairs(ret) do
         test.mustBeTrue(rv[1]:sum() == 360, 'expected final value of 360, not '..rv[1]:sum())
         test.mustBeTrue(rv[2]:sum() == 280, 'expected final value of 280, not '..rv[2]:sum())
      end
   end,

   testScatter = function()
      local njobs = 4
      local base = 2
      local server, port = ipc.server('127.0.0.1')
      local m = ipc.map(njobs - 1, function(njobs, base, port, mapid)
         local ipc = require 'libipc'
         local Tree = require 'ipc.Tree'
         local client = ipc.client('127.0.0.1', port)
         local jobid = mapid + 1
         local tree = Tree(jobid, njobs, base, nil, client, '127.0.0.1')
         return tree.scatter(jobid)
      end, njobs, base, port)
      server:clients(njobs - 1, function(client) end)
      local tree = Tree(1, njobs, base, server, nil, '127.0.0.1')
      local final = tree.scatter(1)
      local ret = { m:join() }
      table.insert(ret, 1, final)
      test.mustBeTrue(#ret == njobs, 'expected '..njobs..' results, not '..#ret)
      for _,rv in ipairs(ret) do
         test.mustBeTrue(rv == 1, 'expected final value of 1, not '..rv)
      end
   end,
}
