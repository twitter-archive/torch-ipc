SET(CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR})

CMAKE_MINIMUM_REQUIRED(VERSION 2.6 FATAL_ERROR)
CMAKE_POLICY(VERSION 2.6)

FIND_PACKAGE(Torch REQUIRED)
FIND_PACKAGE(CUDA 6.5)

FIND_PACKAGE(OpenMP)
IF(OPENMP_FOUND)
   SET(CMAKE_C_FLAGS "-D_OPENMP ${CMAKE_C_FLAGS}")
ENDIF ()

SET(CMAKE_C_FLAGS "--std=c99 -pedantic -Werror -Wall -Wextra -Wno-unused-function -D_GNU_SOURCE ${CMAKE_C_FLAGS}")
SET(src
   src/parallel.c
   src/workqueue.c
   src/ringbuffer.c
   src/serialize.c
   src/cliser.c
   src/map.c
   src/error.c
)
SET(luasrc
   lua/Tree.lua
)

ADD_TORCH_PACKAGE(parallel "${src}" "${luasrc}" "A set of primitives for parallel computation in Torch")

IF (CUDA_FOUND AND ("$ENV{CUDA}" STREQUAL "YES"))
   INCLUDE_DIRECTORIES(${CUDA_INCLUDE_DIRS})
   INCLUDE_DIRECTORIES("${CUDA_SDK_ROOT_DIR}/common/inc")
   SET(CMAKE_C_FLAGS "-DUSE_CUDA ${CMAKE_C_FLAGS}")
   IF (NOT "$ENV{TH_ONLY_STATIC}" STREQUAL "YES")
      TARGET_LINK_LIBRARIES(parallel luaT TH THC)
   ENDIF()
ELSE()
   TARGET_LINK_LIBRARIES(parallel luaT TH)
ENDIF()

SET_TARGET_PROPERTIES(parallel_static PROPERTIES COMPILE_FLAGS "-fPIC -DSTATIC_TH")

INSTALL(FILES ${luasrc} DESTINATION "${Torch_INSTALL_LUA_PATH_SUBDIR}/parallel")
