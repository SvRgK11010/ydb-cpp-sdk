add_library(impl-ydb_internal-session_pool)

target_link_libraries(impl-ydb_internal-session_pool PUBLIC
  yutil
  cpp-threading-future
  api-protos
  client-impl-ydb_endpoints
  client-ydb_types-operation
  public-issue-protos
)

target_sources(impl-ydb_internal-session_pool PRIVATE
  session_pool.cpp
)
