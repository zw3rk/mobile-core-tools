#include <stdio.h>
#include <stdint.h>
#include <string.h>
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

void patch_object(FILE *fp, size_t len) {
    int pos = ftell(fp);

    fread(buffer,sizeof(buffer),1,fp);

    mach_header_64 *hdr = (mach_header_64*)buffer;

    size_t offset = sizeof(mach_header_64);
    load_command *cmd;
    while(offset < sizeof(mach_header_64) + hdr->sizeofcmds) {
        cmd = (load_command*)(buffer+offset);
        switch(cmd->cmd) {
            case LC_BUILD_VERSION: {
                struct build_version_command* bvc = (struct build_version_command*)(buffer+offset);
                printf("Chaning platform from: %d -> %d\n", bvc->platform, PLATFORM_IOS);
                bvc->platform = PLATFORM_IOS;
                fseek(fp, pos+offset, SEEK_SET);
                fwrite(buffer+offset, sizeof(struct build_version_command), 1, fp);
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

int parse_archive(FILE *fp) {
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
        patch_object(fp, payload);
        fseek(fp, payload, SEEK_CUR);
    }
    fseek(fp, pos, SEEK_SET);
    return 0;
}

int main(int argc, char ** argv) {

    FILE *ptr;

    ptr = fopen(argv[1],"rb+");
    if(NULL == ptr) {
        printf("Failed to open %s\n", argv[1]);
        return 1;
    }

    int ret = parse_archive(ptr);
    // patch_object(fp);
    fclose(ptr);
    return ret;
}