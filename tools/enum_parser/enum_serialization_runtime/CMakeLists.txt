add_library(tools-enum_parser-enum_serialization_runtime)

target_link_libraries(tools-enum_parser-enum_serialization_runtime PUBLIC
  yutil
)

target_sources(tools-enum_parser-enum_serialization_runtime PRIVATE
  ${YDB_SDK_SOURCE_DIR}/tools/enum_parser/enum_serialization_runtime/dispatch_methods.cpp
  ${YDB_SDK_SOURCE_DIR}/tools/enum_parser/enum_serialization_runtime/enum_runtime.cpp
  ${YDB_SDK_SOURCE_DIR}/tools/enum_parser/enum_serialization_runtime/ordered_pairs.cpp
  ${YDB_SDK_SOURCE_DIR}/tools/enum_parser/enum_serialization_runtime/serialized_enum.cpp
)


