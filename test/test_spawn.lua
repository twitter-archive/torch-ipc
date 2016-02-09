local test = require 'regress'
local ipc = require 'libipc'

test {
   testStdout = function()
      local p = ipc.spawn({
         file = 'echo',
         args = {
            'what',
            'up',
            'dawg',
         },
      })
      local line = p:stdout():read('*all')
      assert(line == 'what up dawg\n')
      assert(p:wait() == 0)
      p:close()
   end,

   testStderr = function()
      local p = ipc.spawn({
         file = 'bash',
         args = {
            '-c',
            'echo hi 1>&2',
         },
         stderr = true,
      })
      local line = p:stderr():read('*all')
      assert(line == 'hi\n')
      assert(p:wait() == 0)
      p:close()
   end,

   testStdin = function()
      local p = ipc.spawn({
         file = 'tee',
      })
      local stdin = p:stdin()
      stdin:write('yoyo\n')
      stdin:close()
      local line = p:stdout():read('*all')
      assert(line == 'yoyo\n')
      assert(p:wait() == 0)
      p:close()
      collectgarbage()
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
      local line = p:stdout():read('*all')
      assert(line == '42\n')
      assert(p:wait() == 0)
      p:close()
   end,
}
