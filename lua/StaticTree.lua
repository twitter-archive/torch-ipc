local ipc = require 'libipc'
local Tree = require 'ipc.Tree'
local NullTree = require 'ipc.NullTree'

local function StaticTree(nodeIndex, numNodes, nodeHost, nodePort, rootHost, rootPort)
   if numNodes == 1 then
      return NullTree()
   end
   if nodeIndex == 1 then
      local server = ipc.server(nodeHost, nodePort)
      return Tree(nodeIndex, numNodes, 2, server, nil, nodeHost, nodePort)
   else
      local client = ipc.client(rootHost, rootPort)
      return Tree(nodeIndex, numNodes, 2, nil, client, nodeHost, nodePort)
   end
end

return StaticTree
