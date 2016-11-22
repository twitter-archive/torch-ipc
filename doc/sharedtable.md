# sharedtable #

```lua
local t = ipc.sharedtable([tbl, move])
```

A `sharedtable` is a table that can be shared between threads.
The constructor can take an optional `tbl` argument, which is a Lua table.
When provided, `tbl` is used to initialize the `sharedtable`.

The optional `move` argument specifyies that elements should be moved from original table to the new one (defaults to `false`).
This is useful in case the table is very big and we don't want to have 2 copies of the full data in memory.

A `sharedtable` can be shared between threads, either by passing it through
a `ipc.workqueue` or as an argument to `ipc.map`.
Here is an example using `ipc.map`:

```lua
local t = ipc.sharedtable({0})
ipc.map(2, function(tab, threadid)
   for i=1,100 do
      if (i % 2) + 1 == threadid then
         tab[1] = i
      end
   end
end, t):join()

for i=1,100 do
   assert(t[i] == i)
end
```

Indeed, the `sharedtable` instance `t` is shared between 2 worker threads and the main thread.

Internally, the `sharedtable` is implemented by storing the table in a separate `lua_State`.
Recall that each thread also has its own `lua_State`.
And the way data is passed between threads is by serializing/deserializing the data from one `lua_State` to another.
The `sharedtable` uses the same principle.
By this I mean that all read and writes to the table (get/set) are implemented by (de)serializing between the calling thread's and the shared table's `lua_State`.

The `sharedtable` is subject to some caveats.
Some use-cases will still require a lock.
For example:

```lua

local t = ipc.sharedtable()
local m = ipc.mutex()
ipc.map(10, function(tab, mutex)
   for i=1,100 do
      mutex:lock()
      tab[i] = (tab[i] or 0) + 1
      mutex:unlock()
   end
end, t, m):join()
assert(#t == 100)
for i=1,100 do
   assert(t[i] == 10)
end
```

In the above example, we want to atomically add 1 to the 100 first element of the table.
Doing this requires a `mutex`.

## Nested tables

Another very big caveat of `sharedtable` should be highlighted.
When you de-serialize, you're actually creating an unnamed variable in the thread's state.
This causes some confusion, as in:

```lua
local ipc = require 'libipc'
local tbl = {key={subkey=1}}
local shared_tbl = ipc.sharedtable(tbl)
shared_tbl.key.subkey = 2
assert(shared_tbl.key.subkey == 1)
```

The reason for this is that when accessing the sub-table pointed to by `shared_tbl.key`, the entire sub-table is unserialized.
That is, we are writing directly to that unserialized table without re-serializing it into the shared table.
A solution could be to re-write the entire modified sub-table:

```lua
local sub_tbl = shared_tbl.key
sub_tbl.subkey = 2
shared_tbl.key = sub_tbl
assert(shared_tbl.key.subkey == 2)
```

But that is inefficient. A more interesting alternative is to nest shared tables:

```lua
local shared_tbl = ipc.sharedtable{key=ipc.sharedtable{subkey=1}}
shared_tbl.key.subkey = 2
assert(shared_tbl.key.subkey == 2)
```

