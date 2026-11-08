cmake_minimum_required(VERSION 3.20)

project(LedgerCore
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# PostgreSQL installation paths
set(PostgreSQL_INCLUDE_DIR "C:/Program Files/PostgreSQL/18/include")
set(PostgreSQL_LIBRARY "C:/Program Files/PostgreSQL/18/lib/libpq.lib")

find_package(PostgreSQL REQUIRED)
# Optional REST API build
option(LEDGERCORE_BUILD_API "Build LedgerCore REST API" OFF)

if(LEDGERCORE_BUILD_API)
    find_package(Drogon CONFIG REQUIRED)
endif()

add_executable(ledgercore
    src/main.cpp
)

target_link_libraries(ledgercore
    PRIVATE
    PostgreSQL::PostgreSQL
)

# Explicit experiment target: uses the shared transfer implementation,
# with test-only synchronization after ordered account locks.
find_package(Threads REQUIRED)
add_executable(ledgercore_concurrency EXCLUDE_FROM_ALL
    tests/validate_phase5.cpp
    src/main.cpp
)
target_compile_definitions(ledgercore_concurrency PRIVATE
    LEDGERCORE_NO_MAIN LEDGERCORE_CONCURRENCY_TEST
)
target_link_libraries(ledgercore_concurrency PRIVATE
    PostgreSQL::PostgreSQL Threads::Threads
)
# LedgerCore REST API
if(LEDGERCORE_BUILD_API)

    add_executable(ledgercore_api
        src/api.cpp
    )

    target_link_libraries(ledgercore_api
        PRIVATE
        Drogon::Drogon
    )

endif()
