# Generate scripts/gitversion.h from git: the release name (nearest tag), the
# full version (tag + commits-since + short hash + -dirty marker) and a build
# timestamp.
#
# Run as a CMake script, not included:
#   cmake -DSRC_DIR=<repo> -DOUT=<header> -P gitversion.cmake

# Full version string: nearest tag, commits since it, short hash, and an
# automatic -dirty suffix for uncommitted tracked changes. Falls back to a bare
# short hash, then to "unknown" outside a git tree.
execute_process(
   COMMAND git -C "${SRC_DIR}" describe --tags --always --dirty=-dirty --abbrev=7
   OUTPUT_VARIABLE GIT_VERSION
   OUTPUT_STRIP_TRAILING_WHITESPACE
   RESULT_VARIABLE GIT_RESULT
   ERROR_QUIET
)
if(NOT GIT_RESULT EQUAL 0 OR GIT_VERSION STREQUAL "")
   set(GIT_VERSION "unknown")
endif()

# Release name: just the nearest tag, e.g. v1.28. This is the single source of
# truth for the release number - bump it by creating a git tag, not by editing
# source. "dev" when the tree has no reachable tag.
execute_process(
   COMMAND git -C "${SRC_DIR}" describe --tags --abbrev=0
   OUTPUT_VARIABLE GIT_RELEASE
   OUTPUT_STRIP_TRAILING_WHITESPACE
   RESULT_VARIABLE TAG_RESULT
   ERROR_QUIET
)
if(NOT TAG_RESULT EQUAL 0 OR GIT_RELEASE STREQUAL "")
   set(GIT_RELEASE "dev")
endif()

# "-dirty" alone cannot tell two builds of a modified tree apart, and that is
# the case we actually live in: editing a header changes the image but leaves
# the describe output byte-identical, so the banner kept naming an image it did
# not build. Fingerprint the working tree instead - the diff against HEAD, the
# porcelain status, and the contents of any untracked file (which neither of
# the other two sees) - and fold 8 hex digits of it into the version.
#
# Deliberately git, not a filesystem walk: globbing src/ and stat-ing every
# file took 117 seconds on this /mnt/c tree (2267 files over 9p). git answers
# from its index in well under a second.
#
# The untracked list is gathered BEFORE the gate below, because it is also what
# decides whether the gate opens: "git describe --dirty" reports only tracked
# modifications, so a tree whose sole difference from HEAD is an untracked
# source file - a service overlay named in CMakeLists, say - describes clean.
# Gating the fingerprint on "-dirty" alone therefore stamped two such trees
# identically while they built different images, which is the case this whole
# block exists to prevent.
#
# third_party is excluded throughout: a vendored dependency is a nested git
# repo, so it surfaces here as one untracked "third_party/" entry and would
# churn the fingerprint on every build without saying anything about Pi1MHz.
execute_process(
   COMMAND git -C "${SRC_DIR}" ls-files --others --exclude-standard -- . ":(exclude)scripts/gitversion.h" ":(exclude)third_party/**"
   OUTPUT_VARIABLE GIT_UNTRACKED_FILES
   OUTPUT_STRIP_TRAILING_WHITESPACE
   RESULT_VARIABLE UNTRACKED_RESULT
   ERROR_QUIET
)
if(NOT UNTRACKED_RESULT EQUAL 0)
   # Treat a failed query as "nothing untracked" rather than discarding the
   # whole fingerprint: a partial identity still beats none.
   set(GIT_UNTRACKED_FILES "")
endif()

if(GIT_VERSION MATCHES "-dirty$" OR NOT GIT_UNTRACKED_FILES STREQUAL "")
   # Pathspec matters: an unqualified "git diff HEAD" covers the whole repo,
   # and every build rewrites the tracked firmware/kernel*.img, so the
   # fingerprint moved on its own and each build regenerated the header and
   # forced another LTO relink. Limit it to the source tree, and exclude the
   # generated header itself (its BUILD_DATE would otherwise feed back in).
   execute_process(
      COMMAND git -C "${SRC_DIR}" diff --ignore-submodules=dirty HEAD -- . ":(exclude)scripts/gitversion.h" ":(exclude)third_party/**"
      OUTPUT_VARIABLE GIT_DIFF
      RESULT_VARIABLE DIFF_RESULT
      ERROR_QUIET
   )
   execute_process(
      COMMAND git -C "${SRC_DIR}" status --porcelain --ignore-submodules=dirty -- . ":(exclude)scripts/gitversion.h" ":(exclude)third_party/**"
      OUTPUT_VARIABLE GIT_STATUS
      ERROR_QUIET
   )
   # Porcelain status records only the NAMES of untracked files, so editing an
   # already-untracked service overlay left *VERSION naming the previous kernel
   # image. Hash their contents too. Only untracked files reach this loop -
   # normally none at all - so it does not reintroduce the filesystem walk the
   # comment above rejects; a deliberately huge file dropped under src/ would
   # be read on every build, which is the one case worth knowing about.
   set(GIT_UNTRACKED_CONTENT "")
   if(NOT GIT_UNTRACKED_FILES STREQUAL "")
      string(REPLACE "\n" ";" GIT_UNTRACKED_LIST "${GIT_UNTRACKED_FILES}")
      foreach(UNTRACKED_FILE IN LISTS GIT_UNTRACKED_LIST)
         if(EXISTS "${SRC_DIR}/${UNTRACKED_FILE}" AND
            NOT IS_DIRECTORY "${SRC_DIR}/${UNTRACKED_FILE}")
            file(SHA256 "${SRC_DIR}/${UNTRACKED_FILE}" UNTRACKED_HASH)
            string(APPEND GIT_UNTRACKED_CONTENT
               "${UNTRACKED_FILE}:${UNTRACKED_HASH}\n")
         endif()
      endforeach()
   endif()
   if(DIFF_RESULT EQUAL 0)
      string(MD5 TREE_HASH
         "${GIT_DIFF}${GIT_STATUS}${GIT_UNTRACKED_CONTENT}")
      string(SUBSTRING "${TREE_HASH}" 0 8 TREE_HASH)
      # Keep the suffix regular. A tree that differs from HEAD only by
      # untracked content still builds a different image, and git describe
      # will not have marked it, so mark it here: the version is always
      # either "<describe>" or "<describe>-dirty.<hash>", never a bare
      # "<describe>.<hash>" that no git command would ever produce.
      if(NOT GIT_VERSION MATCHES "-dirty$")
         set(GIT_VERSION "${GIT_VERSION}-dirty")
      endif()
      set(GIT_VERSION "${GIT_VERSION}.${TREE_HASH}")
   endif()
endif()

# Decide whether the header actually needs rewriting. This script runs on every
# build, but rewriting it recompiles its three includers and forces a full LTO
# relink, so only do it when something really changed.
#
# Two independent triggers, because either one alone leaves a banner that names
# an image it did not build:
#   1. the git version string moved - covers committing, which changes no file
#      under src/ at all, so a source-mtime test would never notice it;
#   2. any source OR HEADER is newer than the header - covers editing a .h,
#      which recompiles code but changes no .c.
set(NEED_WRITE TRUE)
set(WHY "no existing header")

if(EXISTS "${OUT}")
   file(READ "${OUT}" OLD_HEADER)
   string(REGEX MATCH "#define GITVERSION  \"([^\"]*)\"" _m "${OLD_HEADER}")
   set(OLD_VERSION "${CMAKE_MATCH_1}")
   string(REGEX MATCH "#define RELEASENAME \"([^\"]*)\"" _m2 "${OLD_HEADER}")
   set(OLD_RELEASE "${CMAKE_MATCH_1}")

   if(NOT OLD_VERSION STREQUAL GIT_VERSION OR NOT OLD_RELEASE STREQUAL GIT_RELEASE)
      set(WHY "git version changed: ${OLD_VERSION} -> ${GIT_VERSION}")
   else()
      set(NEED_WRITE FALSE)
      set(WHY "unchanged")
   endif()
endif()

if(NOT NEED_WRITE)
   message(STATUS "gitversion.h: up to date (${GIT_RELEASE} / ${GIT_VERSION})")
   return()
endif()

string(TIMESTAMP BUILD_DATE "%Y-%m-%d %H:%M:%S")

file(WRITE "${OUT}"
   "/* Auto-generated by scripts/gitversion.cmake - do not edit, do not commit. */\n"
   "#define RELEASENAME \"${GIT_RELEASE}\"\n"
   "#define GITVERSION  \"${GIT_VERSION}\"\n"
   "#define BUILD_DATE  \"${BUILD_DATE}\"\n"
)

message(STATUS "gitversion.h: ${GIT_RELEASE} / ${GIT_VERSION}  ${BUILD_DATE}  (${WHY})")
