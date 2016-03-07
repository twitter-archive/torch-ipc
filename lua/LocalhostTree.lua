local DiscoveredTree = require 'ipc.DiscoveredTree'
local ipc = require 'libipc'

local function LocalhostTree(nodeIndex, numNodes, ppid)
   local fn = '/tmp/'..(ppid or ipc.getppid())..'.localhost'
   local function publish(host, port)
      local f = io.open(fn, 'w')
      f:write(host..':'..port)
      f:close()
   end
   local function query()
      while true do
         local f = io.open(fn, 'r')
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
   return DiscoveredTree(nodeIndex, numNodes, '127.0.0.1', nil, publish, query)
end

return LocalhostTree
