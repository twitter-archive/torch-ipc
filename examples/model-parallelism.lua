require 'nn'
local ipc = require 'libipc'

local layerFrameSizes = {20, 10, 5, 1}
local datasetSize = 10000

-- Generate data
local inputSize = layerFrameSizes[1]
local dataset = {}
for i=1,datasetSize do
   local timesteps = 100+torch.random(6)
   table.insert(dataset, torch.rand(timesteps, inputSize))
end

local function makeLayer(seq, inputFrameSize, outputFrameSize)
   local kW = 5
   seq:add(nn.TemporalConvolution(inputFrameSize, outputFrameSize, kW))
   seq:add(nn.Tanh())
end

local function pairseq(xs)
   return tablex.zip(tablex.sub(xs,1,-2), tablex.sub(xs,2,-1))
end

-- Compute results for single-threaded model
local single = nn.Sequential()
for i,x in ipairs(pairseq(layerFrameSizes)) do
   local inputFrameSize = x[1]
   local outputFrameSize = x[2]
   makeLayer(single, inputFrameSize, outputFrameSize)
end
single:add(nn.Max(1))
print(single)

sys.tic()
local results = {}
for i,x in ipairs(dataset) do
   table.insert(results, single:forward(x)[1])
end
local t = sys.toc()
print('single-threaded took '..t..'s')

-- Compute results for multi-threaded model
local stages = {}
for i,x in ipairs(pairseq(layerFrameSizes)) do
   local inputFrameSize = x[1]
   local outputFrameSize = x[2]
   local layer = {
      layerType='nn.TemporalConvolution',
      inputFrameSize=inputFrameSize,
      outputFrameSize=outputFrameSize,
      kW=5,
   }
   table.insert(stages, {layer=layer, output=ipc.channel()})
end
table.insert(stages, {layer={layerType='nn.Max',dimension=1}, output=ipc.channel()})

local input = ipc.channel()

-- set up input channels in each stage
for i,stage in ipairs(stages) do
   if i == 1 then
      stage.input = input
   else
      stage.input = stages[i-1].output
   end
end

-- Set stages up with workers
local stageWorkers = {}
for i,stage in ipairs(stages) do
   local worker = ipc.map(1, function(layerSpec, input, output)
      local nn = require 'nn'
      local ipc = require 'libipc'
      local layer
      if layerSpec.layerType == 'nn.Max' then
         layer = nn.Max(layerSpec.dimension)
      else
         layer = nn.TemporalConvolution(
            layerSpec.inputFrameSize,
            layerSpec.outputFrameSize,
            layerSpec.kW
         )
      end
      while true do
         local nonblocking = false
         local status, x = input:read(nonblocking)
         if status == ipc.channel.DRAINED then
            output:close()
            break
         else
            output:write(layer:forward(x))
         end
      end
   end, stage.layer, stage.input, stage.output)
   table.insert(stageWorkers, worker)
end

sys.tic()
for i,x in ipairs(dataset) do
   input:write(x)
end
input:close()
for i,worker in ipairs(stageWorkers) do
   worker:join()
end
local output = stages[#stages].output
local multithreadedResults = {}
while true do
   local nonblocking = false
   local status, x = output:read(nonblocking)
   if status == ipc.channel.DRAINED then
      break
   else
      table.insert(multithreadedResults, x[1])
   end
end
local t = sys.toc()
print('multi-threaded took '..t..'s')
