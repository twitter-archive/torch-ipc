local opt = lapp [[
Options:
   -h,--host            (default '127.0.0.1')            host name of the server
   -p,--port            (default 8080)                   port number of the server
   -s,--server          (default 0)                      number of clients the server should expect to connect
   -d,--dimensions      (default '1000,1000')            comma delimited tensor dimensions
   -i,--iterations      (default 1000)                   number of send/recv iterations
   --verify                                              verify contents of transmission (slows things down)
   --verbose                                             print lots of network stats
   --cuda                                                use CUDA tensors
]]

-- Load our requires
local ipc = require 'libipc'
local sys = require 'sys'

-- Load cutorch if CUDA was requested
if opt.cuda then
   print('loading cutorch...')
   local ok = pcall(require, 'cutorch')
   if ok then
      print('cutorch loaded ok.')
   end
end

-- Compute the total size of the tensor
local total = 4
local dimensions = string.split(opt.dimensions, ",")
for i = 1,#dimensions do
   dimensions[i] = tonumber(dimensions[i])
   total = total * dimensions[i]
end

-- Print the network performance so far
local function printStat(n, t, i)
   if t > 0 then
      print('Did '..i..' '..n..' in '..t..' seconds')
      print('\t'..math.floor(i/t)..' ops per second')
      print('\t'..math.floor(i*total/(t*1024*1024))..' MB/s')
   end
end

if opt.server > 0 then
   local server = ipc.server(opt.host, opt.port)
   local unpack = unpack or table.unpack
   local t0 = torch.randn(unpack(dimensions)):float()
   if opt.cuda then
      t0 = t0:cuda()
   end
   local t1 = t0:clone()
   local t2 = t0:clone():mul(2)
   local totalSend = 0
   local totalRecv = 0
   for i = 1,opt.iterations do
      server:clients(opt.server, function(client)
         local s0 = sys.clock()
         client:send(t0)
         local received = client:recv()
         if received ~= "received" then
            error('bad received string')
         end
         local s1 = sys.clock()
         totalSend = totalSend + (s1 - s0)
         client:recv(t1)
         local s2 = sys.clock()
         totalRecv = totalRecv + (s2 - s1)
         if opt.verify then
            if torch.all(torch.eq(t1, t2)) == false then
               error('tensors dont match.')
            end
         end
      end)
      if i % 100 == 0 then
         printStat('sends', totalSend, i)
         printStat('recvs', totalRecv, i)
         if opt.verbose then
            print(server:netStats())
         end
      end
   end
   server:close()
else
   local client = ipc.client(opt.host, opt.port)
   local unpack = unpack or table.unpack
   local t0 = torch.randn(unpack(dimensions)):float()
   if opt.cuda then
      t0 = t0:cuda()
   end
   for i = 1,opt.iterations do
      client:recv(t0)
      if opt.verify then
         t0:mul(2)
      end
      client:send("received")
      client:send(t0)
   end
   client:close()
end
