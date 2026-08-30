cmake_minimum_required(VERSION 3.20)

project(
    MiniMM
    VERSION 0.1.0
    DESCRIPTION "A small user-space model of Linux memory-management concepts"
    LANGUAGES C
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MINIMM_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(MINIMM_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(MINIMM_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(MINIMM_BUILD_TOOLS "Build the MiniMM server and client tools" ON)
option(MINIMM_BUILD_EXAMPLES "Build executable MiniMM learning examples" ON)
option(
    MINIMM_BUILD_SECURITY_BENCHMARKS
    "Enable optional evaluator reproduction tests"
    OFF
)

find_program(
    MINIMM_CLANG_FORMAT
    NAMES
        clang-format-21
        clang-format-20
        clang-format-19
        clang-format-18
        clang-format
    DOC "clang-format executable used by the format targets"
)

if(MINIMM_CLANG_FORMAT)
    file(GLOB_RECURSE _minimm_format_files CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/cmd/*.c"
        "${PROJECT_SOURCE_DIR}/cmd/*.h"
        "${PROJECT_SOURCE_DIR}/benchmarks/*.c"
        "${PROJECT_SOURCE_DIR}/benchmarks/*.h"
        "${PROJECT_SOURCE_DIR}/examples/*.c"
        "${PROJECT_SOURCE_DIR}/examples/*.h"
        "${PROJECT_SOURCE_DIR}/include/*.c"
        "${PROJECT_SOURCE_DIR}/include/*.h"
        "${PROJECT_SOURCE_DIR}/src/*.c"
        "${PROJECT_SOURCE_DIR}/src/*.h"
        "${PROJECT_SOURCE_DIR}/tests/*.c"
        "${PROJECT_SOURCE_DIR}/tests/*.h"
    )
    list(SORT _minimm_format_files)

    add_custom_target(format
        COMMAND
            "${MINIMM_CLANG_FORMAT}"
            --style=file
            -i
            ${_minimm_format_files}
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        COMMENT "Formatting MiniMM C sources in Linux kernel style"
        COMMAND_EXPAND_LISTS
        VERBATIM
    )
    add_custom_target(format-check
        COMMAND
            "${MINIMM_CLANG_FORMAT}"
            --style=file
            --dry-run
            --Werror
            ${_minimm_format_files}
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        COMMENT "Checking MiniMM C source formatting"
        COMMAND_EXPAND_LISTS
        VERBATIM
    )
else()
    message(STATUS "clang-format not found; format targets are unavailable")
endif()

if(MSVC)
    add_compile_options(/W4)
    if(MINIMM_WARNINGS_AS_ERRORS)
        add_compile_options(/WX)
    endif()
else()
    add_compile_options(
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Wformat=2
        -Wundef
        -Wstrict-prototypes
        -Wmissing-prototypes
        -Werror=implicit-function-declaration
    )
    if(MINIMM_WARNINGS_AS_ERRORS)
        add_compile_options(-Werror)
    endif()
endif()

set(_minimm_sanitizers "")
if(MINIMM_ENABLE_ASAN)
    list(APPEND _minimm_sanitizers address)
endif()
if(MINIMM_ENABLE_UBSAN)
    list(APPEND _minimm_sanitizers undefined)
endif()

if(_minimm_sanitizers)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "The selected compiler does not support this sanitizer profile")
    endif()

    list(JOIN _minimm_sanitizers "," _minimm_sanitizer_list)
    add_compile_options(
        -fno-omit-frame-pointer
        -fno-sanitize-recover=all
        "-fsanitize=${_minimm_sanitizer_list}"
    )
    add_link_options("-fsanitize=${_minimm_sanitizer_list}")
endif()

include(CTest)
find_package(Threads REQUIRED)

add_library(minimm STATIC
    src/access.c
    src/brk.c
    src/fault.c
    src/fault.h
    src/fork.c
    src/frame.c
    src/frame.h
    src/mapping.c
    src/mapping_backing.c
    src/mapping_backing.h
    src/memory_api.c
    src/memory_api.h
    src/mm.c
    src/note.c
    src/note.h
    src/page_table.c
    src/page_table.h
    src/rcu.c
    src/rcu.h
    src/space.c
    src/space.h
    src/snapshot.c
    src/stats.c
    src/stats.h
    src/tlb.c
    src/tlb.h
    src/vma_tree.c
    src/vma_tree.h
    src/internal.h
    include/minimm/minimm.h
)
add_library(MiniMM::minimm ALIAS minimm)

target_include_directories(minimm
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
)
target_compile_definitions(minimm PRIVATE
    MINIMM_VERSION_STRING="${PROJECT_VERSION}"
    _DEFAULT_SOURCE=1
    _POSIX_C_SOURCE=200809L
)
target_link_libraries(minimm PRIVATE Threads::Threads)

add_library(minimm_protocol STATIC
    src/protocol.c
    src/protocol.h
    include/minimm/protocol.h
)
add_library(MiniMM::protocol ALIAS minimm_protocol)
target_include_directories(minimm_protocol
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
)
target_compile_definitions(
    minimm_protocol
    PRIVATE _POSIX_C_SOURCE=200809L
)

add_library(minimm_service STATIC
    src/client.c
    src/mglru_reparent.c
    src/mglru_reparent.h
    src/mseal_merge.c
    src/mseal_merge.h
    src/page_remap.c
    src/page_remap.h
    src/private_preview.c
    src/private_preview.h
    src/server.c
    src/stack_expand.c
    src/stack_expand.h
    include/minimm/client.h
    include/minimm/server.h
)
add_library(MiniMM::service ALIAS minimm_service)
target_include_directories(minimm_service
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
)
target_compile_definitions(
    minimm_service
    PRIVATE _POSIX_C_SOURCE=200809L
)
target_link_libraries(minimm_service
    PUBLIC
        MiniMM::minimm
        MiniMM::protocol
    PRIVATE
        Threads::Threads
)

if(MINIMM_BUILD_TOOLS)
    include(GNUInstallDirs)

    add_executable(minimm_server_cli cmd/minimm_server_main.c)
    set_target_properties(minimm_server_cli PROPERTIES OUTPUT_NAME minimm-server)
    target_link_libraries(minimm_server_cli PRIVATE MiniMM::service)

    add_executable(minimm_client_cli cmd/minimm_client_main.c)
    set_target_properties(minimm_client_cli PROPERTIES OUTPUT_NAME minimm-client)
    target_link_libraries(minimm_client_cli PRIVATE MiniMM::service)

    install(
        TARGETS minimm_server_cli minimm_client_cli
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        COMPONENT Runtime
    )
endif()

if(MINIMM_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(MINIMM_BUILD_SECURITY_BENCHMARKS AND
   EXISTS "${PROJECT_SOURCE_DIR}/benchmarks/security/CMakeLists.txt")
    add_subdirectory(benchmarks/security)
elseif(MINIMM_BUILD_SECURITY_BENCHMARKS)
    message(STATUS "Optional evaluator sources are not present; skipping them")
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
