# Sourced by every host test runner that compiles C, right after it sets
# HERE.  It wraps gcc so that every compile in the suite turns a call to an
# undeclared function, or an implicit int, into an error.  GCC 14 and later
# already make these errors in C99 and later; GCC 13 (ubuntu-latest's CI
# compiler) only warns, so a stub that forgot a firmware prototype built and
# ran against a guessed int-returning signature.  A shell function rather
# than a CFLAGS variable so that each gcc line stays as it was and a suite
# run on its own gets the same flags as run_all.sh.  Not a script: no
# shebang, sourced with ".".
gcc() {
   command gcc -Werror=implicit-function-declaration -Werror=implicit-int "$@"
}
