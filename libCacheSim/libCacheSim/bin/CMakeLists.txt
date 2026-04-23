


add_subdirectory(cachesim)
# add_subdirectory(traceWriter)
add_subdirectory(distUtil)
add_subdirectory(traceUtils)
add_subdirectory(traceAnalyzer)

add_executable(flash flash/flash.cpp cachesim/cli_parser.c cachesim/sim.c cli_reader_utils.c)
target_link_libraries(flash ${ALL_MODULES} ${LIBS} ${CMAKE_THREAD_LIBS_INIT} utils)

if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/priv)
    message(STATUS "Found priv directory, building priv")
    add_subdirectory(priv)
endif()

