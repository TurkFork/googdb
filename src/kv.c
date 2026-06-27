#include "_storage.h"
#include <stdlib.h>
#include <string.h>

/* ===== Key-value convenience ===== */

bool gb_kv_put(gb_storage *db, const char *bucket, const char *key, const char *value)
{
    gb_schema *tbl = gb_open_table(db, bucket);
    if (!tbl) {
        gb_column cols[2];
        strncpy(cols[0].name, "key", GB_MAX_NAME - 1);
        cols[0].type = GB_STRING;
        strncpy(cols[1].name, "value", GB_MAX_NAME - 1);
        cols[1].type = GB_STRING;
        if (!gb_create_table(db, bucket, cols, 2)) return false;
        tbl = gb_open_table(db, bucket);
        if (!tbl) return false;
    }

    unsigned char row[4096] = {0};
    strncpy((char *)row, key, GB_STR_SIZE - 1);
    strncpy((char *)(row + GB_STR_SIZE), value, GB_STR_SIZE - 1);
    bool ok = gb_insert(db, tbl, row);
    gb_close_table(tbl);
    return ok;
}

char *gb_kv_get(gb_storage *db, const char *bucket, const char *key)
{
    gb_schema *tbl = gb_open_table(db, bucket);
    if (!tbl) return NULL;

    int count = gb_row_count(db, tbl);
    char *result = NULL;

    for (int i = 0; i < count; i++) {
        unsigned char row[4096];
        if (!gb_select(db, tbl, i, row)) break;
        if (strcmp((const char *)row, key) == 0) {
            free(result);
            result = strdup((const char *)(row + GB_STR_SIZE));
        }
    }

    gb_close_table(tbl);
    return result;
}
