SET(CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR})

CMAKE_MINIMUM_REQUIRED(VERSION 2.6 FATAL_ERROR)
CMAKE_POLICY(VERSION 2.6)

FIND_PACKAGE(Torch REQUIRED)
FIND_PACKAGE(CUDA 6.5)

FIND_PACKAGE(OpenMP)
IF(OPENMP_FOUND)
   SET(CMAKE_C_FLAGS "-D_OPENMP ${CMAKE_C_FLAGS}")
ENDIF ()

SET(BUILD_STATIC YES) # makes sure static targets are enabled in ADD_TORCH_PACKAGE

SET(CMAKE_C_FLAGS "-std=c11 -pedantic -Werror -Wall -Wextra -Wno-unused-function -D_GNU_SOURCE ${CMAKE_C_FLAGS}")
SET(src
   src/ipc.c
   src/workqueue.c
   src/ringbuffer.c
   src/serialize.c
   src/cliser.c
   src/map.c
   src/spawn.c
   src/flock.c
   src/mutex.c
   src/sharedtable.c
)
SET(luasrc
   lua/Tree.lua
   lua/StaticTree.lua
   lua/DiscoveredTree.lua
   lua/LocalhostTree.lua
   lua/NullTree.lua
   lua/utils.lua
   lua/BackgroundTask.lua
   lua/BackgroundTaskPool.lua
   test/test.lua
   test/test_BackgroundTask.lua
   test/test_BackgroundTaskPool.lua
   test/test_cliser.lua
   test/test_map.lua
   test/test_spawn.lua
   test/test_Tree.lua
   test/test_workqueue.lua
   test/test_mutex.lua
   test/test_sharedtable.lua
)

ADD_TORCH_PACKAGE(ipc "${src}" "${luasrc}" "A set of primitives for ipc computation in Torch")

IF (CUDA_FOUND AND NOT ("$ENV{CUDA}" STREQUAL "NO"))
   INCLUDE_DIRECTORIES(${CUDA_INCLUDE_DIRS})
   INCLUDE_DIRECTORIES("${CUDA_SDK_ROOT_DIR}/common/inc")
   SET(CMAKE_C_FLAGS "-DUSE_CUDA ${CMAKE_C_FLAGS}")
   IF (NOT "$ENV{TH_ONLY_STATIC}" STREQUAL "YES")
      TARGET_LINK_LIBRARIES(ipc luaT TH THC ${CUDA_LIBRARIES})
   ENDIF()
ELSE()
   TARGET_LINK_LIBRARIES(ipc luaT TH)
ENDIF()

SET_TARGET_PROPERTIES(ipc_static PROPERTIES COMPILE_FLAGS "-fPIC -DSTATIC_TH")

INSTALL(FILES ${luasrc} DESTINATION "${Torch_INSTALL_LUA_PATH_SUBDIR}/ipc")
