# Sets TEMPLATE_PYTHON to the first Python 3.11+ on PATH, and _tpl_python_ok.
#
# A file of its own so that tests/configure_test.py can run it with `cmake -P`
# against a PATH it builds, without running the generator.
#
# Every candidate is ASKED, not just found. `python3.exe` on Windows is often
# the Microsoft Store stub, which prints an advert and exits 9009 while `py`
# and `python` work; macOS's own /usr/bin/python3 is 3.9, older than tomllib
# (3.11). Taking the first name found and probing only that one failed both of
# those machines with Python installed. The same names, in the same order, as
# the `rmp` launcher.
set(TEMPLATE_PYTHON "")
set(_tpl_python_ok FALSE)
foreach(_tpl_name python3 python3.14 python3.13 python3.12 python3.11 python py)
  # A found find_program() variable is not searched again, so each name
  # starts from nothing.
  unset(_tpl_candidate)
  find_program(_tpl_candidate NAMES ${_tpl_name} NO_CACHE)
  if(NOT _tpl_candidate)
    continue()
  endif()
  execute_process(
    COMMAND "${_tpl_candidate}" -c "import tomllib"
    RESULT_VARIABLE _tpl_probe
    OUTPUT_QUIET ERROR_QUIET)
  if(_tpl_probe EQUAL 0)
    set(TEMPLATE_PYTHON "${_tpl_candidate}")
    set(_tpl_python_ok TRUE)
    break()
  endif()
endforeach()
unset(_tpl_candidate)
unset(_tpl_name)
