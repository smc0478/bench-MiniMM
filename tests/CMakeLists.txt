add_executable(minimm_smoke test_smoke.c)
target_link_libraries(minimm_smoke PRIVATE MiniMM::minimm)

add_test(NAME minimm.smoke COMMAND minimm_smoke)

add_executable(minimm_frame_test test_frame.c)
target_include_directories(minimm_frame_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(minimm_frame_test PRIVATE _POSIX_C_SOURCE=200809L)
target_link_libraries(minimm_frame_test PRIVATE MiniMM::minimm Threads::Threads)

add_test(NAME minimm.frame COMMAND minimm_frame_test)

add_executable(minimm_translation_test test_translation.c)
target_include_directories(
    minimm_translation_test
    PRIVATE ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(minimm_translation_test PRIVATE MiniMM::minimm)

add_test(NAME minimm.translation COMMAND minimm_translation_test)

add_executable(minimm_page_table_clone_test test_page_table_clone.c)
target_include_directories(
    minimm_page_table_clone_test
    PRIVATE ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(minimm_page_table_clone_test PRIVATE MiniMM::minimm)

add_test(NAME minimm.page_table_clone COMMAND minimm_page_table_clone_test)

add_executable(minimm_rcu_test test_rcu.c)
target_include_directories(minimm_rcu_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(minimm_rcu_test PRIVATE MiniMM::minimm Threads::Threads)

add_test(NAME minimm.rcu COMMAND minimm_rcu_test)

add_executable(minimm_vma_tree_test test_vma_tree.c)
target_include_directories(minimm_vma_tree_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(minimm_vma_tree_test PRIVATE MiniMM::minimm)

add_test(NAME minimm.vma_tree COMMAND minimm_vma_tree_test)

add_executable(minimm_space_test test_space.c)
target_link_libraries(minimm_space_test PRIVATE MiniMM::minimm)

add_test(NAME minimm.space COMMAND minimm_space_test)

add_executable(minimm_fork_test test_fork.c)
target_link_libraries(minimm_fork_test PRIVATE MiniMM::minimm)

add_test(NAME minimm.fork COMMAND minimm_fork_test)

add_executable(minimm_brk_test test_brk.c)
target_link_libraries(minimm_brk_test PRIVATE MiniMM::minimm)

add_test(NAME minimm.brk COMMAND minimm_brk_test)

add_executable(minimm_mapping_test test_mapping.c)
target_link_libraries(minimm_mapping_test PRIVATE MiniMM::minimm Threads::Threads)

add_test(NAME minimm.mapping COMMAND minimm_mapping_test)
set_tests_properties(minimm.mapping PROPERTIES TIMEOUT 20)

add_executable(minimm_mapping_copy_test test_mapping_copy.c)
target_link_libraries(minimm_mapping_copy_test PRIVATE MiniMM::minimm)

add_test(NAME minimm.mapping_copy COMMAND minimm_mapping_copy_test)
set_tests_properties(minimm.mapping_copy PROPERTIES TIMEOUT 15)

add_executable(minimm_populate_test test_populate.c)
target_link_libraries(minimm_populate_test PRIVATE MiniMM::minimm)

add_test(NAME minimm.populate COMMAND minimm_populate_test)

add_executable(minimm_mremap_test test_mremap.c)
target_link_libraries(minimm_mremap_test PRIVATE MiniMM::minimm)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_definitions(minimm_mremap_test PRIVATE MINIMM_TEST_WRAP_CALLOC=1)
    target_link_options(minimm_mremap_test PRIVATE "LINKER:--wrap=calloc")
endif()

add_test(NAME minimm.mremap COMMAND minimm_mremap_test)

add_executable(minimm_fault_test test_fault.c)
target_include_directories(minimm_fault_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(minimm_fault_test PRIVATE MiniMM::minimm)

add_test(NAME minimm.fault COMMAND minimm_fault_test)

add_executable(minimm_fault_trace_test test_fault_trace.c)
target_link_libraries(minimm_fault_trace_test PRIVATE MiniMM::minimm)

add_test(NAME minimm.fault_trace COMMAND minimm_fault_trace_test)

add_executable(minimm_snapshot_test test_snapshot.c)
target_link_libraries(minimm_snapshot_test PRIVATE MiniMM::minimm Threads::Threads)

add_test(NAME minimm.snapshot COMMAND minimm_snapshot_test)
set_tests_properties(minimm.snapshot PROPERTIES TIMEOUT 15)

add_executable(minimm_reclaim_test test_reclaim.c)
target_link_libraries(minimm_reclaim_test PRIVATE MiniMM::minimm Threads::Threads)

add_test(NAME minimm.reclaim COMMAND minimm_reclaim_test)

add_executable(minimm_access_test test_access.c)
target_link_libraries(minimm_access_test PRIVATE MiniMM::minimm Threads::Threads)

add_test(NAME minimm.access COMMAND minimm_access_test)

add_executable(minimm_note_test test_note.c)
target_include_directories(minimm_note_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(minimm_note_test PRIVATE _POSIX_C_SOURCE=200809L)
target_link_libraries(minimm_note_test PRIVATE MiniMM::minimm Threads::Threads)

add_test(NAME minimm.note COMMAND minimm_note_test)

add_executable(minimm_note_mapping_test test_note_mapping.c)
target_link_libraries(minimm_note_mapping_test PRIVATE MiniMM::minimm)

add_test(NAME minimm.note_mapping COMMAND minimm_note_mapping_test)

add_executable(minimm_stats_test test_stats.c)
target_include_directories(minimm_stats_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(minimm_stats_test PRIVATE MiniMM::minimm)

add_test(NAME minimm.stats COMMAND minimm_stats_test)

add_executable(minimm_memory_api_test test_memory_api.c)
target_include_directories(
    minimm_memory_api_test
    PRIVATE ${PROJECT_SOURCE_DIR}/src
)
target_compile_definitions(
    minimm_memory_api_test
    PRIVATE _POSIX_C_SOURCE=200809L
)
target_link_libraries(minimm_memory_api_test PRIVATE MiniMM::minimm)

add_test(NAME minimm.memory_api COMMAND minimm_memory_api_test)
set_tests_properties(minimm.memory_api PROPERTIES TIMEOUT 15)

add_executable(minimm_protocol_test test_protocol.c)
target_include_directories(
    minimm_protocol_test
    PRIVATE ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(minimm_protocol_test PRIVATE MiniMM::protocol)

add_test(NAME minimm.protocol COMMAND minimm_protocol_test)

add_executable(minimm_service_test test_service.c)
target_include_directories(minimm_service_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(minimm_service_test PRIVATE MiniMM::service)

add_test(NAME minimm.service COMMAND minimm_service_test)
set_tests_properties(minimm.service PROPERTIES TIMEOUT 15)

add_executable(minimm_server_protocol_test test_server_protocol.c)
target_include_directories(
    minimm_server_protocol_test
    PRIVATE ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(minimm_server_protocol_test PRIVATE MiniMM::service)

add_test(NAME minimm.server_protocol COMMAND minimm_server_protocol_test)
set_tests_properties(minimm.server_protocol PROPERTIES TIMEOUT 15)

if(MINIMM_BUILD_TOOLS AND UNIX)
    add_executable(minimm_cli_test test_cli.c)
    add_dependencies(minimm_cli_test minimm_client_cli minimm_server_cli)

    add_executable(minimm_server_app_test test_server_app.c)
    add_dependencies(minimm_server_app_test minimm_client_cli minimm_server_cli)

    add_test(
        NAME minimm.cli
        COMMAND
            minimm_cli_test
            $<TARGET_FILE:minimm_client_cli>
            $<TARGET_FILE:minimm_server_cli>
    )
    set_tests_properties(minimm.cli PROPERTIES TIMEOUT 10)

    add_test(
        NAME minimm.server_app
        COMMAND
            minimm_server_app_test
            $<TARGET_FILE:minimm_client_cli>
            $<TARGET_FILE:minimm_server_cli>
    )
    set_tests_properties(minimm.server_app PROPERTIES TIMEOUT 90)
endif()
