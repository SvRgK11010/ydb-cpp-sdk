LIBRARY()

OWNER(
    dcherednik
    g:kikimr
)

SRCS(
    handlers.cpp
)

PEERDIR(
    ydb/public/sdk/cpp/client/ydb_types/exceptions 
)

END()
