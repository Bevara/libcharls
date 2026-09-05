cmake_minimum_required(VERSION 3.7)
project(Filters)

include(filters.cmake)

add_definitions(-fpic)

SET(LIBCHARLS_SRC
        ${CMAKE_CURRENT_SOURCE_DIR}/dec_jls.c
)

SET(LIBCHARLS_LIBS
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/libcharls.a
)

SET(LIBCHARLS_INC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_filter(libcharls
        "${LIBCHARLS_SRC}"
        "${LIBCHARLS_LIBS}"
        []
        ""
        "${LIBCHARLS_INC}"
        ""
        "1")
