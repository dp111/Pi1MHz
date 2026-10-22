# Apply the lwIP patch series to build-tree copies of the files it changes.
#
#   cmake -DLWIP_DIR=<lwIP checkout> -DOUT_DIR=<copy root> -DFILES=<a;b;...>
#         -DPATCHES=<0001-x.patch;...> -P apply.cmake
#
# FILES are paths relative to the lwIP root (src/core/tcp_in.c).  The series is
# upstream git format-patch output, so it also carries test/ hunks: only src/
# is applied.  git apply has no fuzz: a patch that no longer fits (upstream
# changed the code, or took the fix) stops the build here.

file(REMOVE_RECURSE ${OUT_DIR})
foreach(f IN LISTS FILES)
   get_filename_component(d ${OUT_DIR}/${f} DIRECTORY)
   file(MAKE_DIRECTORY ${d})
   file(COPY_FILE ${LWIP_DIR}/${f} ${OUT_DIR}/${f})
endforeach()

# OUT_DIR is inside the Pi1MHz work tree; stop git finding that repository, or
# git apply would take the patch paths as relative to its top and skip them.
get_filename_component(parent ${OUT_DIR} DIRECTORY)
set(ENV{GIT_CEILING_DIRECTORIES} ${parent})

foreach(p IN LISTS PATCHES)
   execute_process(
      COMMAND git apply --include=src/* --whitespace=nowarn ${p}
      WORKING_DIRECTORY ${OUT_DIR}
      RESULT_VARIABLE rc
      ERROR_VARIABLE  err)
   if(NOT rc EQUAL 0)
      file(REMOVE_RECURSE ${OUT_DIR})
      message(FATAL_ERROR "lwIP patch does not apply: ${p}\n${err}"
         "If upstream lwIP has taken this fix, delete the patch file.")
   endif()
endforeach()
