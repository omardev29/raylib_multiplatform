# rmp_android_gl(<gl_version> <out_opengl> <out_gles> <out_define>)
#
# What [android] gl_version means for the native build. Gradle passes it as
# -DGL_VERSION (raymob/app/build.gradle), and it decides three things here, so
# that it is what the game is compiled for and not only what the manifest asks:
#
#   <out_opengl>  raylib's own OPENGL_VERSION, set before raylib is configured.
#                 raylib's CMake picks GRAPHICS_API_OPENGL_ES2 for Android and
#                 lets this override it.
#   <out_gles>    the GLES library the game's .so links: ES 3.0's functions are
#                 in libGLESv3, not libGLESv2.
#   <out_define>  the GRAPHICS_API_* define the game is compiled with, the one
#                 raylib was.
#
# ES20 and ES30 only: raylib has an ES 3.0 path and no 3.1 or 3.2 one, which is
# why tools/configure.py refuses the other two. Anything else stops the build
# here rather than compiling one thing and declaring another.
#
# A file of its own so tests/configure_test.py can run it with `cmake -P`.
function(rmp_android_gl GL_VERSION OUT_OPENGL OUT_GLES OUT_DEFINE)
  if(GL_VERSION STREQUAL "ES20")
    set(${OUT_OPENGL} "ES 2.0" PARENT_SCOPE)
    set(${OUT_GLES} "GLESv2" PARENT_SCOPE)
    set(${OUT_DEFINE} "GRAPHICS_API_OPENGL_ES2" PARENT_SCOPE)
  elseif(GL_VERSION STREQUAL "ES30")
    set(${OUT_OPENGL} "ES 3.0" PARENT_SCOPE)
    set(${OUT_GLES} "GLESv3" PARENT_SCOPE)
    set(${OUT_DEFINE} "GRAPHICS_API_OPENGL_ES3" PARENT_SCOPE)
  else()
    message(FATAL_ERROR
      "GL_VERSION='${GL_VERSION}': the Android build knows ES20 and ES30. It comes "
      "from [android] gl_version in raylib_multiplatform.toml, through Gradle.")
  endif()
endfunction()
