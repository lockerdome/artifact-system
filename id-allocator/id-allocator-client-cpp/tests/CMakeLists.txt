include(GoogleTest)

function(add_id_allocator_client_cpp_test test_name)
  add_executable(${test_name} ${test_name}.cpp)
  target_link_libraries(${test_name} PRIVATE
    id_allocator_client_cpp_lib
    GTest::gtest_main
    GTest::gmock
  )
  gtest_discover_tests(${test_name})
endfunction()

add_id_allocator_client_cpp_test(block_test)
add_id_allocator_client_cpp_test(retry_test)
add_id_allocator_client_cpp_test(client_integration_test)
