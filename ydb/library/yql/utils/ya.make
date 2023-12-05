LIBRARY()

SRCS(
    bind_in_range.cpp
    bind_in_range.h
    cast.h
    debug_info.cpp
    debug_info.h
    future_action.cpp
    future_action.h
    hash.cpp
    hash.h
    md5_stream.cpp
    md5_stream.h
    method_index.cpp
    method_index.h
    multi_resource_lock.cpp
    multi_resource_lock.h
    parse_double.cpp
    parse_double.h
    proc_alive.cpp
    proc_alive.h
    rand_guid.cpp
    rand_guid.h
    resetable_setting.h
    retry.cpp
    retry.h
    rope_over_buffer.cpp
    rope_over_buffer.h
    sort.cpp
    sort.h
    swap_bytes.cpp
    swap_bytes.h
    url_builder.cpp
    utf8.cpp
    yql_panic.cpp
    yql_panic.h
    yql_paths.cpp
    yql_paths.h
)

PEERDIR(
    ydb/library/actors/util
    library/cpp/digest/md5
    library/cpp/messagebus
    library/cpp/string_utils/quote
    library/cpp/threading/future
    library/cpp/deprecated/atomic
    contrib/libs/miniselect
)

END()

RECURSE(
    actor_log
    actors
    actor_system
    backtrace
    bindings
    failure_injector
    fetch
    log
    plan
    simd
    sys
    test_http_server
    threading
)

RECURSE_FOR_TESTS(
    ut
)
