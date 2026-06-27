#include "_storage.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ===== Schema / Types ===== */

int gb_type_size(int type)
{
    switch (type) {
        case GB_INT32:   return 4;
        case GB_UINT32:  return 4;
        case GB_FLOAT:   return 8;
        case GB_STRING:  return GB_STR_SIZE;
        default:         return 0;
    }
}

static int compute_row_size(const gb_column *cols, int ncols)
{
    int size = 0;
    for (int i = 0; i < ncols; i++)
        size += gb_type_size(cols[i].type);
    return size;
}

/* ===== Table directory (page 0) ===== */

static int dir_find(gb_storage *db, const char *name)
{
    gb_page page;
    if (!gb_page_read(db, 0, &page)) return -1;
    if (page.data[0] != GB_PAGE_DIR) return -1;
    uint32_t count = *(uint32_t *)(page.data + 4);
    for (uint32_t i = 0; i < count; i++) {
        char *entry_name = (char *)(page.data + 8 + i * (GB_MAX_NAME + 4));
        if (strcmp(entry_name, name) == 0)
            return i;
    }
    return -1;
}

static uint32_t dir_get_page(gb_storage *db, int index)
{
    gb_page page;
    if (!gb_page_read(db, 0, &page)) return 0;
    uint32_t *p = (uint32_t *)(page.data + 8 + index * (GB_MAX_NAME + 4) + GB_MAX_NAME);
    return *p;
}

/* ===== Table API ===== */

bool gb_create_table(gb_storage *db, const char *name, const gb_column *cols, int ncols)
{
    if (ncols <= 0 || ncols > GB_MAX_COLS) return false;

    uint32_t tbl_page = gb_page_alloc(db);
    if (tbl_page == UINT32_MAX) return false;

    gb_page page;
    memset(page.data, 0, GB_PAGE_SIZE);
    page.data[0] = GB_PAGE_TABLE;
    *(uint32_t *)(page.data + 4) = 0;
    *(uint32_t *)(page.data + 8) = ncols;
    for (int i = 0; i < ncols; i++) {
        char *dst = (char *)(page.data + 12 + i * (GB_MAX_NAME + 4));
        strncpy(dst, cols[i].name, GB_MAX_NAME - 1);
        *(uint32_t *)(dst + GB_MAX_NAME) = cols[i].type;
    }
    if (!gb_page_write(db, tbl_page, &page)) return false;

    gb_page dir;
    if (!gb_page_read(db, 0, &dir)) return false;
    if (dir.data[0] != GB_PAGE_DIR) return false;
    uint32_t count = *(uint32_t *)(dir.data + 4);
    if (count >= GB_DIR_CAPACITY) return false;

    char *entry = (char *)(dir.data + 8 + count * (GB_MAX_NAME + 4));
    strncpy(entry, name, GB_MAX_NAME - 1);
    *(uint32_t *)(entry + GB_MAX_NAME) = tbl_page;
    *(uint32_t *)(dir.data + 4) = count + 1;

    return gb_page_write(db, 0, &dir);
}

gb_schema *gb_open_table(gb_storage *db, const char *name)
{
    int idx = dir_find(db, name);
    if (idx < 0) return NULL;

    uint32_t tbl_page = dir_get_page(db, idx);
    if (tbl_page == 0) return NULL;

    gb_page page;
    if (!gb_page_read(db, tbl_page, &page)) return NULL;
    if (page.data[0] != GB_PAGE_TABLE) return NULL;

    uint32_t col_count = *(uint32_t *)(page.data + 8);
    if (col_count > GB_MAX_COLS) return NULL;

    gb_schema *tbl = calloc(1, sizeof(gb_schema));
    if (!tbl) return NULL;

    strncpy(tbl->name, name, GB_MAX_NAME - 1);
    tbl->col_count = col_count;
    tbl->first_data_page = *(uint32_t *)(page.data + 4);
    tbl->table_page = tbl_page;

    for (uint32_t i = 0; i < col_count; i++) {
        char *src = (char *)(page.data + 12 + i * (GB_MAX_NAME + 4));
        strncpy(tbl->cols[i].name, src, GB_MAX_NAME - 1);
        tbl->cols[i].type = *(uint32_t *)(src + GB_MAX_NAME);
    }

    tbl->row_size = compute_row_size(tbl->cols, tbl->col_count);
    return tbl;
}

void gb_close_table(gb_schema *tbl)
{
    free(tbl);
}

bool gb_insert(gb_storage *db, gb_schema *tbl, const void *row)
{
    uint32_t page_num = tbl->first_data_page;
    uint32_t prev = 0;

    while (page_num != 0) {
        gb_page page;
        if (!gb_page_read(db, page_num, &page)) return false;
        uint32_t next = *(uint32_t *)(page.data + 4);
        uint32_t count = *(uint32_t *)(page.data + 8);
        uint32_t cap = (GB_PAGE_SIZE - GB_DATA_HDR_SIZE) / (1 + tbl->row_size);

        if (count < cap) {
            uint32_t off = GB_DATA_HDR_SIZE + count * (1 + tbl->row_size);
            page.data[off] = 0x01;
            memcpy(page.data + off + 1, row, tbl->row_size);
            *(uint32_t *)(page.data + 8) = count + 1;
            return gb_page_write(db, page_num, &page);
        }
        prev = page_num;
        page_num = next;
    }

    uint32_t new_page = gb_page_alloc(db);
    if (new_page == UINT32_MAX) return false;

    gb_page page;
    memset(page.data, 0, GB_PAGE_SIZE);
    page.data[0] = GB_PAGE_DATA;
    page.data[GB_DATA_HDR_SIZE] = 0x01;
    memcpy(page.data + GB_DATA_HDR_SIZE + 1, row, tbl->row_size);
    *(uint32_t *)(page.data + 8) = 1;

    if (!gb_page_write(db, new_page, &page)) return false;

    if (prev != 0) {
        gb_page pp;
        if (!gb_page_read(db, prev, &pp)) return false;
        *(uint32_t *)(pp.data + 4) = new_page;
        if (!gb_page_write(db, prev, &pp)) return false;
    } else {
        tbl->first_data_page = new_page;
        gb_page tp;
        if (!gb_page_read(db, tbl->table_page, &tp)) return false;
        *(uint32_t *)(tp.data + 4) = new_page;
        if (!gb_page_write(db, tbl->table_page, &tp)) return false;
    }

    return true;
}

int gb_row_count(gb_storage *db, gb_schema *tbl)
{
    int total = 0;
    uint32_t page_num = tbl->first_data_page;
    while (page_num != 0) {
        gb_page page;
        if (!gb_page_read(db, page_num, &page)) return -1;
        uint32_t count = *(uint32_t *)(page.data + 8);
        uint32_t cap = (GB_PAGE_SIZE - GB_DATA_HDR_SIZE) / (1 + tbl->row_size);
        for (uint32_t i = 0; i < count && i < cap; i++) {
            uint32_t off = GB_DATA_HDR_SIZE + i * (1 + tbl->row_size);
            if (page.data[off] & 0x01) total++;
        }
        page_num = *(uint32_t *)(page.data + 4);
    }
    return total;
}

bool gb_select(gb_storage *db, gb_schema *tbl, int index, void *row)
{
    uint32_t page_num = tbl->first_data_page;
    int so_far = 0;

    while (page_num != 0) {
        gb_page page;
        if (!gb_page_read(db, page_num, &page)) return false;
        uint32_t count = *(uint32_t *)(page.data + 8);
        uint32_t cap = (GB_PAGE_SIZE - GB_DATA_HDR_SIZE) / (1 + tbl->row_size);

        for (uint32_t i = 0; i < count && i < cap; i++) {
            uint32_t off = GB_DATA_HDR_SIZE + i * (1 + tbl->row_size);
            if (!(page.data[off] & 0x01)) continue;
            if (so_far == index) {
                memcpy(row, page.data + off + 1, tbl->row_size);
                return true;
            }
            so_far++;
        }
        page_num = *(uint32_t *)(page.data + 4);
    }
    return false;
}

bool gb_delete_row(gb_storage *db, gb_schema *tbl, int index)
{
    uint32_t page_num = tbl->first_data_page;
    int so_far = 0;

    while (page_num != 0) {
        gb_page page;
        if (!gb_page_read(db, page_num, &page)) return false;
        uint32_t count = *(uint32_t *)(page.data + 8);
        uint32_t cap = (GB_PAGE_SIZE - GB_DATA_HDR_SIZE) / (1 + tbl->row_size);

        for (uint32_t i = 0; i < count && i < cap; i++) {
            uint32_t off = GB_DATA_HDR_SIZE + i * (1 + tbl->row_size);
            if (!(page.data[off] & 0x01)) continue;
            if (so_far == index) {
                page.data[off] = 0x00;
                return gb_page_write(db, page_num, &page);
            }
            so_far++;
        }
        page_num = *(uint32_t *)(page.data + 4);
    }
    return false;
}

void gb_list_tables(gb_storage *db)
{
    if (db->page_count == 0) return;
    gb_page page;
    if (!gb_page_read(db, 0, &page)) return;
    if (page.data[0] != GB_PAGE_DIR) return;
    uint32_t count = *(uint32_t *)(page.data + 4);
    for (uint32_t i = 0; i < count; i++) {
        char *name = (char *)(page.data + 8 + i * (GB_MAX_NAME + 4));
        printf("%s\n", name);
    }
}

/* ===== Print / Parse ===== */

void gb_print_row(gb_schema *tbl, const void *row)
{
    const unsigned char *data = (const unsigned char *)row;
    int offset = 0;
    for (uint32_t i = 0; i < tbl->col_count; i++) {
        if (i > 0) printf(" ");
        printf("%s=", tbl->cols[i].name);
        switch (tbl->cols[i].type) {
            case GB_INT32:
                printf("%d", *(int32_t *)(data + offset));
                offset += 4;
                break;
            case GB_UINT32:
                printf("%u", *(uint32_t *)(data + offset));
                offset += 4;
                break;
            case GB_FLOAT:
                printf("%g", *(double *)(data + offset));
                offset += 8;
                break;
            case GB_STRING:
                printf("'%s'", (const char *)(data + offset));
                offset += GB_STR_SIZE;
                break;
            default:
                offset += gb_type_size(tbl->cols[i].type);
                break;
        }
    }
}

int gb_parse_value(const char *str, int type, void *out)
{
    char *end;
    switch (type) {
        case GB_INT32:
            *((int32_t *)out) = (int32_t)strtol(str, &end, 10);
            return (*end == '\0' || *end == ')') ? 0 : -1;
        case GB_UINT32:
            *((uint32_t *)out) = (uint32_t)strtoul(str, &end, 10);
            return (*end == '\0' || *end == ')') ? 0 : -1;
        case GB_FLOAT:
            *((double *)out) = strtod(str, &end);
            return (*end == '\0' || *end == ')') ? 0 : -1;
        case GB_STRING: {
            const char *s = str;
            if (*s == '\'') s++;
            size_t slen = strlen(s);
            if (slen > 0 && s[slen - 1] == '\'') slen--;
            if (slen >= GB_STR_SIZE) slen = GB_STR_SIZE - 1;
            memcpy(out, s, slen);
            ((char *)out)[slen] = '\0';
            return 0;
        }
        default:
            return -1;
    }
}
