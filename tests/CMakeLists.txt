# tests/CMakeLists.txt

include(Catch)

add_executable(textfabric_tests
    test_basic.cpp
    test_merger.cpp
    fixture_helpers.cpp
)

target_include_directories(textfabric_tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src   # for internal headers like docx/merger.hpp
)

# Tests need access to internal symbols (DocxMerger, docx::Reader, etc.).
# When the main library is STATIC, link to it directly.
# When the main library is SHARED, link to the static twin `textfabric_internal`.
if(TEXTFABRIC_BUILD_SHARED)
    set(_tf_test_lib TextFabric::textfabric_internal)
else()
    set(_tf_test_lib TextFabric::textfabric)
endif()

target_link_libraries(textfabric_tests
    PRIVATE
        ${_tf_test_lib}
        Catch2::Catch2WithMain
        libzip::zip               # fixture_helpers creates DOCX archives
        pugixml::pugixml          # internal merger.hpp uses pugi::xml_document
)

# HAVE_* feature flags. In the SHARED build these already propagate through
# textfabric_internal (PUBLIC). In the STATIC build textfabric hides them
# (PRIVATE), so we repeat them on the test target so conditional test cases
# see the same view of available decoders.
if(TEXTFABRIC_HAVE_STB)
    target_compile_definitions(textfabric_tests PRIVATE TEXTFABRIC_HAVE_STB=1)
endif()
if(TARGET TIFF::TIFF)
    target_compile_definitions(textfabric_tests PRIVATE TEXTFABRIC_HAVE_TIFF=1)
endif()

# Copy test fixtures next to the binary so tests can find them
set(FIXTURE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
if(EXISTS "${FIXTURE_DIR}")
    add_custom_command(TARGET textfabric_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${FIXTURE_DIR}"
            "$<TARGET_FILE_DIR:textfabric_tests>/fixtures"
    )
endif()

# Auto-discover Catch2 test cases
catch_discover_tests(textfabric_tests
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:textfabric_tests>"
)

# End-to-end check: install TextFabric to a staging prefix and build an
# external project against it via find_package(). Covers the install/export
# surface (Targets.cmake, Config.cmake, generated export.h, transitive deps).
add_subdirectory(integration)
