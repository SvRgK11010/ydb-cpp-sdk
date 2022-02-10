UNITTEST_FOR(ydb/public/lib/operation_id) 

OWNER(
    dcherednik
    g:kikimr
)

FORK_SUBTESTS()

IF (SANITIZER_TYPE OR WITH_VALGRIND)
    SIZE(MEDIUM)
ENDIF()

SRCS(
    operation_id_ut.cpp
)

PEERDIR(
    library/cpp/testing/unittest
)

END()
