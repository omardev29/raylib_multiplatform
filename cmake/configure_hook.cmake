# Run tools/configure.py, so `cmake --preset debug` is the only command a user
# ever needs after editing raylib_multiplatform.toml.
#
# This is included BEFORE project(), because the project name itself comes out
# of the generated file. That constrains what is available: CMAKE_SYSTEM_NAME,
# WIN32/APPLE/UNIX/MSVC, the compiler variables and the toolchain file do not
# exist yet. In particular find_package(Python3) must not be used here — it
# branches on those, and on the riscv64 cross build the toolchain sets
# CMAKE_FIND_ROOT_PATH_MODE_PACKAGE to ONLY, which would confine the search to
# the target sysroot. find_program runs before all of that.
#
# The generator runs on every configure rather than being gated behind a
# timestamp. It takes milliseconds, it rewrites a file only when the content
# actually changed (so nothing rebuilds spuriously), and content-hashing inputs
# from CMake would have to duplicate logic that already lives in Python.

set(_tpl_configure "${CMAKE_CURRENT_SOURCE_DIR}/tools/configure.py")
set(_tpl_generated "${CMAKE_CURRENT_SOURCE_DIR}/cmake/generated/project.cmake")

# Re-run CMake when the config changes. Honoured by Ninja and Makefiles, which
# is every generator this project uses.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/raylib_multiplatform.toml")

find_program(TEMPLATE_PYTHON NAMES python3 python py)

# `python.exe` on Windows is frequently the Microsoft Store stub, which prints
# an advert and exits 9009. And tomllib only exists from 3.11. One probe settles
# both questions.
set(_tpl_python_ok FALSE)
if(TEMPLATE_PYTHON)
  execute_process(
    COMMAND "${TEMPLATE_PYTHON}" -c "import tomllib"
    RESULT_VARIABLE _tpl_probe
    OUTPUT_QUIET ERROR_QUIET)
  if(_tpl_probe EQUAL 0)
    set(_tpl_python_ok TRUE)
  endif()
endif()

if(_tpl_python_ok)
  execute_process(
    COMMAND "${TEMPLATE_PYTHON}" "${_tpl_configure}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RESULT_VARIABLE _tpl_rc
    OUTPUT_VARIABLE _tpl_out
    ERROR_VARIABLE _tpl_err)
  if(NOT _tpl_rc EQUAL 0)
    # The generator already printed a specific, actionable message. Passing it
    # through beats CMake's own "process failed with 1".
    message(FATAL_ERROR "${_tpl_err}${_tpl_out}")
  endif()
  string(STRIP "${_tpl_out}" _tpl_out)
  if(_tpl_out)
    message(STATUS "${_tpl_out}")
  endif()
elseif(EXISTS "${_tpl_generated}")
  # The BSD legs land here: none of the FreeBSD/OpenBSD/NetBSD VM images ship
  # Python. CI generates on the Linux runner first and cross-platform-actions
  # rsyncs the workspace into the VM before each step, so the files are already
  # there and correct.
  message(WARNING
    "no usable Python 3.11+ found; building with the previously generated "
    "configuration in cmake/generated/. Edits to raylib_multiplatform.toml "
    "will NOT be picked up.")
else()
  message(FATAL_ERROR
    "no usable Python 3.11+ found and nothing has been generated yet.\n"
    "raylib_multiplatform.toml has to be turned into build files before this "
    "project can configure.\n"
    "Install Python 3.11 or newer, or run this once on a machine that has it:\n"
    "    python3 tools/configure.py")
endif()

unset(_tpl_probe)
unset(_tpl_rc)
unset(_tpl_out)
unset(_tpl_err)
unset(_tpl_python_ok)
unset(_tpl_configure)
unset(_tpl_generated)
