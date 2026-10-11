# tests/deep.cmake -- the deep test campaign's targets: the properties and the
# fuzz targets. The framework's alone: it lives under tests/, which `rmp new`
# does not copy, and CMakeLists.txt includes it with OPTIONAL, so in a game the
# line is nothing at all.
#
#   property_test        every tests/property/*_prop.cpp, doctest's main in
#                        tests/property/main.cpp, RapidCheck
#                        (thirdparty/rapidcheck). `rmp test property` builds
#                        and runs it.
#   fuzz_<name>          one per tests/fuzz/<name>.cpp: LLVMFuzzerTestOneInput,
#                        and the corpus in tests/fuzz/corpus/<name>/ and
#                        tests/fuzz/regressions/<name>/. tools/fuzz.sh builds,
#                        replays, fuzzes and merges them.
#
# Defined whenever the tests are (BUILD_TESTS), so the lint preset's compile
# database has every file of them and clang-tidy reads them like the rest; and
# EXCLUDE_FROM_ALL, so nothing builds them but a build that names them: the
# smoke stage's plain `cmake --build` does not pay for RapidCheck.
#
# A package of the campaign ADDS FILES -- a *_prop.cpp, a fuzz harness and its
# corpus -- and never edits this one, except for the one line a harness needs
# when the code it calls is not in librmp.a (_rmp_fuzz_extra_<name> below).

if(NOT BUILD_TESTS OR NOT TARGET rmp)
  return()
endif()

# --- how a fuzz target runs --------------------------------------------------
#   replay     tests/fuzz/replay_main.cpp is its main(): every file named on the
#              command line, once, under whatever sanitizers this build has.
#              Any compiler; the default, so a laptop with gcc replays the
#              corpus too, and a configure costs no probe.
#   libfuzzer  clang's -fsanitize=fuzzer: libFuzzer is its main(), and the
#              framework library is compiled with libFuzzer's coverage
#              (-fsanitize=fuzzer-no-link), which is what it steers by.
#              tools/fuzz.sh asks for it in build/fuzz when clang can link and
#              run a fuzzer -- the pinned image, or a clang with compiler-rt.
set(RMP_FUZZ_ENGINE "replay" CACHE STRING
    "How the fuzz targets run: replay (any compiler) or libfuzzer (clang, tools/fuzz.sh)")
set_property(CACHE RMP_FUZZ_ENGINE PROPERTY STRINGS replay libfuzzer)
if(NOT RMP_FUZZ_ENGINE MATCHES "^(replay|libfuzzer)$")
  message(FATAL_ERROR "RMP_FUZZ_ENGINE is '${RMP_FUZZ_ENGINE}': replay or libfuzzer.")
endif()
if(RMP_FUZZ_ENGINE STREQUAL "libfuzzer" AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "RMP_FUZZ_ENGINE=libfuzzer is clang's -fsanitize=fuzzer, and this is "
                      "${CMAKE_CXX_COMPILER_ID}. tools/fuzz.sh build picks the engine itself.")
endif()

# --- RapidCheck ----------------------------------------------------------------
# thirdparty/rapidcheck is its include/ and src/ as upstream has them, without
# its CMakeLists.txt (which would bring its tests, its install rules and its
# own flags): the library is these sources and nothing else. Compiled with
# the framework's Debug flags -- the sanitizers and the library hardening --
# because it shares std:: containers with the code under test, and without our
# warnings, which are not ours to fix in it. Its RC_ASSERT throws: exceptions
# are allowed in the test binaries that link it, and never under src/rmp/.
set(_rmp_rapidcheck "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/rapidcheck")
if(EXISTS "${_rmp_rapidcheck}/include/rapidcheck.h")
  file(GLOB_RECURSE _rmp_rapidcheck_sources CONFIGURE_DEPENDS "${_rmp_rapidcheck}/src/*.cpp")
  add_library(rapidcheck STATIC EXCLUDE_FROM_ALL ${_rmp_rapidcheck_sources})
  set_property(TARGET rapidcheck PROPERTY CXX_STANDARD 20)
  target_include_directories(rapidcheck SYSTEM PUBLIC "${_rmp_rapidcheck}/include")
  # Vendored: its warnings are not reported in a file of ours, nor here.
  target_compile_options(rapidcheck PRIVATE "$<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>")
  rmp_apply_compile_flags(rapidcheck)
endif()

# --- property_test ---------------------------------------------------------------
file(GLOB _rmp_properties CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/property/*_prop.cpp")
if(_rmp_properties AND TARGET rapidcheck AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/property/main.cpp")
  add_executable(property_test EXCLUDE_FROM_ALL
    ${_rmp_properties} "${CMAKE_CURRENT_LIST_DIR}/property/main.cpp")
  set_property(TARGET property_test PROPERTY CXX_STANDARD 20)
  target_include_directories(property_test SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/doctest")
  target_compile_definitions(property_test PRIVATE
    RMP_RESOURCES_PATH="${RESOURCES_PATH}"
    RMP_TEST_FIXTURES="${CMAKE_CURRENT_LIST_DIR}/fixtures/")
  rmp_link_framework(property_test)
  target_link_libraries(property_test PRIVATE rapidcheck)
  rmp_apply_compile_flags(property_test)
  rmp_apply_warnings(property_test)
endif()

# --- the fuzz targets -------------------------------------------------------------
# What a harness needs besides librmp.a, by its name. tests/e2e/script.cpp is
# the e2e scripts' grammar, a parser of the tests' own.
set(_rmp_fuzz_extra_e2e_script "${CMAKE_CURRENT_LIST_DIR}/e2e/script.cpp")

file(GLOB _rmp_fuzz_harnesses CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/fuzz/*.cpp")
list(REMOVE_ITEM _rmp_fuzz_harnesses "${CMAKE_CURRENT_LIST_DIR}/fuzz/replay_main.cpp")
set(_rmp_fuzz_targets "")
foreach(_harness IN LISTS _rmp_fuzz_harnesses)
  get_filename_component(_name "${_harness}" NAME_WE)
  set(_target "fuzz_${_name}")
  add_executable(${_target} EXCLUDE_FROM_ALL "${_harness}" ${_rmp_fuzz_extra_${_name}})
  set_property(TARGET ${_target} PROPERTY CXX_STANDARD 20)
  target_compile_definitions(${_target} PRIVATE
    RMP_RESOURCES_PATH="${RESOURCES_PATH}"
    RMP_TEST_FIXTURES="${CMAKE_CURRENT_LIST_DIR}/fixtures/")
  rmp_link_framework(${_target})
  rmp_apply_compile_flags(${_target})
  rmp_apply_warnings(${_target})
  if(RMP_FUZZ_ENGINE STREQUAL "libfuzzer")
    target_compile_options(${_target} PRIVATE -fsanitize=fuzzer)
    target_link_options(${_target} PRIVATE -fsanitize=fuzzer)
  else()
    target_sources(${_target} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/fuzz/replay_main.cpp")
  endif()
  list(APPEND _rmp_fuzz_targets ${_target})
endforeach()
if(_rmp_fuzz_targets)
  # Every fuzz target at once: what tools/fuzz.sh build asks for.
  add_custom_target(fuzz_all DEPENDS ${_rmp_fuzz_targets})
  if(RMP_FUZZ_ENGINE STREQUAL "libfuzzer")
    # The code under test is the framework's: without its coverage, libFuzzer
    # sees only the harness and mutates blind.
    target_compile_options(rmp PRIVATE -fsanitize=fuzzer-no-link)
  endif()
  message(STATUS "=== FUZZ: ${RMP_FUZZ_ENGINE}, ${_rmp_fuzz_targets} ===")
endif()
