#!/bin/sh

objroot=$1

cat <<EOF
#ifndef JEMALLOC_H_
#define JEMALLOC_H_

/* A macro that is used to indicate that this the jemalloc vendored with the project
 * and has been tested with active defragmentation. */
#define VALKEY_VENDORED_JEMALLOC 1

#ifdef __cplusplus
extern "C" {
#endif

EOF

for hdr in jemalloc_defs.h jemalloc_rename.h jemalloc_macros.h \
           jemalloc_protos.h jemalloc_typedefs.h jemalloc_mangle.h ; do
  cat "${objroot}include/jemalloc/${hdr}" \
      | grep -v 'Generated from .* by configure\.' \
      | sed -e 's/ $//g'
  echo
done

cat <<EOF
#ifdef __cplusplus
}
#endif
#endif /* JEMALLOC_H_ */
EOF
