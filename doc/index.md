# IPC Package Reference Manual #

   * [ipc.spawn](spawn.md) a more modern replacement to Lua's io.popen function.
   * [ipc.map](map.md) maps a Lua function onto a set of threads.
   * [ipc.mutex](mutex.md) provides utilies for locking resources and creating synchronization barriers.
   * [ipc.workqueue](workqueue.md) allows the main thread to communicate with a set of worker threads.
   * [ipc.sharedtable](sharedtable.md) provides a table that can be shared between threads.
   * [ipc.BackgroundTask](BackgroundTask.md) provides a simple, pollable interface to a single Lua function run in the background.
   * [ipc.BackgroundTaskPool](BackgroundTaskPool.md) provides a simple, pollable interface to a set of arbitrary Lua functions run in the background.
