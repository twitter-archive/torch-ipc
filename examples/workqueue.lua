local ipc = require 'libipc'

-- Make 3 files
torch.save('f1.t7', torch.randn(1, 2))
torch.save('f2.t7', torch.randn(2, 2))
torch.save('f3.t7', torch.randn(3, 2))

-- Create a named workqueue
local q = ipc.workqueue('my queue')

-- Create 2 background workers that read from the named workqueue
local workers = ipc.map(2, function()
   -- This function is not a closure, its a totally clean Lua environment
   local ipc = require 'libipc'
   -- Open the queue by name (the main thread already created it)
   local q = ipc.workqueue('my queue')
   repeat
      -- Read the next file name off the workqueue
      local fileName = q:read()
      if fileName then
         -- Load the file and write its contents back into the workqueue
         q:write(torch.load(fileName))
      end
   until fileName == nil
end)

-- Write the file names into the workqueue
q:write('f1.t7')
q:write('f2.t7')
q:write('f3.t7')

-- Read back the 3 answers and print them
print(q:read())
print(q:read())
print(q:read())

-- Write nil 2X to tell both workers to finish
q:write(nil)
q:write(nil)

-- Wait for the workers to finish up
workers:join()

-- Shutdown the workqueue
q:close()

-- Cleanup
os.execute('rm f1.t7')
os.execute('rm f2.t7')
os.execute('rm f3.t7')
