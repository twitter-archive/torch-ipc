local ipc = require 'libipc'
local Tree = require 'ipc.Tree'
local NullTree = require 'ipc.NullTree'

local function DiscoveredTree(nodeIndex, numNodes, nodeHost, nodePort, publish, query)
   if numNodes == 1 then
      return NullTree()
   end
   if nodeIndex == 1 then
      local server,nodePort = ipc.server(nodeHost, nodePort)
      publish(nodeHost, nodePort)
      return Tree(nodeIndex, numNodes, 2, server, nil, nodeHost, nodePort)
   else
      local rootHost,rootPort = query()
      local client = ipc.client(rootHost, rootPort)
      return Tree(nodeIndex, numNodes, 2, nil, client, nodeHost, nodePort)
   end
end

return DiscoveredTree
