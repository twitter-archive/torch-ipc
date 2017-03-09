local opt = lapp [[
Options:
   -d,--dimensions      (default '1000,1000')            comma delimited tensor dimensions
   -i,--iterations      (default 1000)                   number of send/recv iterations
   --verify                                              verify contents of transmission (slows things down)
   --verbose                                             print lots of network stats
   --cuda                                                use CUDA tensors
]]

-- Load our requires
local ipc = require 'libipc'
local sys = require 'sys'
-- Build the AllReduce tree
local tree = require 'ipc.SlurmTree'()
local node = tree.nodeIndex
local numNodes = tree.numNodes
local gpu = tree.gpu

-- Requires
if opt.cuda then
   print('loading cutorch...')
   local ok = pcall(require, 'cutorch')
   if ok then
      print('cutorch loaded ok.')
   end
   cutorch.setDevice(gpu)
end

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

-- Iterate!
sys.tic()
for i = 1,opt.iterations do
   tree.allReduce(t0, function(a, b) return a:add(b) end)
end
print('Task '..node..' did '..opt.iterations..' in '..sys.toc()..' seconds')
if opt.verbose then
   tree.netStats()
end
