#ifndef GOOGBASE_STORAGE_H
#define GOOGBASE_STORAGE_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#define GB_PAGE_SIZE   4096
#define GB_MAGIC       0x474F4F47 /* "GOOG" */

typedef struct {
    unsigned char data[GB_PAGE_SIZE];
} gb_page;

typedef struct {
    FILE      *file;
    uint32_t  page_count;
    char      *path;
} gb_storage;

gb_storage *gb_open(const char *path);
void        gb_close(gb_storage *db);
bool        gb_page_read(gb_storage *db, uint32_t page_num, gb_page *page);
bool        gb_page_write(gb_storage *db, uint32_t page_num, const gb_page *page);
uint32_t    gb_page_alloc(gb_storage *db);

#endif
