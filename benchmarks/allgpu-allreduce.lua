local opt = lapp [[
Options:
   -d,--dimensions      (default '1000,1000')            comma delimited tensor dimensions
   -i,--iterations      (default 1000)                   number of send/recv iterations
   -v,--verify                                           verify the results (affects performance)
]]

local ipc = require 'libipc'

-- Use a child to get the device count (can't fork after CUDA init)
local ppid = ipc.getppid()
local pid = ipc.fork()
if pid == 0 then
   local cutorch = require 'cutorch'
   os.exit(cutorch.getDeviceCount())
end
local deviceCount = ipc.waitpid(pid)

-- Fork a new process per device
print('Found '..deviceCount..' GPUs, forking children...')
local device = 1
for i = 2,deviceCount do
   local pid = ipc.fork()
   if pid == 0 then
      device = i
      break
   end
end

-- This is the forked child process
local cutorch = require 'cutorch'
local sys = require 'sys'
local LocalhostTree = require 'ipc.LocalhostTree'

-- grab a GPU
cutorch.setDevice(device)

-- Create the tree of nodes (one per GPU)
local tree = LocalhostTree(device, deviceCount, ppid)

-- Create a big tensor
local dimensions = string.split(opt.dimensions, ",")
for i = 1,#dimensions do
   dimensions[i] = tonumber(dimensions[i])
end
local unpack = unpack or table.unpack
local t0 = torch.randn(unpack(dimensions)):cuda()

-- Iterate!
sys.tic()
for i = 1,opt.iterations do
   if opt.verify then
      t0:fill(i / opt.iterations):pow(device)
   end
   tree.allReduce(t0, function(a, b) return a:add(b) end)
   if opt.verify then
      local expected = 0
      for d = 1,deviceCount do
         expected = expected + math.pow(i / opt.iterations, d)
      end
      t0:add(-expected)
      local err = math.max(math.abs(t0:min()), math.abs(t0:max()))
      assert(err < 1e-6, 'err too high: '..err)
   end
end
if device == 1 then
   print('did '..opt.iterations..' in '..sys.toc()..' seconds')
   tree.netStats()
end
