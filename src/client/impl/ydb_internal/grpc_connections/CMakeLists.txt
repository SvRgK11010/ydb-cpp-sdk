add_library(impl-ydb_internal-grpc_connections)

target_link_libraries(impl-ydb_internal-grpc_connections PUBLIC
  yutil
  api-grpc
  api-protos
  impl-ydb_internal-db_driver_state
  impl-ydb_internal-plain_status
  impl-ydb_internal-thread_pool
  client-impl-ydb_stats
  cpp-client-resources
  client-ydb_types-exceptions
)

target_sources(impl-ydb_internal-grpc_connections PRIVATE
  actions.cpp
  grpc_connections.cpp
)
