/* Fault-injection shim for the mac2ios regression tests.
 *
 * Loaded into the tool process with DYLD_INSERT_LIBRARIES (Apple) or
 * LD_PRELOAD (elsewhere); the MAC2IOS_FAULT environment variable picks the
 * stdio call to fail with errno=EIO:
 *
 *   fread:  only requests larger than 127 bytes fail, so header-sized reads
 *           (archive magic, 60-byte member headers, <=127 byte names) pass.
 *   fwrite: every call fails before reaching the stream; nothing is written.
 *   fseek:  fails only once a real fwrite has succeeded, so the pre-write
 *           reposition passes and the post-write one fails.
 *   fclose: every call fails without closing the stream.
 *
 * An absent or empty MAC2IOS_FAULT passes everything through unchanged.
 */
#if !defined(__APPLE__)
#define _GNU_SOURCE 1 /* RTLD_NEXT and the dlfcn prototypes elsewhere. */
#endif

#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef RTLD_NEXT
#define RTLD_NEXT ((void *)-1l)
#endif

#ifdef __APPLE__
/* Static shims; dyld's __DATA,__interpose section does the redirection. */
#define FAULTINJ_LINKAGE static
#define FAULTINJ_NAME(name) faultinj_##name
#else
/* Exported shims so LD_PRELOAD symbol interposition finds them first. */
#define FAULTINJ_LINKAGE
#define FAULTINJ_NAME(name) name
#endif

static size_t (*real_fread)(void *, size_t, size_t, FILE *);
static size_t (*real_fwrite)(const void *, size_t, size_t, FILE *);
static int (*real_fseek)(FILE *, long, int);
static int (*real_fclose)(FILE *);

enum fault_mode { FAULT_NONE, FAULT_FREAD, FAULT_FWRITE, FAULT_FSEEK, FAULT_FCLOSE };
static enum fault_mode mode = FAULT_NONE;
/* Set once a passthrough fwrite has fully succeeded; fseek fails after it. */
static int wrote = 0;
/* Where that fwrite started, so only the write's own rewind is disturbed. */
static long wrote_from = -1;
static int rewound = 0; /* the post-write reposition already failed once */

FAULTINJ_LINKAGE size_t FAULTINJ_NAME(fread)(void *, size_t, size_t, FILE *);
FAULTINJ_LINKAGE size_t FAULTINJ_NAME(fwrite)(const void *, size_t, size_t, FILE *);
FAULTINJ_LINKAGE int FAULTINJ_NAME(fseek)(FILE *, long, int);
FAULTINJ_LINKAGE int FAULTINJ_NAME(fclose)(FILE *);

#ifdef __APPLE__
/* dyld swaps every binding of replacee for replacement, in this process. */
#define DYLD_INTERPOSE(replacement, replacee) \
    __attribute__((used)) static struct { \
        __typeof__(&replacement) replacement; \
        __typeof__(&replacee) replacee; \
    } interpose_##replacee \
    __attribute__((section("__DATA,__interpose"))) = { replacement, replacee };

DYLD_INTERPOSE(faultinj_fread, fread)
DYLD_INTERPOSE(faultinj_fwrite, fwrite)
DYLD_INTERPOSE(faultinj_fseek, fseek)
DYLD_INTERPOSE(faultinj_fclose, fclose)
#endif

__attribute__((constructor)) static void faultinj_init(void) {
    const char *name = getenv("MAC2IOS_FAULT");
    if(name != NULL) {
        if(strcmp(name, "fread") == 0) mode = FAULT_FREAD;
        else if(strcmp(name, "fwrite") == 0) mode = FAULT_FWRITE;
        else if(strcmp(name, "fseek") == 0) mode = FAULT_FSEEK;
        else if(strcmp(name, "fclose") == 0) mode = FAULT_FCLOSE;
    }
#ifdef __APPLE__
    /* dlsym(RTLD_NEXT, ...) is unusable here: dyld feeds every lookup result
       back through the interpose table, so it hands out our own shims and
       calling them recurses forever. The entries above still hold the true
       replacee addresses, so resolve the originals from those slots. */
    real_fread = interpose_fread.fread;
    real_fwrite = interpose_fwrite.fwrite;
    real_fseek = interpose_fseek.fseek;
    real_fclose = interpose_fclose.fclose;
#else
    /* Under LD_PRELOAD dlsym(RTLD_NEXT, ...) finds the real libc symbols. */
#define LOAD_REAL(field, symbol) \
    do { \
        union { __typeof__(real_##field) fn; void *obj; } swap; \
        swap.obj = dlsym(RTLD_NEXT, symbol); \
        real_##field = swap.fn; \
    } while(0)
    LOAD_REAL(fread, "fread");
    LOAD_REAL(fwrite, "fwrite");
    LOAD_REAL(fseek, "fseek");
    LOAD_REAL(fclose, "fclose");
#undef LOAD_REAL
#endif
}

FAULTINJ_LINKAGE size_t FAULTINJ_NAME(fread)(
        void *ptr, size_t size, size_t nmemb, FILE *stream) {
    /* Overflow-safe "size * nmemb > 127": only big payload reads fail. */
    if(mode == FAULT_FREAD && size != 0 && nmemb > (size_t)127 / size) {
        errno = EIO;
        return 0;
    }
    return real_fread(ptr, size, nmemb, stream);
}

FAULTINJ_LINKAGE size_t FAULTINJ_NAME(fwrite)(
        const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if(mode == FAULT_FWRITE) {
        errno = EIO;
        return 0;
    }
    long at = ftell(stream);
    size_t written = real_fwrite(ptr, size, nmemb, stream);
    if(written == nmemb) {
        wrote = 1;
        wrote_from = at;
        rewound = 0;
    }
    return written;
}

FAULTINJ_LINKAGE int FAULTINJ_NAME(fseek)(FILE *stream, long offset, int whence) {
    /* Fail the write's own reposition: the first post-write absolute seek
       back to where the successful fwrite started. Every other fseek (all
       pre-write seeks, forward members steps, later rewrites' rewinds) and
       everything before any fwrite passes through. */
    if(mode == FAULT_FSEEK && wrote && !rewound &&
       whence == SEEK_SET && offset == wrote_from) {
        rewound = 1;
        errno = EIO;
        return -1;
    }
    return real_fseek(stream, offset, whence);
}

FAULTINJ_LINKAGE int FAULTINJ_NAME(fclose)(FILE *stream) {
    if(mode == FAULT_FCLOSE) {
        errno = EIO;
        return EOF;
    }
    return real_fclose(stream);
}
