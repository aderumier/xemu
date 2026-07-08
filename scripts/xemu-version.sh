#!/bin/bash

set -eu

dir="$1"
XEMU_DATE=$(date -u)
XEMU_COMMIT=$( \
  cd "$dir"; \
  if test -e .git; then \
    git rev-parse HEAD 2>/dev/null | tr -d '\n'; \
  elif test -e XEMU_COMMIT; then \
    cat XEMU_COMMIT; \
  fi)
XEMU_VERSION=$( \
  cd "$dir"; \
  if test -e .git; then \
    git describe --tags --match 'v*' | cut -c 2- | tr -d '\n'; \
  elif test -e XEMU_VERSION; then \
    cat XEMU_VERSION; \
  fi)

if [[ "${XEMU_VERSION}" == "" ]]; then
  XEMU_VERSION="0.0.0"
fi

get_version_field() {
  echo ${XEMU_VERSION}-0 | cut -d- -f$1
}

get_version_dot () {
  echo $(get_version_field 1) | cut -d. -f$1
}

# The four FILEVERSION fields must be plain numbers; tags like
# "v0.9-beta" would otherwise produce empty/alpha fields and break
# the version.rc resource compile.
sanitize_num() {
  case "$1" in
    ''|*[!0-9]*) echo 0 ;;
    *) echo "$1" ;;
  esac
}

XEMU_VERSION_MAJOR=$(sanitize_num "$(get_version_dot 1)")
XEMU_VERSION_MINOR=$(sanitize_num "$(get_version_dot 2)")
XEMU_VERSION_PATCH=$(sanitize_num "$(get_version_dot 3)")
XEMU_VERSION_COMMIT=$(sanitize_num "$(get_version_field 2)")

cat <<EOF
#define XEMU_VERSION       "$XEMU_VERSION"
#define XEMU_VERSION_MAJOR $XEMU_VERSION_MAJOR
#define XEMU_VERSION_MINOR $XEMU_VERSION_MINOR
#define XEMU_VERSION_PATCH $XEMU_VERSION_PATCH
#define XEMU_VERSION_COMMIT $XEMU_VERSION_COMMIT
#define XEMU_COMMIT        "$XEMU_COMMIT"
#define XEMU_DATE          "$XEMU_DATE"
EOF
