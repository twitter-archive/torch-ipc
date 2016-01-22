local ipc = require 'libipc'
local walkTable = require 'ipc.utils'.walkTable

local function rcsvAllPairs(b, n, i, d, f)
   local function ff(a, b, d)
      if a <= n and b <= n then
         f(a, b, d)
      end
   end
   if d == 0 then
      local skip = math.pow(b, d + 1)
      for j = i+2,i+skip do
         ff(i + 1, j, d)
      end
   else
      local skip = math.pow(b, d)
      ff(i + 1, i + skip + 1, d)
      for c = 0,b-1 do
         rcsvAllPairs(b, n, i + (c * skip), d - 1, f)
      end
   end
end

local function Tree(nodeIndex, numNodes, base, server, client, host, port)

   local maxDepth = math.ceil(math.log(numNodes) / math.log(base))

   local function initialServer()
      -- Get every node's address and nodeIndex
      local addresses = { }
      addresses[nodeIndex] = {
         nodeIndex = nodeIndex,
         host = host,
         port = port,
      }
      server:clients(numNodes - 1, function(client)
         client:send({ q = "address?" })
         local msg = client:recv()
         assert(msg.q == "address")
         addresses[msg.nodeIndex] = {
            nodeIndex = msg.nodeIndex,
            host = msg.host,
            port = msg.port
         }
      end)
      -- Build a tree of connections to establish
      local tree = { }
      rcsvAllPairs(base, numNodes, 0, maxDepth - 1, function(to, from, depth)
         tree[from] = tree[from] or { }
         tree[from].connect = addresses[to]
         tree[to] = tree[to] or { }
         tree[to].listen = (tree[to].listen or 0) + 1
      end)
      -- Broadcast the tree of connections all nodes
      server:broadcast(tree)
      -- Order the nodes that stay connected to this server
      server:clients(function(client)
         -- Expect some clients to disconnect
         local ok,msg = pcall(function()
            client:send("order?")
            return client:recv()
         end)
         if ok then
            client:id(msg.order)
         else
            -- Node has a new parent, so close it
            client:close()
         end
      end)
      -- Make sure the entire tree is ready for action
      server:clients(function(client)
         client:send("start?")
         assert(client:recv() == "start")
      end)
   end

   local function initialClient()
      -- Open a new server, we may end up a parent (reuse the same server upvalue)
      server, port = ipc.server(host, tonumber(port))
      -- Register our address and nodeIndex
      local msg = client:recv()
      assert(msg.q == "address?")
      client:send({
         q = "address",
         nodeIndex = nodeIndex,
         host = host,
         port = port
      })
      -- Get the tree of connections
      local tree = client:recv()
      local node = tree[nodeIndex]
      if node.listen and node.listen > 0 then
         -- If we are a parent, connect the children and order them
         server:clients(node.listen, function(client)
            client:send("order?")
            local msg = client:recv()
            client:id(msg.order)
         end)
      else
         -- Just a leaf
         server:close()
         server = nil
      end
      if node.connect then
         if node.connect.nodeIndex == 1 then
            -- Already connnected to our parent
            assert(client:recv() == "order?")
            client:send({
               order = nodeIndex,
            })
         else
            -- A new parent is required (reuse the same client upvalue)
            client:close()
            client = ipc.client(node.connect.host, node.connect.port)
            assert(client:recv() == "order?")
            client:send({
               order = nodeIndex,
            })
         end
      end
      -- Wait for the start message
      assert(client:recv() == "start?")
      if server then
         -- Get our subtree ready to start
         server:clients(function(client)
            client:send("start?")
            assert(client:recv() == "start")
         end)
      end
      -- This subtree is ready
      client:send("start")
   end

   -- Establish the tree structure
   if server then
      initialServer()
   else
      initialClient()
   end

   -- We need temp space to receive tensors
   local tempValue
   local function getTempValue(value)
      if torch.isTensor(value) then
         if tempValue then
            tempValue = tempValue:typeAs(value):resizeAs(value)
         else
            tempValue = value:clone()
         end
         return tempValue
      end
   end

   -- Not the prettiest but it conserves memory when
   -- ending the allReduce on uneven # of steps per node
   local lastValue

   local function allReduceInner(value, reduce, zero)
      -- Handle uneven endings
      if zero then
         -- Restore the last value if a zero function is supplied
         value = (value[1] ~= nil and value) or lastValue
         local i = 0
         walkTable(value, function(valuei)
            i = i + 1
            return zero(valuei, i)
         end)
      else
         -- Save the last value we saw (for uneven ending)
         lastValue = value
      end
      -- Keep track of the number of done nodes
      local numDone = zero and 1 or 0
      -- Reduce the value up to the root
      if server then
         -- Recv from the shortest branch first
         server:clients(function(client)
            walkTable(value, function(valuei)
               return reduce(valuei, client:recv(getTempValue(valuei)))
            end)
            numDone = numDone + client:recv()
         end)
      end
      if client then
         walkTable(value, function(valuei)
            client:send(valuei)
         end)
         client:send(numDone)
      end
      -- Map the root value back down the tree
      if client then
         walkTable(value, function(valuei)
            return client:recv(valuei)
         end)
         numDone = client:recv()
      end
      if server then
         -- Send the longest branch first
         server:clients(function(client)
            walkTable(value, function(valuei)
               client:send(valuei)
            end)
            client:send(numDone)
         end, 1) -- Magic bit to invert the client order (longest branch first)
      end
      if zero and numDone < numNodes then
         -- If we are done, but not everyone else is, then do it again
         return allReduceInner(value, reduce, zero)
      else
         -- Return the final value and how many nodes contributed
         return value, numNodes - numDone
      end
   end

   -- Classic MPI style all reduce (reduce where all nodes get the final value)
   local function allReduce(value, reduce, zero)
      -- Support tables of values (as multiple sequential transfers)
      local isTable = type(value) == 'table'
      value = (isTable and value) or { value }
      local finalValue, numNodes = allReduceInner(value, reduce, zero)
      return (isTable and finalValue) or finalValue[1], numNodes
   end

   -- Classic MPI style scatter (root value to all nodes)
   local function scatter(value)
      -- Support tables of tensors
      local isTable = type(value) == 'table'
      value = (isTable and value) or { value }
      -- Map the root value back down the tree
      if client then
         walkTable(value, function(valuei)
            return client:recv(valuei)
         end)
      end
      if server then
         -- Send the longest branch first
         server:clients(function(client)
            walkTable(value, function(valuei)
               client:send(valuei)
            end)
         end, 1) -- Magic bit to invert the client order (longest branch first)
      end
      return (isTable and value) or value[1]
   end

   -- Handy debug info on network performance
   local function netStats()
      if server then
         print(server:netStats())
      end
      if client then
         print(client:netStats())
      end
   end

   return {
      nodeIndex = nodeIndex,
      numNodes = numNodes,
      walkTable = walkTable,
      allReduce = allReduce,
      scatter = scatter,
      netStats = netStats,
   }
end

return Tree
