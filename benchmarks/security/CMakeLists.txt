find_package(Python3 REQUIRED COMPONENTS Interpreter)

if(NOT TARGET minimm_server_cli OR NOT TARGET minimm_client_cli)
    message(FATAL_ERROR
        "MINIMM_BUILD_SECURITY_BENCHMARKS requires MINIMM_BUILD_TOOLS=ON"
    )
endif()

set(
    _minimm_race_001_reproducer
    "${CMAKE_CURRENT_SOURCE_DIR}/cases/MM-RACE-001/reproduce.py"
)
set(
    _minimm_race_001_command
    "${Python3_EXECUTABLE}"
    "${_minimm_race_001_reproducer}"
    --server "$<TARGET_FILE:minimm_server_cli>"
    --client "$<TARGET_FILE:minimm_client_cli>"
)
set(
    _minimm_uaf_001_reproducer
    "${CMAKE_CURRENT_SOURCE_DIR}/cases/MM-UAF-001/reproduce.py"
)
set(
    _minimm_uaf_001_command
    "${Python3_EXECUTABLE}"
    "${_minimm_uaf_001_reproducer}"
    --server "$<TARGET_FILE:minimm_server_cli>"
    --client "$<TARGET_FILE:minimm_client_cli>"
)
set(
    _minimm_logic_001_reproducer
    "${CMAKE_CURRENT_SOURCE_DIR}/cases/MM-LOGIC-001/reproduce.py"
)
set(
    _minimm_logic_001_command
    "${Python3_EXECUTABLE}"
    "${_minimm_logic_001_reproducer}"
    --server "$<TARGET_FILE:minimm_server_cli>"
    --client "$<TARGET_FILE:minimm_client_cli>"
)
set(
    _minimm_mseal_001_reproducer
    "${CMAKE_CURRENT_SOURCE_DIR}/cases/MM-MSEAL-001/reproduce.py"
)
set(
    _minimm_mseal_001_command
    "${Python3_EXECUTABLE}"
    "${_minimm_mseal_001_reproducer}"
    --server "$<TARGET_FILE:minimm_server_cli>"
    --client "$<TARGET_FILE:minimm_client_cli>"
)
set(
    _minimm_mglru_001_reproducer
    "${CMAKE_CURRENT_SOURCE_DIR}/cases/MM-MGLRU-001/reproduce.py"
)
set(
    _minimm_mglru_001_command
    "${Python3_EXECUTABLE}"
    "${_minimm_mglru_001_reproducer}"
    --server "$<TARGET_FILE:minimm_server_cli>"
    --client "$<TARGET_FILE:minimm_client_cli>"
)
set(
    _minimm_rmap_001_reproducer
    "${CMAKE_CURRENT_SOURCE_DIR}/cases/MM-RMAP-001/reproduce.py"
)
set(
    _minimm_rmap_001_command
    "${Python3_EXECUTABLE}"
    "${_minimm_rmap_001_reproducer}"
    --server "$<TARGET_FILE:minimm_server_cli>"
    --client "$<TARGET_FILE:minimm_client_cli>"
)
set(
    _minimm_uffd_001_reproducer
    "${CMAKE_CURRENT_SOURCE_DIR}/cases/MM-UFFD-001/reproduce.py"
)
set(
    _minimm_uffd_001_command
    "${Python3_EXECUTABLE}"
    "${_minimm_uffd_001_reproducer}"
    --server "$<TARGET_FILE:minimm_server_cli>"
    --client "$<TARGET_FILE:minimm_client_cli>"
)
set(
    _minimm_hugetlb_001_reproducer
    "${CMAKE_CURRENT_SOURCE_DIR}/cases/MM-HUGETLB-001/reproduce.py"
)
set(
    _minimm_hugetlb_001_command
    "${Python3_EXECUTABLE}"
    "${_minimm_hugetlb_001_reproducer}"
    --server "$<TARGET_FILE:minimm_server_cli>"
    --client "$<TARGET_FILE:minimm_client_cli>"
)
set(
    _minimm_percpu_001_reproducer
    "${CMAKE_CURRENT_SOURCE_DIR}/cases/MM-PERCPU-001/reproduce.py"
)
set(
    _minimm_percpu_001_command
    "${Python3_EXECUTABLE}"
    "${_minimm_percpu_001_reproducer}"
    --server "$<TARGET_FILE:minimm_server_cli>"
    --client "$<TARGET_FILE:minimm_client_cli>"
)

add_custom_target(
    minimm-security-benchmark-race-001
    COMMAND ${_minimm_race_001_command}
    DEPENDS minimm_server_cli minimm_client_cli
    COMMENT "Reproducing MM-RACE-001 through the loopback service"
    VERBATIM
)

add_custom_target(
    minimm-security-benchmark-uaf-001
    COMMAND ${_minimm_uaf_001_command}
    DEPENDS minimm_server_cli minimm_client_cli
    COMMENT "Reproducing MM-UAF-001 through the loopback service"
    VERBATIM
)

add_custom_target(
    minimm-security-benchmark-logic-001
    COMMAND ${_minimm_logic_001_command}
    DEPENDS minimm_server_cli minimm_client_cli
    COMMENT "Reproducing MM-LOGIC-001 through the loopback service"
    VERBATIM
)

add_custom_target(
    minimm-security-benchmark-mseal-001
    COMMAND ${_minimm_mseal_001_command}
    DEPENDS minimm_server_cli minimm_client_cli
    COMMENT "Reproducing MM-MSEAL-001 through the loopback service"
    VERBATIM
)

add_custom_target(
    minimm-security-benchmark-mglru-001
    COMMAND ${_minimm_mglru_001_command}
    DEPENDS minimm_server_cli minimm_client_cli
    COMMENT "Reproducing MM-MGLRU-001 through the loopback service"
    VERBATIM
)

add_custom_target(
    minimm-security-benchmark-rmap-001
    COMMAND ${_minimm_rmap_001_command}
    DEPENDS minimm_server_cli minimm_client_cli
    COMMENT "Reproducing MM-RMAP-001 through the loopback service"
    VERBATIM
)

add_custom_target(
    minimm-security-benchmark-uffd-001
    COMMAND ${_minimm_uffd_001_command}
    DEPENDS minimm_server_cli minimm_client_cli
    COMMENT "Reproducing MM-UFFD-001 through the loopback service"
    VERBATIM
)

add_custom_target(
    minimm-security-benchmark-hugetlb-001
    COMMAND ${_minimm_hugetlb_001_command}
    DEPENDS minimm_server_cli minimm_client_cli
    COMMENT "Reproducing MM-HUGETLB-001 through the loopback service"
    VERBATIM
)

add_custom_target(
    minimm-security-benchmark-percpu-001
    COMMAND ${_minimm_percpu_001_command}
    DEPENDS minimm_server_cli minimm_client_cli
    COMMENT "Reproducing MM-PERCPU-001 through the loopback service"
    VERBATIM
)

if(BUILD_TESTING)
    add_test(
        NAME minimm.benchmark.race_001.reproduce
        COMMAND ${_minimm_race_001_command}
    )
    set_tests_properties(
        minimm.benchmark.race_001.reproduce
        PROPERTIES
            LABELS "benchmark;security;race-condition"
            RUN_SERIAL TRUE
            TIMEOUT 20
    )
    add_test(
        NAME minimm.benchmark.uaf_001.reproduce
        COMMAND ${_minimm_uaf_001_command}
    )
    set_tests_properties(
        minimm.benchmark.uaf_001.reproduce
        PROPERTIES
            LABELS "benchmark;security;use-after-free"
            RUN_SERIAL TRUE
            TIMEOUT 20
    )
    add_test(
        NAME minimm.benchmark.logic_001.reproduce
        COMMAND ${_minimm_logic_001_command}
    )
    set_tests_properties(
        minimm.benchmark.logic_001.reproduce
        PROPERTIES
            LABELS "benchmark;security;logic-error"
            RUN_SERIAL TRUE
            TIMEOUT 35
    )
    add_test(
        NAME minimm.benchmark.mseal_001.reproduce
        COMMAND ${_minimm_mseal_001_command}
    )
    set_tests_properties(
        minimm.benchmark.mseal_001.reproduce
        PROPERTIES
            LABELS "benchmark;security;logic-error;mseal"
            RUN_SERIAL TRUE
            TIMEOUT 35
    )
    add_test(
        NAME minimm.benchmark.mglru_001.reproduce
        COMMAND ${_minimm_mglru_001_command}
    )
    set_tests_properties(
        minimm.benchmark.mglru_001.reproduce
        PROPERTIES
            LABELS "benchmark;security;race-condition;mglru"
            RUN_SERIAL TRUE
            TIMEOUT 35
    )
    add_test(
        NAME minimm.benchmark.rmap_001.reproduce
        COMMAND ${_minimm_rmap_001_command}
    )
    set_tests_properties(
        minimm.benchmark.rmap_001.reproduce
        PROPERTIES
            LABELS "benchmark;security;out-of-bounds;rmap"
            RUN_SERIAL TRUE
            TIMEOUT 35
    )
    add_test(
        NAME minimm.benchmark.uffd_001.reproduce
        COMMAND ${_minimm_uffd_001_command}
    )
    set_tests_properties(
        minimm.benchmark.uffd_001.reproduce
        PROPERTIES
            LABELS "benchmark;security;race-condition;userfaultfd"
            RUN_SERIAL TRUE
            TIMEOUT 35
    )
    add_test(
        NAME minimm.benchmark.hugetlb_001.reproduce
        COMMAND ${_minimm_hugetlb_001_command}
    )
    set_tests_properties(
        minimm.benchmark.hugetlb_001.reproduce
        PROPERTIES
            LABELS "benchmark;security;accounting;hugetlb"
            RUN_SERIAL TRUE
            TIMEOUT 35
    )
    add_test(
        NAME minimm.benchmark.percpu_001.reproduce
        COMMAND ${_minimm_percpu_001_command}
    )
    set_tests_properties(
        minimm.benchmark.percpu_001.reproduce
        PROPERTIES
            LABELS "benchmark;security;out-of-bounds;percpu"
            RUN_SERIAL TRUE
            TIMEOUT 35
    )
endif()
