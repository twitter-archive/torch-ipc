local parallel = require 'libparallel'
local Tree = require 'parallel.Tree'
local NullTree = require 'parallel.NullTree'

local function DiscoveredTree(nodeIndex, numNodes, nodeHost, nodePort, publish, query)
   if numNodes == 1 then
      return NullTree()
   end
   if nodeIndex == 1 then
      local server,nodePort = parallel.server(nodeHost, nodePort)
      publish(nodeHost, nodePort)
      return Tree(nodeIndex, numNodes, 2, server, nil, nodeHost, nodePort)
   else
      local rootHost,rootPort = query()
      local client = parallel.client(rootHost, rootPort)
      return Tree(nodeIndex, numNodes, 2, nil, client, nodeHost, nodePort)
   end
end

return DiscoveredTree
