#include <stdio.h>
#include <string.h>
#include "storage.h"

int main(void)
{
    gb_storage *db = gb_open("test.gdb");
    if (!db) {
        fprintf(stderr, "failed to open database\n");
        return 1;
    }

    uint32_t pnum = gb_page_alloc(db);
    if (pnum == UINT32_MAX) {
        fprintf(stderr, "failed to allocate page\n");
        gb_close(db);
        return 1;
    }
    printf("allocated page %u\n", pnum);

    gb_page page;
    const char *msg = "Hello googdb!";
    memcpy(page.data, msg, strlen(msg) + 1);

    if (!gb_page_write(db, pnum, &page)) {
        fprintf(stderr, "failed to write page\n");
        gb_close(db);
        return 1;
    }
    printf("wrote: %s\n", page.data);

    gb_page page2;
    if (!gb_page_read(db, pnum, &page2)) {
        fprintf(stderr, "failed to read page\n");
        gb_close(db);
        return 1;
    }
    printf("read:  %s\n", page2.data);

    gb_close(db);
    return 0;
}
