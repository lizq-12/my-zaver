include_directories(${PROJECT_SOURCE_DIR}/src)

option(ZV_BUILD_UPSTREAM_TESTS "Build legacy upstream C unit tests" OFF)

if (ZV_BUILD_UPSTREAM_TESTS)
	add_executable(list_test list_test.c)

	add_executable(priority_queue_test priority_queue_test.c ../src/priority_queue.c)

	add_executable(thread_pool_test thread_pool_test.c ../src/threadpool.c)
endif()
