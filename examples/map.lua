local ipc = require 'libipc'

-- Make 3 files
torch.save('f1.t7', torch.randn(1, 2))
torch.save('f2.t7', torch.randn(2, 2))
torch.save('f3.t7', torch.randn(3, 2))

-- Load 3 files in ipc
local t1,t2,t3 = ipc.map(3, function(fileNames, mapid)
   return torch.load(fileNames[mapid])
end, {'f1.t7', 'f2.t7', 'f3.t7'}):join()

-- Show what we loaded
print(t1)
print(t2)
print(t3)

-- Cleanup
os.execute('rm f1.t7')
os.execute('rm f2.t7')
os.execute('rm f3.t7')
