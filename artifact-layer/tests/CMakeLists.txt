include(GoogleTest)

# Helper function: add a test binary, link it, and register it.
function(add_artifact_layer_test test_name)
  add_executable(${test_name} ${test_name}.cpp)
  target_link_libraries(${test_name} PRIVATE
    artifact_layer_lib
    GTest::gtest_main
    GTest::gmock
  )
  gtest_discover_tests(${test_name})
endfunction()

add_artifact_layer_test(placeholder_test)
add_artifact_layer_test(proto_compilation_test)
add_artifact_layer_test(memory_storage_test)
add_artifact_layer_test(lakefs_storage_test)
add_artifact_layer_test(base64url_test)
add_artifact_layer_test(artifact_path_test)
add_artifact_layer_test(index_key_encoder_test)
