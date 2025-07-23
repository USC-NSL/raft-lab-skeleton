# Proto file
get_filename_component(raft_proto "./raft.proto" ABSOLUTE)
get_filename_component(raft_proto_path "${raft_proto}" PATH)

set(raft_proto_srcs "${CMAKE_CURRENT_BINARY_DIR}/raft.pb.cc")
set(raft_proto_hdrs "${CMAKE_CURRENT_BINARY_DIR}/raft.pb.h")
set(raft_grpc_srcs "${CMAKE_CURRENT_BINARY_DIR}/raft.grpc.pb.cc")
set(raft_grpc_hdrs "${CMAKE_CURRENT_BINARY_DIR}/raft.grpc.pb.h")
add_custom_command(
      OUTPUT "${raft_proto_srcs}" "${raft_proto_hdrs}" "${raft_grpc_srcs}" "${raft_grpc_hdrs}"
      COMMAND ${_PROTOBUF_PROTOC}
      ARGS --grpc_out "${CMAKE_CURRENT_BINARY_DIR}"
        --cpp_out "${CMAKE_CURRENT_BINARY_DIR}"
        -I "${raft_proto_path}"
        --plugin=protoc-gen-grpc="${_GRPC_CPP_PLUGIN_EXECUTABLE}"
        "${raft_proto}"
      DEPENDS "${raft_proto}")

# Include generated *.pb.h files
include_directories("${CMAKE_CURRENT_BINARY_DIR}")

# Build proto files a separated static library
add_library(raft_grpc_proto STATIC
  ${raft_grpc_srcs}
  ${raft_grpc_hdrs}
  ${raft_proto_srcs}
  ${raft_proto_hdrs})

target_link_libraries(raft_grpc_proto
  absl::check
  ${_REFLECTION}
  ${_GRPC_GRPCPP}
  ${_PROTOBUF_LIBPROTOBUF})

add_custom_target(generate_proto_files ALL DEPENDS 
  ${CMAKE_CURRENT_BINARY_DIR}/raft.pb.cc 
  ${CMAKE_CURRENT_BINARY_DIR}/raft.pb.h
  ${CMAKE_CURRENT_BINARY_DIR}/raft.grpc.pb.cc 
  ${CMAKE_CURRENT_BINARY_DIR}/raft.grpc.pb.h
)

# Create the generated directory if it doesn't exist
add_custom_command(
      TARGET raft_grpc_proto POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/generated"
)

# Just for convenience...
# Custom command to create symbolic links for each generated file
add_custom_command(
      TARGET raft_grpc_proto POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E echo "Creating symbolic links for generated protobuf files"
      COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_BINARY_DIR}/create_symlinks.cmake"
)

# CMake script to handle the symlink creation
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/create_symlinks.cmake"
"file(GLOB proto_files \"${CMAKE_CURRENT_BINARY_DIR}/*.pb.cc\" \"${CMAKE_CURRENT_BINARY_DIR}/*.pb.h\")
foreach(proto_file \${proto_files})
    get_filename_component(filename \${proto_file} NAME)
    execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink \${proto_file} \"${CMAKE_SOURCE_DIR}/generated/\${filename}\")
endforeach()
")
