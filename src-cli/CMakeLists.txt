cmake_minimum_required(VERSION 3.12)

file(GLOB_RECURSE SatDump_cli_CPPS *.cpp)
if(SATDUMP_PACKAGE_PATH_MAP)
    satdump_package_examples(SatDump_cli_CPPS "src-cli/legacy/offline.cpp")
endif()

add_executable(satdump ${SatDump_cli_CPPS})
target_include_directories(satdump PUBLIC src-cli src-core)

# Link against the core
target_link_libraries(satdump PUBLIC satdump_core)

install(TARGETS satdump DESTINATION ${CMAKE_INSTALL_BINDIR})
