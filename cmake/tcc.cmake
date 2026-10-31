# tcc as the C compiler: [dev] c_compiler = "tcc" or "auto" in
# raylib_multiplatform.toml, Debug builds only, x86-64 Linux only.
#
# tcc, the Tiny C Compiler, compiles C many times faster than GCC — raylib and
# GLFW in about a second — and optimizes nothing, which a debug build does not
# want anyway. It writes ELF object files, as GCC does, and the C++ compiler
# links them with everything else, through GNU ld (cmake/dev_linker.cmake says
# why). But tcc is not GCC, and this file is what that takes: for CMake to
# treat it like any other compiler, and for its code to link and run like
# anyone else's. rmp-docs' Configuration page, [dev] c_compiler, says what is
# still different.

if(NOT CMAKE_C_COMPILER_ID STREQUAL "TinyCC")
  return()
endif()

# x86-64 only. On 64-bit ARM, miniaudio, which raylib's audio is built on,
# reaches for NEON intrinsics as soon as it sees the processor, and tcc has
# none: the build would stop on <arm_neon.h>, deep inside raudio.c. On RISC-V
# it builds, and runs, but tcc's calls do not yet follow the platform's rules:
# some arguments arrive wrong, in raylib's own code too, with no error to say
# so.
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
  message(FATAL_ERROR
    "tcc builds this project on x86-64 Linux only, and this is "
    "${CMAKE_SYSTEM_PROCESSOR}: set [dev] c_compiler to \"same\" or \"auto\".")
endif()

# Header dependencies. CMake has no flag on file for asking tcc which headers
# a file includes, so it would record none, and editing a header would
# recompile nothing. tcc has GCC's -MD and -MF for that, though not -MT, which
# it does not need.
set(CMAKE_DEPFILE_FLAGS_C "-MD -MF <DEP_FILE>")

# C standards. CMake knows none for tcc either and passes it no -std= at all,
# so every target would get tcc's default, C99, whatever it asked for. tcc has
# C99, which C90 code compiles in, and C11; C17 is C11 with its defects fixed
# and nothing new, so it gets tcc's C11 too. -std=c11 even where CMake would ask
# for GNU extensions: tcc keeps its own in every mode, and 0.9.27 ignores
# -std=gnu11. C23 tcc does not have, and asking for it stops the configure with
# CMake's own message.
set(CMAKE_C_STANDARD_DEFAULT 99)
foreach(std 90 99)
  set(CMAKE_C${std}_STANDARD_COMPILE_OPTION "")
  set(CMAKE_C${std}_EXTENSION_COMPILE_OPTION "")
endforeach()
foreach(std 11 17)
  set(CMAKE_C${std}_STANDARD_COMPILE_OPTION "-std=c11")
  set(CMAKE_C${std}_EXTENSION_COMPILE_OPTION "-std=c11")
endforeach()

# The same for the C features CMake knows by name, which a library you add may
# ask for with target_compile_features(): without these, CMake stops on any of
# them, c_std_99 included, as unknown to tcc. Each goes with the standard that
# brought it.
set(CMAKE_C90_COMPILE_FEATURES c_std_90 c_function_prototypes)
set(CMAKE_C99_COMPILE_FEATURES c_std_99 c_restrict c_variadic_macros)
set(CMAKE_C11_COMPILE_FEATURES c_std_11 c_static_assert)
set(CMAKE_C17_COMPILE_FEATURES c_std_17)
set(CMAKE_C_COMPILE_FEATURES ${CMAKE_C90_COMPILE_FEATURES} ${CMAKE_C99_COMPILE_FEATURES}
    ${CMAKE_C11_COMPILE_FEATURES} ${CMAKE_C17_COMPILE_FEATURES})

# glibc's <regex.h>, which GLFW includes, declares regexec() with an array
# parameter sized by the parameter before it, and tcc 0.9.27, the one Debian
# and Ubuntu ship, cannot parse that. __STDC_NO_VLA__ is C11's way of saying
# "no variable length arrays here", and glibc then leaves the size out. Only
# code that looks at it changes, and in this build that is regex.h alone.
add_compile_definitions("$<$<COMPILE_LANGUAGE:C>:__STDC_NO_VLA__=1>")

# Debug information and unwind tables: DWARF, which gdb reads to show variables
# and step from line to line, and .eh_frame, which a C++ exception reads to
# unwind through tcc's code to its catch. Newer tcc writes both, but leaves
# part of each address where only its own linker looks: linked by any other,
# every function of a file sits where the file's first one does, gdb stops in
# the wrong function, and GNU ld refuses the unwind tables ("overlapping
# FDEs"). So every object tcc writes goes through tcc_fixup.c, next to this
# file, which moves those bytes to where every linker looks. CMake runs it as
# the C compiler's launcher, ahead of ccache, and tcc builds it here, in a
# fraction of a second. Adapted from raylib_cmake_template, as is that file. 0.9.27 writes no unwind tables, and no
# DWARF: it takes -gdwarf as -g, for stabs, which gdb 18 and later do not read.
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tcc")
set(TCC_LAUNCHER "${CMAKE_BINARY_DIR}/tcc/tcc_fixup")
execute_process(COMMAND "${CMAKE_C_COMPILER}" -o "${TCC_LAUNCHER}" "${CMAKE_CURRENT_LIST_DIR}/tcc_fixup.c"
  RESULT_VARIABLE _rc ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "tcc could not build cmake/tcc_fixup.c: ${_rc} ${_err}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/tcc_fixup.c")
list(PREPEND CMAKE_C_COMPILER_LAUNCHER "${TCC_LAUNCHER}")
add_compile_options("$<$<COMPILE_LANGUAGE:C>:-gdwarf>")

# tcc's runtime library, libtcc1.a: functions tcc's code calls for what GCC
# does inline — reading a va_arg on x86-64, turning a double into an unsigned
# 64-bit integer, __builtin_clz(). tcc adds it whenever it links, and the C++
# compiler, which links the game, has never heard of it. tcc says where it is.
execute_process(COMMAND "${CMAKE_C_COMPILER}" -print-search-dirs
  OUTPUT_VARIABLE _tcc_dirs ERROR_QUIET)
if(_tcc_dirs MATCHES "libtcc1:[ \t]*[\r\n]+[ \t]*([^\r\n]+)")
  string(STRIP "${CMAKE_MATCH_1}" TCC_LIBTCC1)
endif()
if(NOT EXISTS "${TCC_LIBTCC1}")
  message(FATAL_ERROR
    "`tcc -print-search-dirs` did not give the path of tcc's runtime library, "
    "libtcc1.a, or it is not there: \"${TCC_LIBTCC1}\".")
endif()
# A tcc update brings a new one, so CMake configures again when it changes.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${TCC_LIBTCC1}")

# The game links a copy, with the unwind tables taken out: tcc compiled
# libtcc1.a too, wrong tables and all, and no tcc_fixup ever saw it. None of
# its functions calls back into code that could throw. objcopy comes with the
# linker.
set(TCC_RUNTIME "${CMAKE_BINARY_DIR}/tcc/libtcc1.a")
execute_process(
  COMMAND "${CMAKE_OBJCOPY}" --remove-section=.eh_frame --remove-section=.rela.eh_frame
          "${TCC_LIBTCC1}" "${TCC_RUNTIME}"
  RESULT_VARIABLE _rc ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "objcopy (\"${CMAKE_OBJCOPY}\") could not copy ${TCC_LIBTCC1}: ${_rc} ${_err}")
endif()

# tcc 0.9.27's libtcc1.a has no __builtin_clz(), which raylib's DEFLATE
# decoder calls (sinfl.h, in rcore.c): the game would not link. A newer tcc
# has it; for 0.9.27, the copy gets one, compiled by tcc itself.
execute_process(COMMAND "${CMAKE_NM}" "${TCC_RUNTIME}" OUTPUT_VARIABLE _syms ERROR_QUIET)
if(NOT _syms MATCHES " [TW] __builtin_clz\n")
  file(WRITE "${CMAKE_BINARY_DIR}/tcc/builtin_clz.c"
    "int __builtin_clz(unsigned int x) { int n = 32; while (x) { x >>= 1; n--; } return n; }\n")
  execute_process(COMMAND "${CMAKE_C_COMPILER}" -c builtin_clz.c -o builtin_clz.o
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/tcc" RESULT_VARIABLE _rc)
  if(_rc EQUAL 0)
    execute_process(COMMAND "${CMAKE_AR}" rs libtcc1.a builtin_clz.o
      WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/tcc" RESULT_VARIABLE _rc)
  endif()
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "Could not add __builtin_clz() to ${TCC_RUNTIME}: ${_rc}")
  endif()
endif()

# Every target from here on links it: raylib, GLFW, your game. As a target,
# CMake places it after everything that calls into it, where a compiler's
# runtime library goes.
add_library(tcc_runtime STATIC IMPORTED)
set_target_properties(tcc_runtime PROPERTIES IMPORTED_LOCATION "${TCC_RUNTIME}")
link_libraries(tcc_runtime)

# tcc's object files do not say that their stack need not be executable —
# GCC's carry a .note.GNU-stack for that — so GNU ld would warn and make the
# game's stack executable, which only an exploit wants. tcc never needs it.
add_link_options(LINKER:-z,noexecstack)
