#ifndef GB_STORAGE_H
#define GB_STORAGE_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>

#define GB_PAGE_SIZE     4096
#define GB_HEADER_SIZE   16
#define GB_MAGIC         0x474F4F47
#define GB_MAX_NAME      64
#define GB_MAX_COLS      32
#define GB_STR_SIZE      256
#define GB_DATA_HDR_SIZE 12
#define GB_DIR_CAPACITY  60

#define GB_PAGE_FREE   0x00
#define GB_PAGE_DIR    0x01
#define GB_PAGE_TABLE  0x02
#define GB_PAGE_DATA   0x03

#define GB_INT32   1
#define GB_UINT32  2
#define GB_FLOAT   3
#define GB_STRING  4

#define HEADER_ENCRYPTED 0x00000001

typedef struct {
    char  name[GB_MAX_NAME];
    int   type;
} gb_column;

typedef struct {
    char      name[GB_MAX_NAME];
    uint32_t  col_count;
    gb_column cols[GB_MAX_COLS];
    uint32_t  row_size;
    uint32_t  first_data_page;
    uint32_t  table_page;
} gb_schema;

typedef struct {
    unsigned char data[GB_PAGE_SIZE];
} gb_page;

typedef struct {
    FILE     *file;
    uint32_t  page_count;
    uint32_t  flags;
    char     *path;
    bool      encrypted;
    uint32_t  key[4];
    char     *lock_path;
    time_t    last_hb;
} gb_storage;

/* Storage layer */
gb_storage *gb_open(const char *path);
void        gb_close(gb_storage *db);
bool        gb_page_read(gb_storage *db, uint32_t page_num, gb_page *page);
bool        gb_page_write(gb_storage *db, uint32_t page_num, const gb_page *page);
uint32_t    gb_page_alloc(gb_storage *db);
void        gb_lock(gb_storage *db);
void        gb_unlock(gb_storage *db);
void        gb_lock_heartbeat(gb_storage *db);

/* Encryption */
void gb_encrypt_set(gb_storage *db, const char *password);
void gb_encrypt_off(gb_storage *db);
bool gb_is_encrypted(gb_storage *db);

/* Table operations */
bool      gb_create_table(gb_storage *db, const char *name, const gb_column *cols, int ncols);
gb_schema *gb_open_table(gb_storage *db, const char *name);
void      gb_close_table(gb_schema *tbl);
bool      gb_insert(gb_storage *db, gb_schema *tbl, const void *row);
int       gb_row_count(gb_storage *db, gb_schema *tbl);
bool      gb_select(gb_storage *db, gb_schema *tbl, int index, void *row);
bool      gb_delete_row(gb_storage *db, gb_schema *tbl, int index);
void      gb_list_tables(gb_storage *db);
int       gb_type_size(int type);

/* Key-value */
bool gb_kv_put(gb_storage *db, const char *bucket, const char *key, const char *value);
char *gb_kv_get(gb_storage *db, const char *bucket, const char *key);

/* Helpers */
void gb_print_row(gb_schema *tbl, const void *row);
int  gb_parse_value(const char *str, int type, void *out);

#endif
