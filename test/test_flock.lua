local test = require 'regress'
local ipc = require 'libipc'

test {
   testSimple = function()
      local fn = os.tmpname()
      local flock = ipc.flock(fn)
      assert(ipc.flock(fn, true) == nil)
      flock:write("test")
      flock:close()
      flock = nil
      collectgarbage()
      local flock = ipc.flock(fn)
      assert(flock:read() == "test")
   end,

   testDeadProcess = function()
      local fn = os.tmpname()
      os.remove(fn)
      local pid = ipc.fork()
      if pid == 0 then
         local flock = ipc.flock(fn)
         sys.sleep(0.1)
         flock:close()
         os.exit(0)
      else
         local i = 0
         while ipc.flock(fn, true) == nil do
            i = i + 1
            if pid then
               ipc.waitpid(pid)
               pid = nil
            end
         end
         assert(i > 0)
      end
   end,
}
