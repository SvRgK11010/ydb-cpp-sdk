add_library(cpp-client-ydb_datastreams)

target_link_libraries(cpp-client-ydb_datastreams PUBLIC
  yutil
  library-grpc-client
  cpp-string_utils-url
  api-grpc-draft
  library-operation_id
  impl-ydb_internal-make_request
  cpp-client-ydb_driver
)

target_sources(cpp-client-ydb_datastreams PRIVATE
  datastreams.cpp
)

add_library(YDB-CPP-SDK::Datastreams ALIAS cpp-client-ydb_datastreams)
