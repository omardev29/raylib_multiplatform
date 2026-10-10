# cmake/sanitize.cmake -- what a Debug build is checked with while it runs.
#
# Included by CMakeLists.txt after project() and only when PRODUCTION_BUILD is
# off: nothing in here can reach a build that ships. It provides one function,
# rmp_apply_debug_checks(<target>), which rmp_apply_compile_flags() calls for
# every target it compiles in Debug -- raylib, the framework, the game, every
# example and every test -- so the C++ that shares STL containers is all
# instrumented the same way.
#
# Two things, and only the second needs a runtime:
#
#   HARDENING, every Debug build, framework and game. Bounds-checked operator[]
#   and front()/back() in libstdc++ (_GLIBCXX_ASSERTIONS) and libc++
#   (_LIBCPP_HARDENING_MODE), and uninitialised locals filled with a pattern
#   that crashes instead of a zero that happens to work
#   (-ftrivial-auto-var-init=pattern, where the compiler has it: gcc 12+,
#   clang). _FORTIFY_SOURCE is not here: it needs -O1, and Debug is -O0.
#
#   SANITIZERS, [dev] sanitize in raylib_multiplatform.toml: "address" and
#   "undefined". The framework lists both, so its Debug builds always have them;
#   a game turns them on. RMP_SANITIZE=OFF switches them off for one build
#   directory, to measure what they cost:
#
#       cmake --preset debug -DRMP_SANITIZE=OFF
#
# Whether a compiler can give them is decided by RUNNING a sanitized program,
# not by finding a flag: Ubuntu's clang accepts -fsanitize=address and then has
# no libclang_rt.asan to link, and a runtime can link and still refuse to
# start. A sanitizer the probe cannot run is a WARNING and a build without it
# -- a machine condition, like the [dev] linker falling back -- unless
# RMP_REQUIRE_SANITIZERS=1 is in the environment, which CI sets: there it is a
# FATAL_ERROR, because a gate that quietly stopped instrumenting is a gate
# that passes by not looking.
#
# Never Web, Android or iOS: the web preset is a release, and Android and iOS
# build through their own projects, which never read [dev].

option(RMP_SANITIZE "Instrument this Debug build with [dev] sanitize (OFF to measure without)" ON)

include(CheckCCompilerFlag)
include(CheckCXXCompilerFlag)

# --- hardening ---------------------------------------------------------------
# Unset first, as everywhere in this project: a cached "yes" from another
# compiler is exactly what would hide a flag that went away.
unset(RMP_C_AUTO_VAR_INIT CACHE)
unset(RMP_CXX_AUTO_VAR_INIT CACHE)
set(CMAKE_REQUIRED_QUIET ON)
check_c_compiler_flag(-ftrivial-auto-var-init=pattern RMP_C_AUTO_VAR_INIT)
check_cxx_compiler_flag(-ftrivial-auto-var-init=pattern RMP_CXX_AUTO_VAR_INIT)
unset(CMAKE_REQUIRED_QUIET)
set(RMP_AUTO_VAR_INIT_C ${RMP_C_AUTO_VAR_INIT})
set(RMP_AUTO_VAR_INIT_CXX ${RMP_CXX_AUTO_VAR_INIT})
unset(RMP_C_AUTO_VAR_INIT CACHE)
unset(RMP_CXX_AUTO_VAR_INIT CACHE)

# --- sanitizers --------------------------------------------------------------
# Linked into every executable that is instrumented: the default options of
# each runtime, and the LeakSanitizer suppressions for the system's libraries.
# In C, so that a plain C game gets it too. It has to be an object of the
# executable and not of librmp.a: the runtimes declare these functions weak,
# and an archive member nothing else references is never pulled in.
set(RMP_SANITIZER_HOOKS "${CMAKE_CURRENT_LIST_DIR}/sanitizer_hooks.c")
# Third-party code only, each line with its reason: see the file.
set(RMP_SANITIZE_IGNORELIST "${CMAKE_CURRENT_LIST_DIR}/sanitize_ignore.txt")

set(RMP_SANITIZE_WANTED "")
set(RMP_SANITIZE_APPLIED "")
set(RMP_SANITIZE_COMPILE_FLAGS "")
set(RMP_SANITIZE_LINK_FLAGS "")
if(DEFINED RMP_DEV_SANITIZE AND RMP_SANITIZE AND NOT (EMSCRIPTEN OR ANDROID OR IOS))
  set(RMP_SANITIZE_WANTED ${RMP_DEV_SANITIZE})
endif()
if(DEFINED RMP_DEV_SANITIZE AND RMP_DEV_SANITIZE AND NOT RMP_SANITIZE)
  message(STATUS "=== SANITIZERS: off for this build directory (RMP_SANITIZE=OFF) ===")
endif()

# The flags for one list of sanitizers, by compiler. OUT_COMPILE and OUT_LINK
# are set in the caller's scope; both empty means "this compiler has none".
function(_rmp_sanitize_flags names out_compile out_link)
  set(_compile "")
  set(_link "")
  if(MSVC)
    # MSVC's /fsanitize takes address and nothing else (configure.py refuses
    # undefined with msvc). /INCREMENTAL is incompatible with it.
    if("address" IN_LIST names)
      set(_compile /fsanitize=address)
      set(_link /INCREMENTAL:NO)
    endif()
  elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "GNU")
    set(_list ${names})
    # float-cast-overflow by name: clang counts it in "undefined", gcc does
    # not, and a float cast to an int it does not fit is exactly the UB the
    # LDtk reader had.
    if("undefined" IN_LIST names)
      list(APPEND _list float-cast-overflow)
    endif()
    list(JOIN _list "," _joined)
    set(_compile -fsanitize=${_joined} -fno-sanitize-recover=all -fno-omit-frame-pointer)
    set(_link -fsanitize=${_joined})
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_C_COMPILER_ID MATCHES "Clang")
    list(JOIN names "," _joined)
    set(_compile -fsanitize=${_joined} -fno-sanitize-recover=all -fno-omit-frame-pointer
                 "-fsanitize-ignorelist=${RMP_SANITIZE_IGNORELIST}")
    set(_link -fsanitize=${_joined})
  endif()
  set(${out_compile} "${_compile}" PARENT_SCOPE)
  set(${out_link} "${_link}" PARENT_SCOPE)
endfunction()

# Build the probe with these sanitizers, the hooks linked in like every real
# executable, and RUN it: in C++ and in C, which is what raylib is. RESULT is
# TRUE or FALSE in the caller's scope, WHY the reason when it is FALSE.
function(_rmp_sanitize_probe names result why)
  _rmp_sanitize_flags("${names}" _compile _link)
  if(NOT _compile)
    set(${result} FALSE PARENT_SCOPE)
    set(${why} "${CMAKE_CXX_COMPILER_ID} has no ${names} sanitizer this build knows how to ask for"
        PARENT_SCOPE)
    return()
  endif()
  set(_dir "${CMAKE_BINARY_DIR}/CMakeFiles/rmp_sanitize_probe")
  file(WRITE "${_dir}/probe.cpp" [=[
#include <cstdio>
#include <string>
#include <vector>
int main() {
    std::vector<std::string> words{ "sanitizer", "probe" };
    std::printf("%s %s\n", words[0].c_str(), words[1].c_str());
    return words.size() == 2 ? 0 : 1;
}
]=])
  file(WRITE "${_dir}/probe.c" [=[
#include <stdio.h>
#include <stdlib.h>
int main(void) {
    char *word = malloc(6);
    if (!word) return 1;
    snprintf(word, 6, "probe");
    puts(word);
    free(word);
    return 0;
}
]=])
  foreach(_lang CXX C)
    if(_lang STREQUAL "CXX")
      set(_src "${_dir}/probe.cpp")
    else()
      set(_src "${_dir}/probe.c")
    endif()
    if(CMAKE_CROSSCOMPILING)
      # Nothing here can run what it builds, so linking is the most this can know.
      try_compile(_built SOURCES "${_src}" "${RMP_SANITIZER_HOOKS}"
                  COMPILE_DEFINITIONS ${_compile}
                  LINK_OPTIONS ${_link}
                  OUTPUT_VARIABLE _out)
      set(_ran 0)
    else()
      try_run(_ran _built SOURCES "${_src}" "${RMP_SANITIZER_HOOKS}"
              COMPILE_DEFINITIONS ${_compile}
              LINK_OPTIONS ${_link}
              COMPILE_OUTPUT_VARIABLE _out
              RUN_OUTPUT_VARIABLE _run_out)
    endif()
    if(NOT _built)
      string(REGEX MATCHALL "[^\n]*(error|cannot find|not found|undefined reference)[^\n]*"
             _errors "${_out}")
      list(SUBLIST _errors 0 3 _errors)
      list(JOIN _errors "\n      " _errors)
      set(${result} FALSE PARENT_SCOPE)
      set(${why} "a ${_lang} program with ${_compile} does not build:\n      ${_errors}" PARENT_SCOPE)
      return()
    endif()
    if(NOT _ran EQUAL 0)
      string(STRIP "${_run_out}" _run_out)
      set(${result} FALSE PARENT_SCOPE)
      set(${why} "a ${_lang} program with ${_compile} builds and does not run (exit ${_ran}):\n      ${_run_out}"
          PARENT_SCOPE)
      return()
    endif()
  endforeach()
  set(${result} TRUE PARENT_SCOPE)
endfunction()

if(RMP_SANITIZE_WANTED)
  if(MSVC)
    # /RTC is incompatible with /fsanitize=address, and CMake puts /RTC1 in
    # every Debug configuration. Before the probe, which inherits these.
    foreach(_lang C CXX)
      string(REGEX REPLACE "/RTC[1csu]+" "" CMAKE_${_lang}_FLAGS_DEBUG "${CMAKE_${_lang}_FLAGS_DEBUG}")
    endforeach()
  endif()
  _rmp_sanitize_probe("${RMP_SANITIZE_WANTED}" _ok _why)
  if(_ok)
    set(RMP_SANITIZE_APPLIED ${RMP_SANITIZE_WANTED})
  else()
    # All of them did not run together: keep each one that runs on its own.
    set(_reasons "${_why}")
    list(LENGTH RMP_SANITIZE_WANTED _count)
    if(_count GREATER 1)
      foreach(_one ${RMP_SANITIZE_WANTED})
        _rmp_sanitize_probe("${_one}" _one_ok _one_why)
        if(_one_ok)
          list(APPEND RMP_SANITIZE_APPLIED ${_one})
        endif()
      endforeach()
    endif()
  endif()
  set(_missing ${RMP_SANITIZE_WANTED})
  if(RMP_SANITIZE_APPLIED)
    list(REMOVE_ITEM _missing ${RMP_SANITIZE_APPLIED})
  endif()
  if(_missing)
    list(JOIN _missing ", " _missing_text)
    set(_message
      "[dev] sanitize asks for ${_missing_text}, and ${CMAKE_CXX_COMPILER_ID} "
      "${CMAKE_CXX_COMPILER_VERSION} (${CMAKE_CXX_COMPILER}) cannot run it here:\n"
      "      ${_reasons}\n"
      "  This Debug build goes on without it. To get it: on Linux with clang, install "
      "compiler-rt (libclang-rt-<N>-dev on Debian and Ubuntu, compiler-rt on Arch and "
      "Fedora); or use [dev] compiler = \"gcc\", whose libasan and libubsan most "
      "distributions ship; or set [dev] sanitize = [] to stop being told.")
    if("$ENV{RMP_REQUIRE_SANITIZERS}" STREQUAL "1")
      message(FATAL_ERROR ${_message}
        "\n  RMP_REQUIRE_SANITIZERS=1 is set, so a build without them is refused: CI "
        "sets it where the build is a gate.")
    else()
      message(WARNING ${_message})
    endif()
  endif()
  if(RMP_SANITIZE_APPLIED)
    _rmp_sanitize_flags("${RMP_SANITIZE_APPLIED}" RMP_SANITIZE_COMPILE_FLAGS RMP_SANITIZE_LINK_FLAGS)
    list(JOIN RMP_SANITIZE_APPLIED ", " _applied_text)
    message(STATUS "=== SANITIZERS: ${_applied_text} ===")
  endif()
endif()

# Every Debug target: the hardening, then the sanitizers that ran. Called by
# rmp_apply_compile_flags(), never at directory scope, so it reaches exactly
# what that function reaches -- and rres_pack, a host tool that rmp pack runs,
# is not one of them.
function(rmp_apply_debug_checks target)
  target_compile_definitions(${target} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:_GLIBCXX_ASSERTIONS>
    $<$<COMPILE_LANGUAGE:CXX>:_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE>)
  if(RMP_AUTO_VAR_INIT_C)
    target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:C>:-ftrivial-auto-var-init=pattern>)
  endif()
  if(RMP_AUTO_VAR_INIT_CXX)
    target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-ftrivial-auto-var-init=pattern>)
  endif()

  get_target_property(_type ${target} TYPE)
  # The hooks go into every executable whenever sanitizers were asked for,
  # applied or not: without a runtime they are four functions nobody calls,
  # and with them the file always has a compile command for clang-tidy.
  if(RMP_SANITIZE_WANTED AND _type STREQUAL "EXECUTABLE")
    target_sources(${target} PRIVATE "${RMP_SANITIZER_HOOKS}")
  endif()
  if(NOT RMP_SANITIZE_APPLIED)
    return()
  endif()
  target_compile_options(${target} PRIVATE ${RMP_SANITIZE_COMPILE_FLAGS})
  # What the code can ask: RMP_SANITIZE for "any", and one per sanitizer.
  # tests/sanitize_test.cpp holds them to what the compiler itself says.
  target_compile_definitions(${target} PRIVATE RMP_SANITIZE=1)
  if("address" IN_LIST RMP_SANITIZE_APPLIED)
    target_compile_definitions(${target} PRIVATE RMP_SANITIZE_ADDRESS=1)
  endif()
  if("undefined" IN_LIST RMP_SANITIZE_APPLIED)
    target_compile_definitions(${target} PRIVATE RMP_SANITIZE_UNDEFINED=1)
  endif()
  if(_type STREQUAL "EXECUTABLE")
    target_link_options(${target} PRIVATE ${RMP_SANITIZE_LINK_FLAGS})
  endif()
endfunction()
