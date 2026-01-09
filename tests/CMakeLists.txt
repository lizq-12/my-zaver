include_directories(${PROJECT_SOURCE_DIR}/src)

enable_testing()

add_executable(list_test list_test.c)
add_test(NAME list_test COMMAND list_test)

add_executable(priority_queue_test priority_queue_test.c ../src/priority_queue.c)
add_test(NAME priority_queue_test COMMAND priority_queue_test)

add_executable(thread_pool_test thread_pool_test.c ../src/threadpool.c)
add_test(NAME thread_pool_test COMMAND thread_pool_test)
