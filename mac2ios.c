#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "ar.h"
#include "macho.h"


void print_hdr(mach_header_64 *hdr) {
    printf("magic:         %x\n", hdr->magic);
    printf("cpu type:      %d\n", hdr->cputype);
    printf("cpu subtype:   %d\n", hdr->cpusubtype);
    printf("file type:     %d\n", hdr->filetype);
    printf("Load Commands: %d\n", hdr->ncmds);
    printf("LC size:       %d\n", hdr->sizeofcmds);
    printf("flags:         %d\n", hdr->flags);
}

// 64kb. Load Commands are limited to 16kb.
// the header is ~64bytes. So 64kb should be plenty.
uint8_t buffer[64*1024];

void patch_object(FILE *fp, size_t len, int platform) {
    int pos = ftell(fp);

    fread(buffer,sizeof(buffer),1,fp);

    mach_header_64 *hdr = (mach_header_64*)buffer;

    if (!(  hdr->magic == 0xfeedface      // 32-bit
         || hdr->magic == 0xfeedfacf      // 64-bit
         || hdr->magic == 0xcafebabe )) { // Universal
      // Object is not Mach-O
      fseek(fp, pos, SEEK_SET);
      return;
    }

    size_t offset = sizeof(mach_header_64);
    load_command *cmd;
    while(offset < sizeof(mach_header_64) + hdr->sizeofcmds) {
        cmd = (load_command*)(buffer+offset);
        switch(cmd->cmd) {
            /* old style LC_VERSION_MIN_<platofrm> rewrites */
            case LC_VERSION_MIN_MACOSX: {
                cmd->cmd = LC_VERSION_MIN_IPHONEOS;
                printf("Chaning LC_VERSION_MIN_ from: %d -> %d\n", LC_VERSION_MIN_MACOSX, LC_VERSION_MIN_IPHONEOS);
                fseek(fp, pos+offset, SEEK_SET);
                fwrite(buffer+offset, sizeof(struct build_version_command), 1, fp);
                break;
            }
            /* new style LC_BUILD_VERSION rewrites */
            case LC_BUILD_VERSION: {
                struct build_version_command* bvc = (struct build_version_command*)(buffer+offset);
                printf("Chaning platform from: %d -> %d\n", bvc->platform, platform);
                bvc->platform = platform;
                fseek(fp, pos+offset, SEEK_SET);
                fwrite(buffer+offset, sizeof(struct build_version_command), 1, fp);
                break;
            }
            default: break;
        }
        offset += cmd->cmdsize;
    }
    fseek(fp, pos, SEEK_SET);
}

uint32_t parse_ar_size(char *s, size_t maxlen) {
    uint32_t ret = 0;
    for(size_t i = 0; i < maxlen; i++) {
        if(s[i] < 48 || s[i] > 57) break;
        ret = ret * 10 + s[i] - 48;
    }
    return ret;
}

int parse_archive(FILE *fp, int platform) {
    int pos = ftell(fp);

    char magic[8];
    fread(&magic,sizeof(magic), 1, fp);
    if(0 != strncmp(magic, "!<arch>\n",8)) {
        printf("Doesn't look like an archive. Should start with '!<arch>\\n'\n");
        return 1;
    }

    ar_header entry;
    char ident[128];
    while(fread(&entry, sizeof(ar_header), 1, fp)) {
        size_t payload = parse_ar_size(entry.size, 10);
        // check if the identifiaction is > 16 chars.
        if(0 == strncmp(entry.ident, "#1/", 3)) {
            int ident_len = parse_ar_size(entry.ident+3, 16-3);
            fread(&ident, ident_len, 1, fp);
            payload -= ident_len;
        } else {
            strncpy(ident, entry.ident, 16);
        }
        printf("%6zu %s\n", payload, ident);
        // skip the __.SYMDEF and __.SYMDEF SORTED files.
        if(0 != strncmp(ident, "__.SYMDEF", 9))
            patch_object(fp, payload, platform);
        fseek(fp, payload, SEEK_CUR);
    }
    fseek(fp, pos, SEEK_SET);
    return 0;
}

int main(int argc, char ** argv) {

    opterr = 0;
    int c;
    int platform = PLATFORM_IOS;

    while ((c = getopt(argc, argv, "hs")) != -1) {
        switch (c)
        {
        case 'h':
            printf("mac2ios -- tool to rewrite the platform in object files\n");
            printf("Usage: mac2ios FILE\n");
            printf(" -h show this help message\n");
            printf(" -s set platform to IOSSIMULATOR\n");
            printf(" by default we set IOS\n");
            return EXIT_SUCCESS;

        case 's':
            platform = PLATFORM_IOSSIMULATOR;
            break;
        default:
            break;
        }
    }

    FILE *ptr;

    if(optind == argc) {
        printf("expecting FILE argument.");
        return EXIT_FAILURE;
    }

    ptr = fopen(argv[optind],"rb+");
    if(NULL == ptr) {
        printf("Failed to open %s\n", argv[1]);
        return EXIT_FAILURE;
    }

    int ret = parse_archive(ptr, platform);
    fclose(ptr);

    return ret;
}
