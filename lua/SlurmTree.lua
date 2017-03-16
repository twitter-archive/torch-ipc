local FSTree = require 'ipc.FSTree'
local paths = require 'paths'

local function SlurmTree(numNodes, fn)
   fn = fn or paths.home..'/.torch/slurm.'..os.getenv("SLURM_JOBID")..'.server'
   local procId = os.getenv("SLURM_PROCID")
   local nodeIndex = nil
   if procId == "0" then
      nodeIndex = 1
   end
   return FSTree(numNodes, nodeIndex, fn)
end

return SlurmTree
