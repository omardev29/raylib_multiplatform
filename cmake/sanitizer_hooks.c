/* cmake/sanitizer_hooks.c -- what the sanitizers do by default, linked into
 * every executable cmake/sanitize.cmake instruments.
 *
 * Each runtime asks the program for its defaults through a weak function it
 * declares and calls at startup if the program defines it. The environment
 * still wins -- ASAN_OPTIONS, UBSAN_OPTIONS, LSAN_OPTIONS are read after these
 * -- so a person chasing one report can change anything for one run.
 *
 * C, so that a plain C game gets the same defaults as a C++ one. Defined
 * whether or not the build ended up instrumented: without a runtime nothing
 * calls them, and they cost four tiny functions in a Debug build.
 *
 * The names are the runtimes' ABI, reserved identifiers chosen by the
 * compiler's authors: that is what the NOLINT around them says. */

#if defined(__cplusplus)
#error "cmake/sanitizer_hooks.c is C: the runtimes look these names up unmangled"
#endif

/* NOLINTBEGIN(bugprone-reserved-identifier, readability-identifier-naming) */

/* ASan (gcc, clang, MSVC):
 *   detect_stack_use_after_return  a pointer to a local that outlived its
 *                                  function is a report, not a stale read
 *   check_initialization_order,    a global whose initialiser reads another
 *   strict_init_order              file's global before that one ran
 *   halt_on_error                  the first report ends the run
 *
 * LeakSanitizer is part of ASan where the runtime turns it on by itself,
 * which is Linux: it is left to that default rather than asked for, because
 * asking for it on macOS stops every program with "detect_leaks is not
 * supported on this platform". */
const char *__asan_default_options(void);
const char *__asan_default_options(void) {
    return "detect_stack_use_after_return=1:check_initialization_order=1:"
           "strict_init_order=1:halt_on_error=1";
}

/* UBSan: the stack that got there, the check's name, and stop. The build
 * passes -fno-sanitize-recover=all, so every report is fatal already;
 * halt_on_error says the same for anything compiled without it. */
const char *__ubsan_default_options(void);
const char *__ubsan_default_options(void) {
    return "print_stacktrace=1:report_error_type=1:halt_on_error=1";
}

/* LeakSanitizer suppressions: memory the SYSTEM's libraries keep for the life
 * of the process and never give back -- not a leak of ours, and nothing we
 * can free. Every line names a shared library by its file name, and comes
 * from a run that reported it (the comment says which). Never a function, a
 * file or a library of ours: tests/configure_test.py refuses a line that
 * could match anything under src/, include/, tests/ or examples/, and the
 * leak in tests/sanitizer_canary.cpp has to be reported through all of them. */
const char *__lsan_default_suppressions(void);
const char *__lsan_default_suppressions(void) {
    return "";
}

/* NOLINTEND(bugprone-reserved-identifier, readability-identifier-naming) */
