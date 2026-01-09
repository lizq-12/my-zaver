cmake_minimum_required (VERSION 2.6)
project (zaver)

option(ZV_ENABLE_WERROR "Treat warnings as errors" OFF)
option(ZV_ENABLE_SANITIZERS "Enable ASan/UBSan (recommended with Debug)" OFF)

if (NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

file(GLOB SOURCES "src/*.c")
# NOTE: CMAKE_C_FLAGS must be a plain string. If it becomes a CMake list
# (e.g. by using unquoted expansions), generated Makefiles may split flags
# into separate shell commands ("/bin/sh: 1: -g: not found").
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -pthread")

set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -O0 -g")
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O2 -DNDEBUG")
set(CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO} -O2 -g -DNDEBUG")

if (ZV_ENABLE_WERROR)
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Werror")
endif()

if (ZV_ENABLE_SANITIZERS)
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-omit-frame-pointer -fsanitize=address,undefined")
	set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address,undefined")
endif()

add_executable(zaver ${SOURCES})
add_subdirectory(tests)
