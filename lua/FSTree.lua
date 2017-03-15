local DiscoveredTree = require 'ipc.DiscoveredTree'
local ipc = require 'libipc'
local sys = require 'sys'

local function FSTree(numNodes, nodeIndex, fn)
   fn = fn or '/tmp/torch.'..ipc.getppid()..'.server'
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
   return DiscoveredTree(nodeIndex, numNodes, sys.execute('/bin/hostname'), nil, publish, query)
end

return FSTree
