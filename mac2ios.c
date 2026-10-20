#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
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

/* Byte access keeps archive members safe even when they are unaligned. */
static uint32_t read_u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void write_u32(uint8_t *p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8 * i));
}

static int legacy_version(uint32_t cmd) {
    return cmd == LC_VERSION_MIN_MACOSX || cmd == LC_VERSION_MIN_IPHONEOS ||
           cmd == LC_VERSION_MIN_TVOS || cmd == LC_VERSION_MIN_WATCHOS;
}

int patch_object(FILE *fp, size_t len, int platform) {
    long pos = ftell(fp);
    size_t amount = len < sizeof(buffer) ? len : sizeof(buffer);
    if(pos < 0 || fread(buffer, 1, amount, fp) != amount) return 1;
    if(len < 4) return fseek(fp, pos, SEEK_SET);

    mach_header_64 header = {0};
    mach_header_64 *hdr = &header;
    hdr->magic = read_u32(buffer);

    if (!(  hdr->magic == 0xfeedface      // 32-bit
         || hdr->magic == 0xfeedfacf )) { // 64-bit
      // Object is not Mach-O
      return fseek(fp, pos, SEEK_SET);
    }

    size_t header_size = hdr->magic == 0xfeedfacf ? 32 : 28;
    if(amount < header_size) goto malformed;
    hdr->ncmds = read_u32(buffer+16);
    hdr->sizeofcmds = read_u32(buffer+20);
    if(hdr->sizeofcmds > amount-header_size || hdr->ncmds > hdr->sizeofcmds/8)
        goto malformed;
    size_t end = header_size + hdr->sizeofcmds;
    size_t offset = header_size;
    load_command command;
    load_command *cmd = &command;
    uint32_t builds = 0, removed = 0;
    /* Validate every command before writing any part of this member. */
    for(uint32_t i = 0; i < hdr->ncmds; ++i) {
        if(end-offset < 8) goto malformed;
        cmd->cmd = read_u32(buffer+offset);
        cmd->cmdsize = read_u32(buffer+offset+4);
        if(cmd->cmdsize < 8 || cmd->cmdsize % (header_size == 32 ? 8 : 4) ||
           cmd->cmdsize > end-offset) goto malformed;
        if(legacy_version(cmd->cmd) && cmd->cmdsize != sizeof(struct version_min_command))
            goto malformed;
        if(cmd->cmd == LC_BUILD_VERSION) {
            if(cmd->cmdsize < sizeof(struct build_version_command) ||
               read_u32(buffer+offset+20) !=
               (cmd->cmdsize-sizeof(struct build_version_command))/sizeof(struct build_tool_version))
                goto malformed;
            ++builds;
        }
        offset += cmd->cmdsize;
    }
    if(offset != end || builds > 1) goto malformed;

    offset = header_size;
    size_t output = header_size;
    while(offset < end) {
        cmd->cmd = read_u32(buffer+offset);
        cmd->cmdsize = read_u32(buffer+offset+4);
        if(builds && legacy_version(cmd->cmd)) {
            ++removed;
            offset += cmd->cmdsize;
            continue;
        }
        memmove(buffer+output, buffer+offset, cmd->cmdsize);
        switch(cmd->cmd) {
            /* old style LC_VERSION_MIN_<platofrm> rewrites */
            case LC_VERSION_MIN_MACOSX: {
                cmd->cmd = LC_VERSION_MIN_IPHONEOS;
                printf("Changing LC_VERSION_MIN_ from: %d -> %d\n", LC_VERSION_MIN_MACOSX, LC_VERSION_MIN_IPHONEOS);
                write_u32(buffer+output, cmd->cmd);
                break;
            }
            /* new style LC_BUILD_VERSION rewrites */
            case LC_BUILD_VERSION: {
                printf("Changing platform from: %d -> %d\n", read_u32(buffer+output+8), platform);
                write_u32(buffer+output+8, (uint32_t)platform);
                break;
            }
            default: break;
        }
        offset += cmd->cmdsize;
        output += cmd->cmdsize;
    }
    /* Compact commands only: payload, symbol offsets and member size stay put. */
    write_u32(buffer+16, hdr->ncmds-removed);
    write_u32(buffer+20, (uint32_t)(output-header_size));
    memset(buffer+output, 0, end-output);
    if(fseek(fp, pos, SEEK_SET) || fwrite(buffer, 1, end, fp) != end) return 1;
    return fseek(fp, pos, SEEK_SET);
malformed:
    fprintf(stderr, "Malformed Mach-O load commands\n");
    return 1;
}

uint32_t parse_ar_size(char *s, size_t maxlen) {
    uint32_t ret = 0;
    for(size_t i = 0; i < maxlen; i++) {
        if(s[i] < 48 || s[i] > 57) break;
        if(ret > (UINT32_MAX - (uint32_t)(s[i]-48))/10) return UINT32_MAX;
        ret = ret * 10 + s[i] - 48;
    }
    return ret;
}

int parse_archive(FILE *fp, int platform) {
    long pos = ftell(fp), archive_end;
    if(pos < 0 || fseek(fp, 0, SEEK_END) || (archive_end = ftell(fp)) < 0 ||
       fseek(fp, pos, SEEK_SET)) return 1;

    char magic[8];
    if(fread(&magic,sizeof(magic), 1, fp) != 1 ||
       0 != strncmp(magic, "!<arch>\n",8)) {
        printf("Doesn't look like an archive. Should start with '!<arch>\\n'\n");
        return 1;
    }

    ar_header entry;
    char ident[128];
    size_t nread;
    while((nread = fread(&entry, 1, sizeof(ar_header), fp)) != 0) {
        if(nread != sizeof(ar_header)) return 1;
        size_t payload = parse_ar_size(entry.size, 10);
        long data_pos = ftell(fp);
        size_t padding = payload % 2;
        if(memcmp(entry.end, "`\n", 2) ||
           payload == UINT32_MAX || data_pos < 0 || data_pos > archive_end ||
           payload > (size_t)(archive_end-data_pos) ||
           padding > (size_t)(archive_end-data_pos)-payload) return 1;
        // check if the identifiaction is > 16 chars.
        if(0 == strncmp(entry.ident, "#1/", 3)) {
            size_t ident_len = parse_ar_size(entry.ident+3, 16-3);
            size_t shown = ident_len < sizeof(ident)-1 ? ident_len : sizeof(ident)-1;
            if(ident_len > payload || fread(ident, 1, shown, fp) != shown ||
               fseek(fp, data_pos+(long)ident_len, SEEK_SET)) return 1;
            ident[shown] = '\0';
            payload -= ident_len;
        } else {
            memcpy(ident, entry.ident, 16);
            ident[16] = '\0';
        }
        printf("%6zu %s\n", payload, ident);
        // skip the __.SYMDEF and __.SYMDEF SORTED files.
        if(0 != strncmp(ident, "__.SYMDEF", 9) && patch_object(fp, payload, platform))
            return 1;
        if(fseek(fp, (long)(payload+padding), SEEK_CUR)) return 1;
    }
    if(ferror(fp)) return 1;
    return fseek(fp, pos, SEEK_SET);
}

int patch_file(FILE *fp, int platform) {
    uint8_t magic[8];
    long len;
    if(fseek(fp, 0, SEEK_END) || (len = ftell(fp)) < 0 ||
       (uintmax_t)len > SIZE_MAX || fseek(fp, 0, SEEK_SET)) return 1;
    size_t nread = fread(magic, 1, sizeof(magic), fp);
    if(ferror(fp) || fseek(fp, 0, SEEK_SET)) return 1;
    if(nread == 8 && memcmp(magic, "!<arch>\n", 8) == 0)
        return parse_archive(fp, platform);
    if(nread >= 4 && (read_u32(magic) == 0xfeedface || read_u32(magic) == 0xfeedfacf))
        return patch_object(fp, (size_t)len, platform);
    fprintf(stderr, "Expected an ar archive or a thin little-endian Mach-O file\n");
    return 1;
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
            printf("Usage: mac2ios [-s] FILE.a|FILE.o (detected by content)\n");
            printf(" -h show this help message\n");
            printf(" -s set platform to IOSSIMULATOR\n");
            printf(" by default we set IOS\n");
            return EXIT_SUCCESS;

        case 's':
            platform = PLATFORM_IOSSIMULATOR;
            break;
        default:
            return EXIT_FAILURE;
        }
    }

    FILE *ptr;

    if(optind + 1 != argc) {
        printf("expecting FILE argument.\n");
        return EXIT_FAILURE;
    }

    ptr = fopen(argv[optind],"rb+");
    if(NULL == ptr) {
        printf("Failed to open %s\n", argv[optind]);
        return EXIT_FAILURE;
    }

    int ret = patch_file(ptr, platform);
    if(fclose(ptr)) ret = 1;

    return ret;
}
