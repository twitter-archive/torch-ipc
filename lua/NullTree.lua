local walkTable = require 'ipc.utils'.walkTable

local function NullTree()
   return {
      nodeIndex = 1,
      numNodes = 1,
      walkTable = walkTable,
      allReduce = function(value) return value, 1 end,
      scatter = function(value) return value end,
      netStats = function() end,
   }
end

return NullTree
