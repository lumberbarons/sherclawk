/* The one staged File Manager publication body write, shared by the tools,
 * the build executor, the job queue and the launcher. Callers keep their
 * journal envelopes and publication renames; this module owns the byte-level
 * ladder so a correctness fix to it lands once, not per caller. */
#ifndef SHERCLAWK_FSUTIL_H
#define SHERCLAWK_FSUTIL_H
#include <Files.h>

/* Create dest as a plain file with the given identity, write the whole body,
 * close, flush, prove the catalog record and read the bytes back to compare.
 * Returns the first failing OSErr (dupFNErr: the name was taken), so no
 * caller can forget a step of the sequence. *out_info, when not NULL,
 * receives the verified catalog record of the staged file. */
OSErr fs_write_verified(const FSSpec *dest, OSType creator, OSType type,
                        const void *bytes, long length, CInfoPBRec *out_info);

/* The catalog proof and read-back comparison alone, for a body someone else
 * wrote: the post-publication verification of an already-complete file. */
OSErr fs_verify_body(const FSSpec *file, OSType creator, OSType type,
                     const void *bytes, long length);
#endif
