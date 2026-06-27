#include "storage.h"
#include <stdlib.h>
#include <string.h>

#define GB_HEADER_SIZE 8 /* magic(4) + page_count(4) */

static bool read_header(gb_storage *db)
{
    uint32_t magic;
    if (fseek(db->file, 0, SEEK_SET) != 0)
        return false;
    if (fread(&magic, sizeof(magic), 1, db->file) != 1)
        return false;
    if (magic != GB_MAGIC)
        return false;
    if (fread(&db->page_count, sizeof(db->page_count), 1, db->file) != 1)
        return false;
    return true;
}

static bool write_header(gb_storage *db)
{
    uint32_t magic = GB_MAGIC;
    if (fseek(db->file, 0, SEEK_SET) != 0)
        return false;
    if (fwrite(&magic, sizeof(magic), 1, db->file) != 1)
        return false;
    if (fwrite(&db->page_count, sizeof(db->page_count), 1, db->file) != 1)
        return false;
    return fflush(db->file) == 0;
}

gb_storage *gb_open(const char *path)
{
    gb_storage *db = calloc(1, sizeof(gb_storage));
    if (!db) return NULL;

    db->path = strdup(path);
    if (!db->path) { free(db); return NULL; }

    bool exists = (fopen(path, "rb") != NULL);
    db->file = fopen(path, "r+b");
    if (!db->file && exists) { free(db->path); free(db); return NULL; }

    if (!db->file) {
        db->file = fopen(path, "w+b");
        if (!db->file) { free(db->path); free(db); return NULL; }
        db->page_count = 0;
        if (!write_header(db)) { gb_close(db); return NULL; }
    } else {
        if (!read_header(db)) { gb_close(db); return NULL; }
    }

    return db;
}

void gb_close(gb_storage *db)
{
    if (!db) return;
    if (db->file) fclose(db->file);
    free(db->path);
    free(db);
}

bool gb_page_read(gb_storage *db, uint32_t page_num, gb_page *page)
{
    if (page_num >= db->page_count) return false;
    long offset = GB_HEADER_SIZE + (long)page_num * GB_PAGE_SIZE;
    if (fseek(db->file, offset, SEEK_SET) != 0) return false;
    return fread(page->data, GB_PAGE_SIZE, 1, db->file) == 1;
}

bool gb_page_write(gb_storage *db, uint32_t page_num, const gb_page *page)
{
    if (page_num >= db->page_count) return false;
    long offset = GB_HEADER_SIZE + (long)page_num * GB_PAGE_SIZE;
    if (fseek(db->file, offset, SEEK_SET) != 0) return false;
    if (fwrite(page->data, GB_PAGE_SIZE, 1, db->file) != 1) return false;
    return fflush(db->file) == 0;
}

uint32_t gb_page_alloc(gb_storage *db)
{
    uint32_t num = db->page_count;
    db->page_count++;
    if (!write_header(db)) return UINT32_MAX;

    gb_page blank;
    memset(blank.data, 0, GB_PAGE_SIZE);
    long offset = GB_HEADER_SIZE + (long)num * GB_PAGE_SIZE;
    if (fseek(db->file, offset, SEEK_SET) != 0) return UINT32_MAX;
    if (fwrite(blank.data, GB_PAGE_SIZE, 1, db->file) != 1) return UINT32_MAX;
    if (fflush(db->file) != 0) return UINT32_MAX;

    return num;
}
