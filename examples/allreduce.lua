local opt = lapp [[
Options:
   -h,--host            (default '127.0.0.1')            host name of the server
   -p,--port            (default 8080)                   port number of the server
   -n,--numNodes        (default 1)                      number of nodes
   -x,--node            (default 1)                      which node index is this?
   -b,--base            (default 2)                      power of 2 base of the tree of nodes
   -d,--dimensions      (default '1000,1000')            comma delimited tensor dimensions
   -i,--iterations      (default 1000)                   number of send/recv iterations
   --verify                                              verify contents of transmission (slows things down)
   --verbose                                             print lots of network stats
   --cuda                                                use CUDA tensors
]]

-- Load our requires
local ipc = require 'libipc'
local sys = require 'sys'
local Tree = require 'ipc.Tree'

-- Load cutorch if CUDA was requested
if opt.cuda then
   print('loading cutorch...')
   local ok = pcall(require, 'cutorch')
   if ok then
      print('cutorch loaded ok.')
   end
end

-- Create a big tensor
local dimensions = string.split(opt.dimensions, ",")
for i = 1,#dimensions do
   dimensions[i] = tonumber(dimensions[i])
end
local unpack = unpack or table.unpack
local t0 = torch.randn(unpack(dimensions)):float()
if opt.cuda then
   t0 = t0:cuda()
end

-- Create the tree of nodes
local client,server
if opt.node == 1 then
   server = ipc.server(opt.host, opt.port)
   server:clients(opt.numNodes - 1, function(client) end)
else
   client = ipc.client(opt.host, opt.port)
end
local tree = Tree(opt.node, opt.numNodes, opt.base, server, client, opt.host, opt.port + opt.node)

-- Iterate!
sys.tic()
for i = 1,opt.iterations do
   tree.allReduce(t0, function(a, b) return a:add(b) end)
end
print('did '..opt.iterations..' in '..sys.toc()..' seconds')
if opt.verbose then
   tree.netStats()
end
