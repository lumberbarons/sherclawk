/* File Manager identity, revision and staged-publication helpers shared by the tools,
 * the build executor, the job queue and the launcher. Callers keep their
 * journal envelopes and publication renames; this module owns the byte-level
 * ladder so a correctness fix to it lands once for these callers. The
 * cooperative chunked stagers share identity and hashing but retain their
 * own stepped body writes. */
#ifndef SHERCLAWK_FSUTIL_H
#define SHERCLAWK_FSUTIL_H
#include <Files.h>
#include <stddef.h>
#include <stdint.h>

OSErr fs_catalog(const FSSpec *, CInfoPBRec *);
int fs_is_alias(const CInfoPBRec *);
/* Stable identity, fork sizes, lock/directory bits and Finder type, creator and flags.
 * Transient fork-open catalog bits are deliberately ignored. */
int fs_same_file(const CInfoPBRec *, const CInfoPBRec *);
uint32_t fs_fnv1a(const void *, size_t);
uint32_t fs_fnv1a_more(uint32_t, const void *, size_t);
/* Whole bytes of a nonnegative length already checked by the caller. */
void fs_revision(const CInfoPBRec *, const void *, long, char *, size_t);
/* Cooperative scanners already computed the hash a page at a time. */
void fs_revision_hash(const CInfoPBRec *, long, uint32_t, char *, size_t);

/* Create dest as a plain file with the given identity, write the whole body,
 * close, flush, prove the catalog record and read the bytes back to compare.
 * Returns the first failing OSErr (dupFNErr: the name was taken), so no
 * caller can forget a step of the sequence. *out_info, when not NULL,
 * receives the verified catalog record of the staged file and is left
 * untouched on any failure. */
OSErr fs_write_verified(const FSSpec *dest, OSType creator, OSType type,
                        const void *bytes, long length, CInfoPBRec *out_info);

/* The catalog proof and read-back comparison alone, for a body someone else
 * wrote: the post-publication verification of an already-complete file. */
OSErr fs_verify_body(const FSSpec *file, OSType creator, OSType type,
                     const void *bytes, long length);
#endif
