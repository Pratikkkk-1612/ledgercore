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

add_executable(ledgercore
    src/main.cpp
)

target_link_libraries(ledgercore
    PRIVATE
    PostgreSQL::PostgreSQL
)