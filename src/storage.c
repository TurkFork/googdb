#include "storage.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/file.h>

/* ===== XTEA encryption ===== */

static void xtea_encipher(uint32_t v[2], const uint32_t key[4])
{
    uint32_t v0 = v[0], v1 = v[1], sum = 0, delta = 0x9E3779B9;
    for (int i = 0; i < 32; i++) {
        v0 += (((v1 << 4) ^ (v1 >> 5)) + v1) ^ (sum + key[sum & 3]);
        sum += delta;
        v1 += (((v0 << 4) ^ (v0 >> 5)) + v0) ^ (sum + key[(sum >> 11) & 3]);
    }
    v[0] = v0; v[1] = v1;
}

static void page_crypt(gb_page *page, uint32_t page_num, const uint32_t key[4])
{
    for (int i = 0; i < GB_PAGE_SIZE; i += 8) {
        uint32_t ctr[2] = { page_num, i / 8 };
        xtea_encipher(ctr, key);
        uint32_t *p = (uint32_t *)(page->data + i);
        p[0] ^= ctr[0];
        p[1] ^= ctr[1];
    }
}

static void derive_key(const char *password, uint32_t key[4])
{
    uint32_t h[4] = { 0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A };
    for (const char *p = password; *p; p++) {
        h[0] ^= (unsigned char)*p;
        h[1] ^= ((unsigned char)*p) << 3;
        h[2] ^= ((unsigned char)*p) << 7;
        h[3] ^= ((unsigned char)*p) << 13;
        h[0] += (h[1] ^ (h[1] >> 5)) + (h[2] ^ (h[2] << 4));
        h[1] += (h[2] ^ (h[2] >> 5)) + (h[3] ^ (h[3] << 4));
        h[2] += (h[3] ^ (h[3] >> 5)) + (h[0] ^ (h[0] << 4));
        h[3] += (h[0] ^ (h[0] >> 5)) + (h[1] ^ (h[1] << 4));
    }
    key[0] = h[0]; key[1] = h[1]; key[2] = h[2]; key[3] = h[3];
}

/* ===== Raw page I/O (no encryption) ===== */

static bool read_page_data(gb_storage *db, uint32_t page_num, gb_page *page)
{
    if (page_num >= db->page_count) return false;
    long offset = GB_HEADER_SIZE + (long)page_num * GB_PAGE_SIZE;
    if (fseek(db->file, offset, SEEK_SET) != 0) return false;
    return fread(page->data, GB_PAGE_SIZE, 1, db->file) == 1;
}

static bool write_page_data(gb_storage *db, uint32_t page_num, const gb_page *page)
{
    if (page_num >= db->page_count) return false;
    long offset = GB_HEADER_SIZE + (long)page_num * GB_PAGE_SIZE;
    if (fseek(db->file, offset, SEEK_SET) != 0) return false;
    if (fwrite(page->data, GB_PAGE_SIZE, 1, db->file) != 1) return false;
    return fflush(db->file) == 0;
}

/* ===== File header ===== */

static bool read_header(gb_storage *db)
{
    uint32_t magic;
    if (fseek(db->file, 0, SEEK_SET) != 0) return false;
    if (fread(&magic, sizeof(magic), 1, db->file) != 1) return false;
    if (magic != GB_MAGIC) return false;
    if (fread(&db->page_count, sizeof(db->page_count), 1, db->file) != 1) return false;
    if (fread(&db->flags, sizeof(db->flags), 1, db->file) != 1) return false;
    fseek(db->file, 4, SEEK_CUR);
    return true;
}

static bool write_header(gb_storage *db)
{
    uint32_t magic = GB_MAGIC;
    if (fseek(db->file, 0, SEEK_SET) != 0) return false;
    if (fwrite(&magic, sizeof(magic), 1, db->file) != 1) return false;
    if (fwrite(&db->page_count, sizeof(db->page_count), 1, db->file) != 1) return false;
    if (fwrite(&db->flags, sizeof(db->flags), 1, db->file) != 1) return false;
    uint32_t reserved = 0;
    if (fwrite(&reserved, sizeof(reserved), 1, db->file) != 1) return false;
    return fflush(db->file) == 0;
}

/* ===== Public storage API ===== */

gb_storage *gb_open(const char *path)
{
    gb_storage *db = calloc(1, sizeof(gb_storage));
    if (!db) return NULL;

    db->path = strdup(path);
    if (!db->path) { free(db); return NULL; }

    FILE *check = fopen(path, "rb");
    bool exists = (check != NULL);
    if (check) fclose(check);

    db->file = fopen(path, "r+b");
    if (!db->file && exists) { free(db->path); free(db); return NULL; }

    if (!db->file) {
        db->file = fopen(path, "w+b");
        if (!db->file) { free(db->path); free(db); return NULL; }
        setbuf(db->file, NULL);
        db->page_count = 1;
        db->flags = 0;
        db->encrypted = false;
        if (!write_header(db)) { gb_close(db); return NULL; }
        gb_page dir;
        memset(dir.data, 0, GB_PAGE_SIZE);
        dir.data[0] = GB_PAGE_DIR;
        *(uint32_t *)(dir.data + 4) = 0;
        if (!write_page_data(db, 0, &dir)) { gb_close(db); return NULL; }
    } else {
        setbuf(db->file, NULL);
        if (!read_header(db)) { gb_close(db); return NULL; }
        db->encrypted = false;
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
    if (!read_page_data(db, page_num, page)) return false;
    if (db->encrypted) page_crypt(page, page_num, db->key);
    return true;
}

bool gb_page_write(gb_storage *db, uint32_t page_num, const gb_page *page)
{
    gb_page copy = *page;
    if (db->encrypted) page_crypt(&copy, page_num, db->key);
    return write_page_data(db, page_num, &copy);
}

uint32_t gb_page_alloc(gb_storage *db)
{
    uint32_t num = db->page_count;
    db->page_count++;
    if (!write_header(db)) return UINT32_MAX;

    gb_page blank;
    memset(blank.data, 0, GB_PAGE_SIZE);
    if (db->encrypted) page_crypt(&blank, num, db->key);
    if (!write_page_data(db, num, &blank)) return UINT32_MAX;

    return num;
}

/* ===== File locking ===== */

void gb_lock(gb_storage *db)
{
    int r;
    do {
        r = flock(fileno(db->file), LOCK_EX);
    } while (r < 0 && errno == EINTR);
}

void gb_unlock(gb_storage *db)
{
    flock(fileno(db->file), LOCK_UN);
}

/* ===== Encryption API ===== */

bool gb_is_encrypted(gb_storage *db)
{
    return (db->flags & HEADER_ENCRYPTED) != 0;
}

void gb_encrypt_set(gb_storage *db, const char *password)
{
    uint32_t new_key[4];
    derive_key(password, new_key);

    if (db->encrypted) {
        for (uint32_t i = 0; i < db->page_count; i++) {
            gb_page page;
            if (!read_page_data(db, i, &page)) continue;
            page_crypt(&page, i, db->key);
            page_crypt(&page, i, new_key);
            write_page_data(db, i, &page);
        }
    } else if (db->flags & HEADER_ENCRYPTED) {
        memcpy(db->key, new_key, sizeof(new_key));
        db->encrypted = true;
        return;
    } else {
        for (uint32_t i = 0; i < db->page_count; i++) {
            gb_page page;
            if (!read_page_data(db, i, &page)) continue;
            page_crypt(&page, i, new_key);
            write_page_data(db, i, &page);
        }
    }

    memcpy(db->key, new_key, sizeof(new_key));
    db->encrypted = true;
    db->flags |= HEADER_ENCRYPTED;
    write_header(db);
}

void gb_encrypt_off(gb_storage *db)
{
    if (!db->encrypted) return;

    for (uint32_t i = 0; i < db->page_count; i++) {
        gb_page page;
        if (!read_page_data(db, i, &page)) continue;
        page_crypt(&page, i, db->key);
        write_page_data(db, i, &page);
    }

    db->encrypted = false;
    db->flags &= ~HEADER_ENCRYPTED;
    write_header(db);
}

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

    /* Allocate a table schema page */
    uint32_t tbl_page = gb_page_alloc(db);
    if (tbl_page == UINT32_MAX) return false;

    /* Write table schema page */
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

    /* Add entry to directory */
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
