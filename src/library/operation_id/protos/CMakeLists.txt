add_library(lib-operation_id-protos)
target_link_libraries(lib-operation_id-protos PUBLIC
  yutil
  protobuf::libprotobuf
)
target_proto_messages(lib-operation_id-protos PRIVATE
  ${YDB_SDK_SOURCE_DIR}/src/library/operation_id/protos/operation_id.proto
)
target_proto_addincls(lib-operation_id-protos
  ./
  ${YDB_SDK_SOURCE_DIR}
  ${YDB_SDK_BINARY_DIR}
)
target_proto_outs(lib-operation_id-protos
  --cpp_out=${YDB_SDK_BINARY_DIR}/
)

