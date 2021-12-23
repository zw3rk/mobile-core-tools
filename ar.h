typedef struct _ar_header {
    char ident[16];  // name
    char mtime[12];
    char oid[6];
    char uid[6];
    char mode[8];
    char size[10]; // in decimal
    char end[2]; // 0x60 0x0A
} ar_header;