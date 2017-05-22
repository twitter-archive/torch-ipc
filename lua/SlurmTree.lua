local ipc = require 'libipc'
local Tree = require 'ipc.Tree'
local NullTree = require 'ipc.NullTree'

local function SlurmTree(fn, tasksPerGpu, hostAddress)
   tasksPerGpu = tasksPerGpu or 1
   local slurmProcId = tonumber(os.getenv("SLURM_PROCID"))
   local numNodes = tonumber(os.getenv("SLURM_NTASKS"))
   local slurmNNodes = tonumber(os.getenv("SLURM_JOB_NUM_NODES"))
   local tasksPerHost = math.ceil(numNodes / slurmNNodes)
   local nodeIndex = slurmProcId + 1

   fn = fn or os.getenv("HOME")..'/.torch'
   fpath = fn..'/slurm.'..os.getenv("SLURM_JOBID")..'.server'
   local function publish(host, port)
      os.execute('mkdir -p '..fn)
      local f = io.open(fpath, 'w')
      f:write(host..':'..port)
      f:close()
   end
   local function query()
      while true do
         local f = io.open(fpath, 'r')
         if f then
            local s = f:read('*all')
            if type(s) == 'string' then
               local p = s:split(':')
               if type(p) == 'table' and #p == 2 then
                  return p[1], tonumber(p[2])
               end
            end
            f:close()
         end
      end
   end
   local function rcsvAllPairs(base, numNodes, index, depth, linkFunc)
      local function link(a, b, d)
         if a <= numNodes and b <= numNodes then
            linkFunc(a, b, d)
         end
      end
      if depth == 0 then
         local skip = math.pow(base, depth + 1)
         for j = index + 2, index + skip do
            link(index + 1, j, depth)
         end
      else
         local skip = math.pow(base, depth)
         link(index + 1, index + skip + 1, depth)
         for c = 0, base - 1 do
            rcsvAllPairs(base, numNodes, index + (c * skip), depth - 1, linkFunc)
         end
      end
   end
   -- cluster the tasks on each host then build tree between hosts
   local function buildTree(base, numNodes, index, depth, linkFunc)
      local numHosts = math.ceil(numNodes / tasksPerHost)
      --build local trees between tasks on each host
      rcsvAllPairs(base, tasksPerHost, index, depth, function(to, from)
         for hostIdx = 0, numHosts-1 do
            local startIdx = hostIdx * tasksPerHost
            linkFunc(startIdx + to, startIdx + from)
         end
      end)
      --build tree between the primary tasks on each host
      rcsvAllPairs(base, numHosts, index, depth, function(to, from)
         linkFunc((to-1) * tasksPerHost + 1, (from-1) * tasksPerHost + 1)
      end)
   end

   local tree = nil
   if numNodes == 1 then
      tree = NullTree()
   else
      local nodeHost = hostAddress or sys.execute('/bin/hostname')
      local nodePort = nil
      if nodeIndex == 1 then
         local server,nodePort = ipc.server(nodeHost, nodePort)
         publish(nodeHost, nodePort)
         tree = Tree(nodeIndex, numNodes, 2, server, nil, nodeHost, nodePort, buildTree)
      else
         local rootHost,rootPort = query()
         local client = ipc.client(rootHost, rootPort)
         tree = Tree(nodeIndex, numNodes, 2, nil, client, nodeHost, nodePort, buildTree)
      end
   end
   tree['gpu'] = math.floor((slurmProcId % tasksPerHost) / tasksPerGpu) + 1
   return tree
end

return SlurmTree
