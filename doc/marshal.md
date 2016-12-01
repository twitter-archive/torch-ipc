# ipc.marshal #

```lua
local m = ipc.marshal(obj[, upval, size, size_increment])
```

The `ipc.marshal` constructor serializes a Lua object (`obj` above).
It returns an `ipc.marshal` instance (`m` above).
The resulting `m` instance provides a simple __:read__ method for deserializing the object:

```lua
local obj = m:read()
```

When `upval` is true, the serialized `obj` can contain upvalues/closures (default is false)
The `size` and `size_increment` are used to incrementally grow the write buffer (see `ipc.workqueue`).

## Example

More concretely, suppose we want to serialize the following table:

```lua
local obj = {1,2,3,v=4,g=5}
```

We can marshal (that is, serialize) the data into `m`

```lua
local m = ipc.marshal(obj)
```

Contrary to normal serialization, `m` is not a string, but a `userdata` object.
The most useful thing about `m` is that if can be read multiple times (like a string):

```lua
local obj2 = m:read()
local obj3 = m:read()
```

This is useful when you want to serialize something only once (say, in the main thread),
and unserialize it many times (say, in worker threads).

