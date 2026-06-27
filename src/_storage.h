#ifndef GB_STORAGE_PRIVATE_H
#define GB_STORAGE_PRIVATE_H

#include "storage.h"

/* Raw page I/O (no encryption) */
bool read_page_data(gb_storage *db, uint32_t page_num, gb_page *page);
bool write_page_data(gb_storage *db, uint32_t page_num, const gb_page *page);

/* Encryption internals */
void page_crypt(gb_page *page, uint32_t page_num, const uint32_t key[4]);

/* File header */
bool read_header(gb_storage *db);
bool write_header(gb_storage *db);

#endif
