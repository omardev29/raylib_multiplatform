# rmp_game_resources(<dir> <out_path> <out_dir>): where a game's resources are.
#
#   <out_path>   what RMP_RESOURCES_PATH tells the code to read
#   <out_dir>    the folder whose files a web build preloads at /resources/
#
# A game -- the project, or an example -- reads its own resources/ when it has
# one, and the project's otherwise. A production build reads "./resources/",
# next to the executable, because that is where the package puts them; what
# goes there is still the game's OWN folder. It used to be the project's for
# every target, so an example built for the web in production preloaded the
# project's art and failed to load its own.
#
# A file of its own so tests/configure_test.py can run it with `cmake -P`.
# Reads PRODUCTION_BUILD, RESOURCES_PATH and CMAKE_CURRENT_SOURCE_DIR from the
# caller, like the rest of CMakeLists.txt.
function(rmp_game_resources DIR OUT_PATH OUT_DIR)
  if(EXISTS "${DIR}/resources")
    set(_dir "${DIR}/resources")
  else()
    set(_dir "${CMAKE_CURRENT_SOURCE_DIR}/resources")
  endif()
  if(PRODUCTION_BUILD)
    set(_path "./resources/")
  elseif(EXISTS "${DIR}/resources")
    set(_path "${DIR}/resources/")
  else()
    set(_path "${RESOURCES_PATH}")
  endif()
  set(${OUT_PATH} "${_path}" PARENT_SCOPE)
  set(${OUT_DIR} "${_dir}" PARENT_SCOPE)
endfunction()
