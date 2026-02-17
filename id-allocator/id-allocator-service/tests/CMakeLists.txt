include(GoogleTest)

# Helper function: add a test binary, link it, and register it.
function(add_id_allocator_test test_name)
  add_executable(${test_name} ${test_name}.cpp)
  target_link_libraries(${test_name} PRIVATE
    id_allocator_lib
    GTest::gtest_main
    GTest::gmock
  )
  gtest_discover_tests(${test_name})
endfunction()

add_id_allocator_test(partition_test)
add_id_allocator_test(id_format_test)
add_id_allocator_test(memory_store_test)
add_id_allocator_test(bucket_selector_test)
add_id_allocator_test(super_block_allocator_test)
add_id_allocator_test(integration_test)
