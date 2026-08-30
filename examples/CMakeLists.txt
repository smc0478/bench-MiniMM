add_executable(minimm_mm_tour mm_tour.c)
set_target_properties(minimm_mm_tour PROPERTIES OUTPUT_NAME minimm-mm-tour)
target_link_libraries(minimm_mm_tour PRIVATE MiniMM::minimm)

if(BUILD_TESTING)
    add_test(NAME minimm.education.fault COMMAND minimm_mm_tour fault)
    add_test(NAME minimm.education.cow COMMAND minimm_mm_tour cow)
    add_test(NAME minimm.education.reclaim COMMAND minimm_mm_tour reclaim)
    add_test(NAME minimm.education.working_set COMMAND minimm_mm_tour working-set)
    add_test(NAME minimm.education.inspect COMMAND minimm_mm_tour inspect)
    set_tests_properties(
        minimm.education.fault
        minimm.education.cow
        minimm.education.reclaim
        minimm.education.working_set
        minimm.education.inspect
        PROPERTIES LABELS education
    )
endif()

include(GNUInstallDirs)
install(
    TARGETS minimm_mm_tour
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    COMPONENT Runtime
)
