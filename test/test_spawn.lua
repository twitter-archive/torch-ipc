local test = require 'regress'
local ipc = require 'libipc'

test {
   testStdoutAll = function()
      local p = ipc.spawn({
         file = 'echo',
         args = {
            'what',
            'up',
            'dawg',
         },
      })
      assert(type(p:stdoutFileId()) == 'number')
      assert(p:stdout('*all') == 'what up dawg\n')
      assert(p:stdout('*all') == nil)
      assert(p:wait() == 0)
   end,

   testStdoutLine = function()
      local p = ipc.spawn({
         file = 'echo',
         args = {
            'what',
            'up',
            'dawg',
         },
      })
      assert(p:stdout('*line') == 'what up dawg')
      assert(p:stdout('*line') == nil)
      assert(p:wait() == 0)
   end,

   testStdoutNumber = function()
      local p = ipc.spawn({
         file = 'echo',
         args = {
            'what',
            'up',
            'dawg',
         },
      })
      assert(p:stdout(256) == 'what up dawg\n')
      assert(p:stdout(256) == nil)
      assert(p:wait() == 0)
   end,

   testStdin = function()
      local p = ipc.spawn({
         file = 'tee',
      })
      p:stdin('a\n')
      p:stdin('b\n')
      p:stdin('c\n')
      p:stdin('d')
      p:stdin()
      assert(p:stdout('*line') == 'a')
      assert(p:stdout('*line') == 'b')
      assert(p:stdout('*line') == 'c')
      assert(p:stdout('*line') == 'd')
      assert(p:stdout() == nil)
      assert(p:wait() == 0)
   end,

   testEnv = function()
      local p = ipc.spawn({
         file = 'printenv',
         args = {
            'SOME_VAR',
         },
         env = {
            'SOME_VAR=42',
         },
      })
      assert(p:stdout('*all') == '42\n')
      assert(p:wait() == 0)
   end,

   testSignalKill = function()
      local p = ipc.spawn({
         file = 'sleep',
         args = {
            100,
         }
      })
      assert(p:wait("KILL") == 0)
   end,

   testSignalTerm = function()
      local p = ipc.spawn({
         file = 'sleep',
         args = {
            100,
         }
      })
      assert(p:wait("TERM") == 0)
   end,

   testExitCode = function()
      local p = ipc.spawn({
         file = 'bash',
         args = {
            '-c',
            'ls /this/will/never/exist/so/crazy 2> /dev/null',
         }
      })
      assert(p:wait() ~= 0)
   end,

   testRunning = function()
      local p = ipc.spawn({
         file = 'sleep',
         args = {
            1,
         },
      })
      local i = 0
      while p:running() do
         i = i + 1
         sys.sleep(0.2)
      end
      assert(i > 3 and i < 7)    -- fuzzy
      assert(p:wait() == 0)
   end,

   testCollectGarbage = function()
      ipc.spawn({
         file = 'echo',
         args = {
            'good',
         },
      }):wait()
      collectgarbage()
      ipc.spawn({
         file = 'echo',
         args = {
            'bad',
         },
      })
      collectgarbage()
   end,

   testErrors = function()
      local ok = pcall(function()
         local p = ipc.spawn({
            file = 'sleep',
            args = {
               100,
            },
         })
         error("die before waiting")
         p:wait()
      end)
      assert(ok == false)
      collectgarbage()
   end,

   testMisuse = function()
      local ok, msg

      ok, msg = pcall(function()
         local p = ipc.spawn({
            file = 'echo',
            args = 'what', -- should be a table
         })
      end)
      assert(ok == false)
      assert(string.find(msg, "expected a table"))

      ok, msg = pcall(function()
         local p = ipc.spawn({
            file = function() return 'echo' end, -- should be a string
         })
      end)
      assert(ok == false)
      assert(string.find(msg, "expected a string"))
   end,
}
