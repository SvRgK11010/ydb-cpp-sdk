PY23_TEST()

SUBSCRIBER(g:util-subscribers)

SRCDIR(src/util/generic)

PY_SRCS(
    NAMESPACE util.generic
    deque_ut.pyx
    hash_set_ut.pyx
    hash_ut.pyx
    list_ut.pyx
    map_ut.pyx
    ptr_ut.pyx
    string_ut.pyx
    vector_ut.pyx
)

TEST_SRCS(
    test_generic.py
)

END()
