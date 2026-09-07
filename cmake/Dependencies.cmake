# cmake/Dependencies.cmake
# Resolves all third-party dependencies.
#
# Strategy (in order):
#   1. find_package() — satisfied by vcpkg toolchain (Windows/macOS)
#      or Nix / system packages (Linux).
#   2. FetchContent fallback — only when TEXTFABRIC_USE_FETCHCONTENT=ON
#      and the package was not found via find_package.
#
# Required packages
#   - pugixml          XML parsing   (vcpkg: pugixml    | nixpkgs: pugixml)
#   - libzip           ZIP archives  (vcpkg: libzip     | nixpkgs: libzip)
#   - nlohmann_json    JSON          (vcpkg: nlohmann-json | nixpkgs: nlohmann_json)
#   - fmt              Formatting    (vcpkg: fmt        | nixpkgs: fmt)
#   - inja             Templates     (vcpkg: inja       | nixpkgs: — use FetchContent)
#
# Optional (auto-detected, feature-flagged into the library):
#   - stb_image /      JPEG + BMP decode, PNG encode. Single-header, public
#     stb_image_write  domain. Pulled via FetchContent from nothings/stb when
#                      not found through find_package. If missing, setImage
#                      falls back to PNG-only (NotImplemented for JPG/BMP).
#                      Adds TEXTFABRIC_HAVE_STB compile definition.
#   - TIFF (libtiff)   TIFF decode (vcpkg: tiff | nixpkgs: libtiff). If not
#                      present, setImage throws NotImplemented for TIFF.
#                      Adds TEXTFABRIC_HAVE_TIFF compile definition.
#
# Optional (tests only):
#   - Catch2 3.x       (vcpkg: catch2 | nixpkgs: catch2_3)

include(FetchContent)

# When building textfabric as SHARED on Linux/macOS, all static deps we pull
# into it must be compiled with -fPIC. Setting this globally before
# FetchContent_MakeAvailable() ensures pugixml/libzip/fmt/etc. pick it up.
if(BUILD_SHARED_LIBS OR TEXTFABRIC_BUILD_SHARED)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

# ── Helper: print dep status ────────────────────────────────────────────────
macro(_tf_dep_status name found_var)
    if(${found_var})
        message(STATUS "  [dep] ${name}: found via find_package")
    else()
        message(STATUS "  [dep] ${name}: NOT found — will use FetchContent")
    endif()
endmacro()

# ── pugixml ─────────────────────────────────────────────────────────────────
find_package(pugixml CONFIG QUIET)
_tf_dep_status("pugixml" pugixml_FOUND)

if(NOT pugixml_FOUND)
    if(TEXTFABRIC_USE_FETCHCONTENT)
        FetchContent_Declare(pugixml
            GIT_REPOSITORY https://github.com/zeux/pugixml.git
            GIT_TAG        v1.14
            GIT_SHALLOW    TRUE
        )
        FetchContent_MakeAvailable(pugixml)
    else()
        message(WARNING "pugixml not found — stubs will compile without it. Install via vcpkg/Nix or set TEXTFABRIC_USE_FETCHCONTENT=ON")
    endif()
endif()

# ── libzip ───────────────────────────────────────────────────────────────────
# vcpkg exports as "libzip::zip"; pkg-config may use "libzip"
find_package(libzip CONFIG QUIET)
if(NOT libzip_FOUND)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBZIP IMPORTED_TARGET libzip)
        if(LIBZIP_FOUND)
            # Provide a uniform alias so CMakeLists.txt doesn't need ifdefs
            add_library(libzip::zip ALIAS PkgConfig::LIBZIP)
            set(libzip_FOUND TRUE)
        endif()
    endif()
endif()
_tf_dep_status("libzip" libzip_FOUND)

if(NOT libzip_FOUND)
    if(TEXTFABRIC_USE_FETCHCONTENT)
        FetchContent_Declare(libzip
            GIT_REPOSITORY https://github.com/nih-at/libzip.git
            GIT_TAG        v1.10.1
            GIT_SHALLOW    TRUE
        )
        set(ENABLE_COMMONCRYPTO OFF CACHE BOOL "" FORCE)
        set(ENABLE_GNUTLS       OFF CACHE BOOL "" FORCE)
        set(ENABLE_MBEDTLS      OFF CACHE BOOL "" FORCE)
        set(ENABLE_OPENSSL      ON  CACHE BOOL "" FORCE)
        set(BUILD_TOOLS         OFF CACHE BOOL "" FORCE)
        set(BUILD_REGRESS       OFF CACHE BOOL "" FORCE)
        set(BUILD_EXAMPLES      OFF CACHE BOOL "" FORCE)
        set(BUILD_DOC           OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(libzip)
    else()
        message(WARNING "libzip not found — stubs will compile without it.")
    endif()
endif()

# ── nlohmann_json ────────────────────────────────────────────────────────────
find_package(nlohmann_json CONFIG QUIET)
_tf_dep_status("nlohmann_json" nlohmann_json_FOUND)

if(NOT nlohmann_json_FOUND)
    if(TEXTFABRIC_USE_FETCHCONTENT)
        FetchContent_Declare(nlohmann_json
            GIT_REPOSITORY https://github.com/nlohmann/json.git
            GIT_TAG        v3.11.3
            GIT_SHALLOW    TRUE
        )
        set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(nlohmann_json)
    else()
        message(WARNING "nlohmann_json not found — stubs will compile without it.")
    endif()
endif()

# ── fmt ──────────────────────────────────────────────────────────────────────
find_package(fmt CONFIG QUIET)
_tf_dep_status("fmt" fmt_FOUND)

if(NOT fmt_FOUND)
    if(TEXTFABRIC_USE_FETCHCONTENT)
        FetchContent_Declare(fmt
            GIT_REPOSITORY https://github.com/fmtlib/fmt.git
            GIT_TAG        10.2.1
            GIT_SHALLOW    TRUE
        )
        FetchContent_MakeAvailable(fmt)
    else()
        message(WARNING "fmt not found — stubs will compile without it.")
    endif()
endif()

# ── inja ─────────────────────────────────────────────────────────────────────
# inja is in vcpkg; not packaged in nixpkgs → always try FetchContent as fallback.
find_package(inja CONFIG QUIET)
_tf_dep_status("inja" inja_FOUND)

if(NOT inja_FOUND)
    if(TEXTFABRIC_USE_FETCHCONTENT)
        FetchContent_Declare(inja
            GIT_REPOSITORY https://github.com/pantor/inja.git
            GIT_TAG        v3.4.0
            GIT_SHALLOW    TRUE
        )
        set(INJA_BUILD_TESTS      OFF CACHE BOOL "" FORCE)
        set(INJA_EXPORT           OFF CACHE BOOL "" FORCE)
        set(BUILD_BENCHMARK       OFF CACHE BOOL "" FORCE)
        set(INJA_USE_EMBEDDED_JSON OFF CACHE BOOL "" FORCE)  # use our nlohmann_json, not inja's bundled copy
        FetchContent_MakeAvailable(inja)
    else()
        message(WARNING "inja not found — stubs will compile without it.")
    endif()
endif()

# ── stb (Stage 5 Phase 2: JPEG + BMP decode, PNG encode) ────────────────────
# Single-header public domain library from https://github.com/nothings/stb.
# vcpkg exposes it as `Stb::Stb` via find_package(Stb CONFIG); nix has `stb`
# but without CMake config — so we FetchContent as a fallback.
set(_tf_stb_include "")
find_package(Stb QUIET)
if(Stb_FOUND AND DEFINED Stb_INCLUDE_DIR)
    set(_tf_stb_include "${Stb_INCLUDE_DIR}")
    message(STATUS "  [dep] stb: found via find_package (${Stb_INCLUDE_DIR})")
elseif(TEXTFABRIC_USE_FETCHCONTENT)
    message(STATUS "  [dep] stb: NOT found — fetching nothings/stb")
    FetchContent_Declare(stb
        GIT_REPOSITORY https://github.com/nothings/stb.git
        GIT_TAG        master
        GIT_SHALLOW    TRUE
    )
    FetchContent_GetProperties(stb)
    if(NOT stb_POPULATED)
        FetchContent_Populate(stb)
    endif()
    if(EXISTS "${stb_SOURCE_DIR}/stb_image.h")
        set(_tf_stb_include "${stb_SOURCE_DIR}")
    else()
        message(WARNING "  [dep] stb: FetchContent completed but stb_image.h not found — JPEG/BMP support disabled")
    endif()
else()
    message(STATUS "  [dep] stb: NOT found and FetchContent disabled — JPEG/BMP support disabled")
endif()

if(_tf_stb_include)
    # Expose as plain cache vars rather than an INTERFACE library, so the
    # install(EXPORT ...) set doesn't need to include an intermediate target
    # that points at a build-tree-only include dir.
    set(TEXTFABRIC_STB_INCLUDE_DIR "${_tf_stb_include}" CACHE INTERNAL "")
    set(TEXTFABRIC_HAVE_STB        ON                   CACHE INTERNAL "")
else()
    set(TEXTFABRIC_STB_INCLUDE_DIR "" CACHE INTERNAL "")
    set(TEXTFABRIC_HAVE_STB        OFF CACHE INTERNAL "")
endif()

# ── libtiff (Stage 5 Phase 2: TIFF decode) ──────────────────────────────────
find_package(TIFF QUIET)
if(TIFF_FOUND)
    message(STATUS "  [dep] TIFF: found via find_package")
    set(TEXTFABRIC_HAVE_TIFF ON  CACHE INTERNAL "TIFF decoder available")
else()
    message(STATUS "  [dep] TIFF: NOT found — TIFF support disabled")
    set(TEXTFABRIC_HAVE_TIFF OFF CACHE INTERNAL "TIFF decoder available")
endif()

# ── Catch2 (tests only) ──────────────────────────────────────────────────────
if(TEXTFABRIC_BUILD_TESTS)
    find_package(Catch2 3 CONFIG QUIET)
    _tf_dep_status("Catch2 v3" Catch2_FOUND)

    if(NOT Catch2_FOUND)
        if(TEXTFABRIC_USE_FETCHCONTENT)
            FetchContent_Declare(Catch2
                GIT_REPOSITORY https://github.com/catchorg/Catch2.git
                GIT_TAG        v3.5.3
                GIT_SHALLOW    TRUE
            )
            FetchContent_MakeAvailable(Catch2)
            list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
        else()
            message(FATAL_ERROR "Catch2 v3 not found. Set TEXTFABRIC_USE_FETCHCONTENT=ON to auto-download.")
        endif()
    endif()
endif()
