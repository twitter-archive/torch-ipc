# ipc.spawn #

The IPC library provides a more modern replacement for Lua's built in
io.popen functionality. You can use the ipc.spawn function to create
child processes, read from stdout, and write to stdin.

```lua
   local ipc = require 'libipc'
   local exitCode = ipc.spawn({ file = 'which', args = { 'nvcc' } }:wait()
   if exitCode == 0 then
      print('nvcc is present')
   end
```

The function ipc.spawn takes a table of options.

   * __file__ is the name of the executable you wish to run. Finding the executable
      follows the rules laid down in [posix_spawn](http://linux.die.net/man/3/posix_spawn).
      In short, if __file__ contains a slash '/' then it is used a direct path,
      relative or absolute, to the executable. If __file__ does not contain a slash '/'
      then the environment variable __PATH__ is used to search.

   * __args__ is an optional table of arguments to be passed to the executable. You do
      not need to worry about quoting or escaping arguments that contains spaces as
      these args are passed directly to the executable.

      ```lua
         local ipc = require 'libipc'
         local p = ipc.spawn({ file = 'echo', args = { 'there -is- no need\nto "escape"' } })
         print(p:stdout('*line')) -- will print 'there -is- no need'
         print(p:stdout('*line')) -- will print 'to "escape"'
         p:wait()
      ```

   * __env__ is an optional table of environment variables to pass to the executable.
      By default if __env__ is not specified the executable inherits the exact same
      environment as the spawning process. The elements of the __env__ table take the
      form of __VAR=VALUE__.

      ```lua
         local ipc = require 'libipc'
         local p = ipc.spawn({ file = 'printenv', args = { 'SOME_VAR' }, env = { 'SOME_VAR=42' } })
         print(p:stdout('*all')) -- will print '42'
         p:wait()
      ```

Once the child executable is spawned ipc.spawn will return a Lua object that
can be used to control the child process.

   * __:pid()__ returns the system process ID of the child process.

      ```lua
         local ipc = require 'libipc'
         local p = ipc.spawn({ file = 'pwd' })
         print('the child process pid is '..p:pid())
         p:wait()
      ```

   * __:running()__ returns true if the child process is still running, false if it is done.

      ```lua
         local ipc = require 'libipc'
         local p = ipc.spawn({ file = 'sleep', args = { 1 } })
         while p:running() do
            print('still sleeping!')
         end
         p:wait()
      ```

   * __:wait(optional string)__ waits for the child process to complete and returns its exit code.
      The __:wait()__ function is blocking, if the child process never ends then __:wait()__
      will never return. You can optionally send a signal to the child process by passing
      in an extra string argument to __:wait()__. The supported signals are "TERM" and "KILL".

      ```lua
         local ipc = require 'libipc'
         local p1 = ipc.spawn({ file = 'sleep', args = { 1 } })
         p1:wait() -- this will block for 1 second
         local p2 = ipc.spawn({ file = 'sleep', args = { 1 } })
         p2:wait("TERM") -- this will return immediately since we sent a SIGTERM
      ```

   * __:stdin(optional string)__ will write a string to the child processes stdin.
      You can call this as many times as you need to. If you wish to close the stdin pipe
      then call __:stdin()__ with no arguments. Some processes will not quit until stdin
      is closed. Calling __:wait()__ will also close the stdin pipe.

      ```lua
         local ipc = require 'libipc'
         local p = ipc.spawn({ file = 'tee' })
         p:stdin('hi\n')
         p:stdin() -- close the stdin pipe so tee will terminate
         print(p:stdout('*all')) -- read all of tee's stdout and print it
         p:wait()
      ```

   * __:stdout(arg)__ will read some amount of data from the child processes stdout pipe.
      The stdout function supports the same arguments as Lua files. There are 3 ways to
      read stdout. You can pass in __*line__ to read a single line (the new line will not be
      returned with the resulting string). You can pass in __*all__ to read all of stdout.
      Finally, you can pass in a number indicating how many bytes of stdout you would like to read.
      When there is no more stdout to read, due to EOF, the function will return __nil__.

      ```lua
         local ipc = require 'libipc'
         local p = ipc.spawn({ file = 'tee' })
         p:stdin('a\n')
         p:stdin('b\n')
         p:stdin('c')
         p:stdin('d\n')
         p:stdin() -- close the stdin pipe so tee will terminate
         -- read and print stdout until we see nil
         repeat
            local line = p:stdout('*line')
            if line then
               print(line)
            end
         until not line
         p:wait()
      ```

Orpaned ipc.spawn objects that are eventually garbage collected by Lua will automatically
send SIGTERM to the child process and then wait on it to exit gracefully. This could lead
to pauses or infinite hangs during garbage collection. It is therefore **highly** recommended
that you always call __:wait()__ on spawned child processes before you let go of the
reference to the ipc.spawn object.
