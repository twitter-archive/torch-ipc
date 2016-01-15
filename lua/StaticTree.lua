local parallel = require 'libparallel'
local Tree = require 'parallel.Tree'
local NullTree = require 'parallel.NullTree'

local function StaticTree(nodeIndex, numNodes, nodeHost, nodePort, rootHost, rootPort)
   if numNodes == 1 then
      return NullTree()
   end
   if nodeIndex == 1 then
      local server = parallel.server(nodeHost, nodePort)
      return Tree(nodeIndex, numNodes, 2, server, nil, nodeHost, nodePort)
   else
      local client = parallel.client(rootHost, rootPort)
      return Tree(nodeIndex, numNodes, 2, nil, client, nodeHost, nodePort)
   end
end

return StaticTree
