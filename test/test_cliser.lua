local test = require 'regress'
pcall(require, 'cutorch')
local ipc = require 'libipc'

local function testCSN(numClients, test, callbackS, callbackC, extra)
   local server,port = ipc.server()
   local clients = ipc.map(numClients, function(port, callbackC, extra)
      local sys = require 'sys'
      local ipc = require 'libipc'
      local client = ipc.client(port)
      callbackC(client, extra)
      assert(client:recv() == "bye")
      client:close()
      return true
   end, port, callbackC, extra)
   callbackS(server)
   server:broadcast("bye")
   local ret = clients:join()
   server:close()
   local passed = type(ret) == 'boolean' and ret == true
   local msg = (type(ret) == 'string' and ret) or 'client failed with an unknown error'
   assert(passed, msg)
end

local function testCS(test, callbackS, callbackC, extra)
   return testCSN(1, test, callbackS, callbackC, extra)
end

local function testT(t0, t1, eq)
   local t2 = t0
   if t2:type() == 'torch.CudaTensor' then
      t2 = t2:float()
   end
   testCS(test,
      function(server)
         server:clients(1, function(client)
            assert(torch.all(torch.eq(t0, t1)) == false, "should not match before recv")
            client:recv(t1)
            local feq = eq or function()
               assert(torch.all(torch.eq(t0, t1)), "should match after recv")
            end
            feq(t0, t1)
         end)
      end,
      function(client, t2)
         client:send(t2)
      end, t2)
end

local function testTF(t0, t1)
   testCS(test,
      function(server)
         server:clients(1, function(client)
            local ok = pcall(function() client:recv(t1) end)
            assert(ok == false, "recv should have failed")
         end)
      end,
      function(client, t0)
         client:send(t0)
      end, t0)
end

test {
   testListenAndConnect = function()
      testCS(test,
         function(server)
            server:clients(1, function(client)
               local tag = client:recv()
               client:tag(tag)
               assert(client:tag() == "hi")
               local id = client:recv()
               client:id(id)
               assert(client:id() == 42)
            end)
         end,
         function(client)
            client:send("hi")
            client:send(42)
         end)
   end,

   testSlowStart = function()
      local m = ipc.map(1, function()
         local ipc = require 'libipc'
         return ipc.client('127.0.0.1', 8080)
      end)
      sys.sleep(2)
      local server = ipc.server('127.0.0.1', 8080)
      local client = m:join()
      server:clients(1, function(client) end)
      client:close()
      server:close()
   end,

   testPingPong = function()
      testCS(test,
         function(server)
            server:clients(1, function(client)
               local m0 = client:recv()
               assert(m0 == "ping", "expected a ping")
               client:send("pong")
            end)
         end,
         function(client)
            client:send("ping")
            local m1 = client:recv()
            assert(m1 == "pong", "expected a pong, saw: "..m1)
         end)
   end,

   testPingPongAsync = function()
      testCS(test,
         function(server)
            server:clients(1, function(client)
               local m0 = client:recv()
               assert(m0 == "ping", "expected a ping")
               sys.sleep(1)
               client:send("pong")
            end)
         end,
         function(client)
            client:send("ping")
            local x = 0
            while 1 do
               local m1 = client:recvAsync()
               if m1 ~= nil then
                  assert(m1 == "pong", "expected a pong, saw: "..m1)
                  assert(x > 0, "expected some delay")
                  break
               end
               x = x + 1
            end
         end)
   end,

   testBroadcast = function()
      testCSN(10, test,
         function(server)
            server:clients(10, function(client)
               local tag = client:recv()
               client:tag(tag)
            end)
            server:broadcast({ x = "2" }, "2")
            server:broadcast({ x = "3" }, "3")
            server:broadcast({ x = "1" }, "1")
            server:broadcast("bye")
         end,
         function(client)
            local tag = tostring(math.random(2))
            client:send(tag)
            local msg1 = client:recv()
            assert(msg1.x == tag)
            local msg2 = client:recv()
            assert(msg2 == "bye")
         end)
   end,

   testRecvAny = function()
      testCSN(10, test,
         function(server)
            server:clients(10, function(client) end)
            for i = 1,10 do
               local msg = server:recvAny()
               assert(msg == "hi")
            end
            server:broadcast("bye")
         end,
         function(client)
            client:send("hi")
            local msg = client:recv()
            assert(msg == "bye")
         end)
   end,

   testStoragePingPong = function()
      testCS(test,
         function(server)
            server:clients(1, function(client)
               local s0 = torch.ByteStorage(4)
               client:recv(s0)
               assert(s0:string() == "ping", "expected a ping")
               client:send(torch.ByteStorage():string("pong"))
            end)
         end,
         function(client)
            local s0 = torch.ByteStorage():string("ping")
            client:send(s0)
            local s1 = torch.ByteStorage(4)
            client:recv(s1)
            assert(s1:string() == "pong", "expected a pong")
         end)
   end,

   testCUDAStoragePingPong = function()
      if cutorch then
         local t0 = torch.randn(3, 3):float()
         local t2 = t0:cuda()
         local t1 = torch.randn(3, 3):cuda()
         testCS(test,
            function(server)
               server:clients(1, function(client)
                  assert(torch.all(torch.eq(t2, t1)) == false, "should not match before recv")
                  client:recv(t1:storage())
                  assert(torch.all(torch.eq(t2, t1)), "should match after recv")
               end)
            end,
            function(client, t0)
               client:send(t0:storage())
            end, t0)
      end
   end,

   testStorageSizeMismatch = function()
      testCS(test,
         function(server)
            server:clients(1, function(client)
               local s0 = torch.ByteStorage(3)
               local ok = pcall(function() client:recv(s0) end)
               assert(ok == false, "should fail with storage size mismatch")
            end)
         end,
         function(client)
            local s0 = torch.ByteStorage():string("ping")
            client:send(s0)
         end)
   end,

   testTensor = function()
      testT(torch.randn(5, 6, 7), torch.randn(5, 6, 7))
   end,

   testNoncontiguousTensorEasy = function()
      testT(torch.randn(5, 6):sub(2,4, 2,5), torch.randn(5, 6):sub(2,4, 2,5))
   end,

   testNoncontiguousTensorMedium = function()
      testT(torch.randn(5, 6, 7):sub(2,4, 2,5, 2,6), torch.randn(5, 6, 7):sub(2,4, 2,5, 2,6))
   end,

   testNoncontiguousTensorHard = function()
      testT(torch.randn(5, 6, 7, 8):sub(2,4), torch.randn(5, 6, 7, 8):sub(2,4))
   end,

   testCUDATensor = function()
      if cutorch then
         testT(torch.randn(3, 4, 5):cuda(), torch.randn(3, 4, 5):cuda())
      end
   end,

   testNoncontiguousCUDATensor = function()
      if cutorch then
         testT(torch.randn(8, 7, 6):cuda():sub(2,7), torch.randn(8, 7, 6):cuda():sub(2,7))
      end
   end,

   testTensorZeroSized = function()
      local t0 = torch.randn(0)
      testCS(test,
         function(server)
            server:clients(1, function(client)
               local t1 = torch.randn(0)
               client:recv(t1)
               assert(t0:nDimension() == t1:nDimension(), "should match after recv")
            end)
         end,
         function(client, t0)
            client:send(t0)
         end, t0)
   end,

   testTensorNumDimensionsMismatch = function()
      testTF(torch.randn(3, 4), torch.randn(3, 5, 6))
   end,

   testTensorDimensionSizeMismatch = function()
      testTF(torch.randn(3, 4, 5), torch.randn(3, 5, 5))
   end,

   testSerialize = function()
      local server,port = ipc.server()
      local t = ipc.map(1, function(port)
         local ipc = require 'libipc'
         return ipc.client(port)
      end, port)
      server:clients(1, function(client) end)
      local client = t:join()
      client:send('hi')
      local msg = server:recvAny()
      test.mustBeTrue(msg == 'hi', 'expected "hi", saw: '..tostring(msg))
      client:close()
      server:close()
   end,

   testNetStats = function()
      local server,port = ipc.server()
      local t = ipc.map(1, function(port)
         local ipc = require 'libipc'
         return ipc.client(port)
      end, port)
      server:clients(1, function(client) end)
      local client = t:join()
      client:send('hi')
      server:recvAny()
      test.mustBeTrue(type(client:netStats()) == 'table', 'expected a table')
      test.mustBeTrue(type(server:netStats()) == 'table', 'expected a table')
      client:close()
      server:close()
   end,
}
