#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "storage.h"

#define MAX_TOKENS 64
#define DEFAULT_PORT 9606

#define OP_CREATE_TABLE 0x01
#define OP_INSERT       0x02
#define OP_SELECT       0x03
#define OP_DELETE       0x04
#define OP_PUT          0x05
#define OP_GET          0x06
#define OP_DEL          0x07
#define OP_ENCRYPT      0x08
#define OP_TABLES       0x09
#define OP_SCHEMA       0x0A
#define OP_EXIT         0xFF

#define STATUS_OK    0
#define STATUS_ERR   1

static int read_all(int fd, void *buf, size_t n)
{
    size_t total = 0;
    while (total < n) {
        ssize_t r = read(fd, (char*)buf + total, n - total);
        if (r <= 0) return -1;
        total += r;
    }
    return 0;
}

static int write_all(int fd, const void *buf, size_t n)
{
    size_t total = 0;
    while (total < n) {
        ssize_t w = write(fd, (const char*)buf + total, n - total);
        if (w <= 0) return -1;
        total += w;
    }
    return 0;
}

static uint32_t read_u32_be(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void write_u32_be(uint8_t *p, uint32_t v)
{
    p[0] = (v >> 24) & 0xFF;
    p[1] = (v >> 16) & 0xFF;
    p[2] = (v >> 8) & 0xFF;
    p[3] = v & 0xFF;
}

static void send_header(int fd, uint8_t status, uint32_t payload_len)
{
    uint8_t hdr[5];
    hdr[0] = status;
    write_u32_be(hdr + 1, payload_len);
    write_all(fd, hdr, 5);
}

static void send_error(int fd, const char *msg)
{
    size_t len = strlen(msg);
    if (len > 65535) len = 65535;
    send_header(fd, STATUS_ERR, (uint32_t)len);
    write_all(fd, msg, len);
}

static int strcmp_nocase(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return *a - *b;
}

static int tokenize(char *line, char **tokens, int max)
{
    int count = 0;
    while (*line) {
        while (*line == ' ' || *line == '\t') line++;
        if (!*line) break;
        if (*line == '\'') {
            line++;
            tokens[count++] = line;
            while (*line && *line != '\'') line++;
            if (*line) *line++ = '\0';
        } else {
            tokens[count++] = line;
            while (*line && *line != ' ' && *line != '\t') line++;
            if (*line) *line++ = '\0';
        }
        if (count >= max) break;
    }
    return count;
}

static int type_from_name(const char *s)
{
    char buf[32];
    strncpy(buf, s, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    size_t len = strlen(buf);
    while (len > 0 && (buf[len-1] == ',' || buf[len-1] == ')')) buf[--len] = '\0';
    if (strcmp_nocase(buf, "INT32") == 0 || strcmp_nocase(buf, "INT") == 0)
        return GB_INT32;
    if (strcmp_nocase(buf, "UINT32") == 0)
        return GB_UINT32;
    if (strcmp_nocase(buf, "FLOAT") == 0 || strcmp_nocase(buf, "DOUBLE") == 0)
        return GB_FLOAT;
    return GB_STRING;
}

static void strip_parens(char *s)
{
    size_t len = strlen(s);
    while (len > 0 && (s[len-1] == ',' || s[len-1] == ')')) s[--len] = '\0';
    if (len > 0 && s[0] == '(') memmove(s, s + 1, len);
}

static int cmd_create(gb_storage *db, char **tok, int n)
{
    if (n < 5) { printf("Usage: CREATE TABLE name (col TYPE ...)\n"); return -1; }
    char *name = tok[2];
    gb_column cols[GB_MAX_COLS];
    int ncols = 0;
    int i = 3;

    if (strcmp_nocase(tok[i], "TABLE") == 0) i++;
    strip_parens(tok[i]);

    while (i < n && ncols < GB_MAX_COLS) {
        char *col_name = tok[i++];
        if (i >= n) break;
        char *type_str = tok[i++];
        strip_parens(col_name);
        strip_parens(type_str);
        if (col_name[0] == '\0') continue;
        strncpy(cols[ncols].name, col_name, GB_MAX_NAME - 1);
        cols[ncols].type = type_from_name(type_str);
        ncols++;
    }

    if (ncols == 0) { printf("No columns defined\n"); return -1; }
    if (gb_create_table(db, name, cols, ncols)) {
        printf("OK\n");
        return 0;
    }
    printf("Failed to create table\n");
    return -1;
}

static int cmd_insert(gb_storage *db, char **tok, int n)
{
    if (n < 5) { printf("Usage: INSERT INTO name VALUES (val1, val2, ...)\n"); return -1; }
    char *tname = tok[2];

    gb_schema *tbl = gb_open_table(db, tname);
    if (!tbl) { printf("No such table: %s\n", tname); return -1; }

    int i = 3;
    while (i < n && strcmp_nocase(tok[i], "VALUES") != 0) i++;
    if (i >= n) { printf("Missing VALUES\n"); gb_close_table(tbl); return -1; }
    i++;

    unsigned char row[4096];
    memset(row, 0, sizeof(row));
    int offset = 0;
    int col = 0;

    for (; i < n && col < (int)tbl->col_count; i++) {
        strip_parens(tok[i]);
        if (tok[i][0] == '\0') continue;
        if (gb_parse_value(tok[i], tbl->cols[col].type, row + offset) != 0) {
            printf("Invalid value '%s' for column %s\n", tok[i], tbl->cols[col].name);
            gb_close_table(tbl);
            return -1;
        }
        offset += gb_type_size(tbl->cols[col].type);
        col++;
    }

    if (col != (int)tbl->col_count) {
        printf("Expected %d values, got %d\n", tbl->col_count, col);
        gb_close_table(tbl);
        return -1;
    }

    if (gb_insert(db, tbl, row))
        printf("OK\n");
    else
        printf("Insert failed\n");

    gb_close_table(tbl);
    return 0;
}

static int cmd_select(gb_storage *db, char **tok, int n)
{
    if (n < 4) { printf("Usage: SELECT * FROM name [WHERE col = val]\n"); return -1; }
    char *tname = tok[3];

    gb_schema *tbl = gb_open_table(db, tname);
    if (!tbl) { printf("No such table: %s\n", tname); return -1; }

    int where_col = -1, val_idx = -1;
    for (int i = 4; i < n; i++) {
        if (strcmp_nocase(tok[i], "WHERE") == 0 && i + 2 < n) {
            for (int c = 0; c < (int)tbl->col_count; c++) {
                if (strcmp(tok[i+1], tbl->cols[c].name) == 0) {
                    where_col = c;
                    val_idx = (i + 2 < n && strcmp(tok[i+2], "=") == 0) ? i + 3 : i + 2;
                    break;
                }
            }
            break;
        }
    }

    int count = gb_row_count(db, tbl);
    for (int i = 0; i < count; i++) {
        unsigned char row[4096];
        if (!gb_select(db, tbl, i, row)) break;

        if (where_col >= 0) {
            int off = 0;
            for (int c = 0; c < where_col; c++)
                off += gb_type_size(tbl->cols[c].type);
            int32_t wval = (int32_t)strtol(tok[val_idx], NULL, 10);
            if (tbl->cols[where_col].type == GB_INT32) {
                if (*(int32_t *)(row + off) != wval) continue;
            }
        }

        printf("[%d] ", i);
        gb_print_row(tbl, row);
        printf("\n");
    }

    gb_close_table(tbl);
    return 0;
}

static int cmd_delete(gb_storage *db, char **tok, int n)
{
    if (n < 6) { printf("Usage: DELETE FROM name WHERE col = val\n"); return -1; }
    char *tname = tok[2];

    gb_schema *tbl = gb_open_table(db, tname);
    if (!tbl) { printf("No such table: %s\n", tname); return -1; }

    int where_col = -1;
    int val_idx = -1;
    for (int i = 3; i < n; i++) {
        if (strcmp_nocase(tok[i], "WHERE") == 0 && i + 2 < n) {
            for (int c = 0; c < (int)tbl->col_count; c++) {
                if (strcmp(tok[i+1], tbl->cols[c].name) == 0) {
                    where_col = c;
                    val_idx = (i + 2 < n && strcmp(tok[i+2], "=") == 0) ? i + 3 : i + 2;
                    break;
                }
            }
            break;
        }
    }

    if (where_col < 0) {
        printf("No such column\n");
        gb_close_table(tbl);
        return -1;
    }

    int count = gb_row_count(db, tbl);
    int deleted = 0;
    for (int i = count - 1; i >= 0; i--) {
        unsigned char row[4096];
        if (!gb_select(db, tbl, i, row)) continue;
        int off = 0;
        for (int c = 0; c < where_col; c++) off += gb_type_size(tbl->cols[c].type);
        int32_t wval = (int32_t)strtol(tok[val_idx], NULL, 10);
        if (tbl->cols[where_col].type == GB_INT32 && *(int32_t *)(row + off) == wval) {
            if (gb_delete_row(db, tbl, i)) deleted++;
        }
    }

    printf("Deleted %d rows\n", deleted);
    gb_close_table(tbl);
    return 0;
}

static int cmd_put(gb_storage *db, char **tok, int n)
{
    if (n < 4) { printf("Usage: PUT bucket key [value]\n"); return -1; }
    char *bucket = tok[1];
    char *key = tok[2];
    char *value = n >= 4 ? tok[3] : "";

    if (gb_kv_put(db, bucket, key, value))
        printf("OK\n");
    else
        printf("PUT failed\n");
    return 0;
}

static int cmd_get(gb_storage *db, char **tok, int n)
{
    if (n < 3) { printf("Usage: GET bucket key\n"); return -1; }
    char *result = gb_kv_get(db, tok[1], tok[2]);
    if (result) {
        printf("%s\n", result);
        free(result);
    } else {
        printf("(not found)\n");
    }
    return 0;
}

static int cmd_del(gb_storage *db, char **tok, int n)
{
    if (n < 3) { printf("Usage: DEL bucket key\n"); return -1; }
    char *bucket = tok[1];
    char *key = tok[2];

    gb_schema *tbl = gb_open_table(db, bucket);
    if (!tbl) { printf("Bucket not found\n"); return -1; }

    int count = gb_row_count(db, tbl);
    int deleted = 0;
    for (int i = count - 1; i >= 0; i--) {
        unsigned char row[4096];
        if (!gb_select(db, tbl, i, row)) continue;
        if (strcmp((const char *)row, key) == 0) {
            if (gb_delete_row(db, tbl, i)) deleted++;
        }
    }

    printf("Deleted %d entries\n", deleted);
    gb_close_table(tbl);
    return 0;
}

static int cmd_encrypt(gb_storage *db, char **tok, int n)
{
    if (n >= 2 && strcmp_nocase(tok[1], "OFF") == 0) {
        gb_encrypt_off(db);
        printf("Encryption disabled\n");
        return 0;
    }
    if (n < 2) { printf("Usage: ENCRYPT password | ENCRYPT OFF\n"); return -1; }
    gb_encrypt_set(db, tok[1]);
    printf("Encryption enabled\n");
    return 0;
}

static void print_help(void)
{
    printf("Commands:\n");
    printf("  CREATE TABLE name (col TYPE ...)  Create a table\n");
    printf("  INSERT INTO name VALUES (...)      Insert a row\n");
    printf("  SELECT * FROM name [WHERE c = v]   Query rows\n");
    printf("  DELETE FROM name WHERE c = v       Delete rows\n");
    printf("  PUT bucket key [value]             Key-value set\n");
    printf("  GET bucket key                     Key-value get\n");
    printf("  DEL bucket key                     Key-value delete\n");
    printf("  ENCRYPT password                   Enable encryption\n");
    printf("  ENCRYPT OFF                        Disable encryption\n");
    printf("  .tables                            List tables\n");
    printf("  .help                              This help\n");
    printf("  .exit                              Exit\n");
}

static int dispatch(gb_storage *db, char *line)
{
    char *nl = strchr(line, '\n');
    if (nl) *nl = '\0';

    char *tok[MAX_TOKENS];
    int n = tokenize(line, tok, MAX_TOKENS);
    if (n == 0) return 0;

    if (strcmp(tok[0], ".exit") == 0 || strcmp(tok[0], ".quit") == 0)
        return 1;
    if (strcmp(tok[0], ".help") == 0)
        { print_help(); return 0; }
    if (strcmp(tok[0], ".tables") == 0)
        { gb_list_tables(db); return 0; }
    if (strcmp_nocase(tok[0], "CREATE") == 0 && strcmp_nocase(tok[1], "TABLE") == 0)
        { cmd_create(db, tok, n); return 0; }
    if (strcmp_nocase(tok[0], "INSERT") == 0)
        { cmd_insert(db, tok, n); return 0; }
    if (strcmp_nocase(tok[0], "SELECT") == 0)
        { cmd_select(db, tok, n); return 0; }
    if (strcmp_nocase(tok[0], "DELETE") == 0)
        { cmd_delete(db, tok, n); return 0; }
    if (strcmp_nocase(tok[0], "PUT") == 0)
        { cmd_put(db, tok, n); return 0; }
    if (strcmp_nocase(tok[0], "GET") == 0)
        { cmd_get(db, tok, n); return 0; }
    if (strcmp_nocase(tok[0], "DEL") == 0)
        { cmd_del(db, tok, n); return 0; }
    if (strcmp_nocase(tok[0], "ENCRYPT") == 0)
        { cmd_encrypt(db, tok, n); return 0; }
    if (strcmp(tok[0], ".schema") == 0 && n >= 2) {
        gb_schema *t = gb_open_table(db, tok[1]);
        if (t) {
            printf("TABLE %s (", t->name);
            for (uint32_t i = 0; i < t->col_count; i++) {
                if (i > 0) printf(", ");
                printf("%s ", t->cols[i].name);
                switch (t->cols[i].type) {
                    case GB_INT32: printf("INT32"); break;
                    case GB_UINT32: printf("UINT32"); break;
                    case GB_FLOAT: printf("FLOAT"); break;
                    case GB_STRING: printf("STRING"); break;
                }
            }
            printf(")\n");
            gb_close_table(t);
        } else printf("No such table\n");
        return 0;
    }
    printf("Unknown command. Try .help\n");
    return 0;
}

static void run_cli(gb_storage *db, bool show_prompt)
{
    if (gb_is_encrypted(db))
        printf("Database is encrypted. Use ENCRYPT <password> to unlock.\n");

    char line[4096];
    while (1) {
        if (show_prompt) {
            printf("googdb> ");
            fflush(stdout);
        }
        if (!fgets(line, sizeof(line), stdin)) break;
        if (dispatch(db, line)) break;
    }
}

static void print_usage(void)
{
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  googdb <database.gdb>              Interactive CLI\n");
    fprintf(stderr, "  googdb --serve <database.gdb>      TCP server (port %d)\n", DEFAULT_PORT);
    fprintf(stderr, "  googdb --serve <database.gdb> <p>  TCP server (custom port)\n");
}

static void endian_convert(void *buf, int type, int to_net)
{
    if (type == GB_INT32 || type == GB_UINT32) {
        uint32_t v;
        memcpy(&v, buf, 4);
        v = to_net ? htonl(v) : ntohl(v);
        memcpy(buf, &v, 4);
    } else if (type == GB_FLOAT) {
        uint8_t *b = buf;
        uint8_t tmp[8];
        memcpy(tmp, b, 8);
        for (int j = 0; j < 8; j++) b[j] = tmp[7 - j];
    }
}

static int handle_binary_request(gb_storage *db, int fd, int *done)
{
    uint8_t hdr[5];
    if (read_all(fd, hdr, 5) < 0) return -1;

    uint8_t op = hdr[0];
    uint32_t plen = read_u32_be(hdr + 1);

    uint8_t payload[65536];
    if (plen > sizeof(payload)) {
        send_error(fd, "payload too large");
        return 0;
    }
    if (plen > 0 && read_all(fd, payload, plen) < 0) return -1;

    /* Protocol column type values match storage.h: GB_INT32(1) .. GB_STRING(4) */
    size_t off = 0;

    switch (op) {
    case OP_CREATE_TABLE: {
        if (off >= plen) { send_error(fd, "bad table name"); break; }
        uint8_t tlen = payload[off++];
        if (tlen == 0 || off + tlen > plen) { send_error(fd, "bad table name"); break; }
        char tname[65];
        memcpy(tname, payload + off, tlen); tname[tlen] = 0; off += tlen;

        if (off >= plen) { send_error(fd, "bad col count"); break; }
        uint8_t ncols = payload[off++];
        if (ncols == 0 || ncols > GB_MAX_COLS) { send_error(fd, "bad col count"); break; }

        gb_column cols[GB_MAX_COLS];
        memset(cols, 0, sizeof(cols));
        int ok = 1;
        for (int i = 0; i < ncols && ok; i++) {
            if (off >= plen) { ok = 0; break; }
            uint8_t cl = payload[off++];
            if (cl == 0 || off + cl > plen) { ok = 0; break; }
            size_t cpl = cl < GB_MAX_NAME - 1 ? cl : GB_MAX_NAME - 1;
            memcpy(cols[i].name, payload + off, cpl); off += cl;
            if (off >= plen) { ok = 0; break; }
            uint8_t ct = payload[off++];
            if (ct < GB_INT32 || ct > GB_STRING) { ok = 0; break; }
            cols[i].type = ct;
        }
        if (!ok) { send_error(fd, "bad column def"); break; }

        if (gb_create_table(db, tname, cols, ncols))
            send_header(fd, STATUS_OK, 0);
        else
            send_error(fd, "CREATE TABLE failed");
        break;
    }

    case OP_INSERT: {
        if (off >= plen) { send_error(fd, "bad table name"); break; }
        uint8_t tlen = payload[off++];
        if (tlen == 0 || off + tlen > plen) { send_error(fd, "bad table name"); break; }
        char tname[65];
        memcpy(tname, payload + off, tlen); tname[tlen] = 0; off += tlen;

        gb_schema *tbl = gb_open_table(db, tname);
        if (!tbl) { send_error(fd, "no such table"); break; }

        if (off + 2 > plen) { gb_close_table(tbl); send_error(fd, "bad value count"); break; }
        uint16_t nvals = (uint16_t)payload[off] << 8 | payload[off+1]; off += 2;

        int ncols = (int)tbl->col_count;
        if (nvals != ncols) { gb_close_table(tbl); send_error(fd, "value count mismatch"); break; }

        uint8_t row[4096];
        size_t roff = 0;
        int vok = 1;
        for (int i = 0; i < ncols && vok; i++) {
            int tsz = gb_type_size(tbl->cols[i].type);
            if (off + tsz > plen) { vok = 0; break; }
            memcpy(row + roff, payload + off, tsz);
            endian_convert(row + roff, tbl->cols[i].type, 0);
            off += tsz; roff += tsz;
        }
        if (!vok) { gb_close_table(tbl); send_error(fd, "bad value data"); break; }

        if (gb_insert(db, tbl, row))
            send_header(fd, STATUS_OK, 0);
        else
            send_error(fd, "INSERT failed");

        gb_close_table(tbl);
        break;
    }

    case OP_SELECT: {
        if (off >= plen) { send_error(fd, "bad table name"); break; }
        uint8_t tlen = payload[off++];
        if (tlen == 0 || off + tlen > plen) { send_error(fd, "bad table name"); break; }
        char tname[65];
        memcpy(tname, payload + off, tlen); tname[tlen] = 0; off += tlen;

        gb_schema *tbl = gb_open_table(db, tname);
        if (!tbl) { send_error(fd, "no such table"); break; }

        /* Optional WHERE: [col_name_len: u8][col_name...][col_type: u8][col_value...] */
        int wcol = -1;
        uint8_t wtype = 0;
        uint8_t wbuf[256];
        size_t wlen = 0;
        if (off < plen) {
            uint8_t wcl = payload[off++];
            if (wcl > 0 && off + wcl <= plen) {
                char wname[65];
                memcpy(wname, payload + off, wcl); wname[wcl] = 0; off += wcl;
                if (off < plen) {
                    wtype = payload[off++];
                    int tsz = gb_type_size((int)wtype);
                    if (tsz > 0 && off + tsz <= plen) {
                        for (int c = 0; c < (int)tbl->col_count; c++) {
                            if (strcmp(wname, tbl->cols[c].name) == 0 &&
                                tbl->cols[c].type == (int)wtype) {
                                wcol = c; wlen = tsz;
                                memcpy(wbuf, payload + off, tsz);
                                endian_convert(wbuf, (int)wtype, 0);
                                break;
                            }
                        }
                        off += tsz;
                    }
                }
            }
        }

        int count = gb_row_count(db, tbl);
        int rsize = (int)tbl->row_size;
        uint8_t *out = malloc((count + 1) * 4 + count * rsize);
        if (!out) { gb_close_table(tbl); send_error(fd, "malloc failed"); break; }

        uint32_t n = 0;
        for (int i = 0; i < count; i++) {
            uint8_t row[4096];
            if (!gb_select(db, tbl, i, row)) break;
            if (wcol >= 0) {
                int col_off = 0;
                for (int c = 0; c < wcol; c++)
                    col_off += gb_type_size(tbl->cols[c].type);
                if (memcmp(row + col_off, wbuf, wlen) != 0) continue;
            }
            memcpy(out + 4 + n * rsize, row, rsize);
            n++;
        }

        write_u32_be(out, n);
        /* Convert numeric fields to network byte order */
        for (uint32_t r = 0; r < n; r++) {
            int col_off = 0;
            for (uint32_t c = 0; c < tbl->col_count; c++) {
                int tsz = gb_type_size(tbl->cols[c].type);
                endian_convert(out + 4 + r * rsize + col_off, tbl->cols[c].type, 1);
                col_off += tsz;
            }
        }
        uint32_t resp_len = 4 + n * rsize;
        send_header(fd, STATUS_OK, resp_len);
        write_all(fd, out, resp_len);
        free(out);
        gb_close_table(tbl);
        break;
    }

    case OP_DELETE: {
        if (off >= plen) { send_error(fd, "bad table name"); break; }
        uint8_t tlen = payload[off++];
        if (tlen == 0 || off + tlen > plen) { send_error(fd, "bad table name"); break; }
        char tname[65];
        memcpy(tname, payload + off, tlen); tname[tlen] = 0; off += tlen;

        gb_schema *tbl = gb_open_table(db, tname);
        if (!tbl) { send_error(fd, "no such table"); break; }

        /* Optional WHERE (same format as SELECT) */
        int wcol = -1;
        uint8_t wtype = 0;
        uint8_t wbuf[256];
        if (off < plen) {
            uint8_t wcl = payload[off++];
            if (wcl > 0 && off + wcl <= plen) {
                char wname[65];
                memcpy(wname, payload + off, wcl); wname[wcl] = 0; off += wcl;
                if (off < plen) {
                    wtype = payload[off++];
                    int tsz = gb_type_size((int)wtype);
                    if (tsz > 0 && off + tsz <= plen) {
                        for (int c = 0; c < (int)tbl->col_count; c++) {
                            if (strcmp(wname, tbl->cols[c].name) == 0 &&
                                tbl->cols[c].type == (int)wtype) {
                                wcol = c;
                                memcpy(wbuf, payload + off, tsz);
                                endian_convert(wbuf, (int)wtype, 0);
                                break;
                            }
                        }
                        off += tsz;
                    }
                }
            }
        }

        if (wcol < 0) { gb_close_table(tbl); send_error(fd, "WHERE clause required"); break; }

        int count = gb_row_count(db, tbl);
        int deleted = 0;
        for (int i = count - 1; i >= 0; i--) {
            uint8_t row[4096];
            if (!gb_select(db, tbl, i, row)) continue;
            int col_off = 0;
            for (int c = 0; c < wcol; c++)
                col_off += gb_type_size(tbl->cols[c].type);
            if (memcmp(row + col_off, wbuf, gb_type_size(wtype)) == 0) {
                if (gb_delete_row(db, tbl, i)) deleted++;
            }
        }

        uint8_t r[4];
        write_u32_be(r, (uint32_t)deleted);
        send_header(fd, STATUS_OK, 4);
        write_all(fd, r, 4);
        gb_close_table(tbl);
        break;
    }

    case OP_PUT: {
        if (off >= plen) { send_error(fd, "bad bucket name"); break; }
        uint8_t blen = payload[off++];
        if (blen == 0 || off + blen > plen) { send_error(fd, "bad bucket name"); break; }
        char bucket[65];
        memcpy(bucket, payload + off, blen); bucket[blen] = 0; off += blen;

        uint16_t klen = 0;
        if (off + 2 > plen) { send_error(fd, "bad key"); break; }
        klen = (uint16_t)payload[off] << 8 | payload[off+1]; off += 2;
        if (klen > 255 || off + klen > plen) { send_error(fd, "key too long"); break; }
        char key[256];
        memcpy(key, payload + off, klen); key[klen] = 0; off += klen;

        uint32_t vlen = 0;
        if (off + 4 > plen) { send_error(fd, "bad value"); break; }
        vlen = read_u32_be(payload + off); off += 4;
        if (vlen > 65535 || off + vlen > plen) { send_error(fd, "value too long"); break; }
        char val[65536];
        memcpy(val, payload + off, vlen); val[vlen] = 0; off += vlen;

        /* Use existing string-based KV API (limited to 255-byte key/value) */
        if (gb_kv_put(db, bucket, key, val))
            send_header(fd, STATUS_OK, 0);
        else
            send_error(fd, "PUT failed");
        break;
    }

    case OP_GET: {
        if (off >= plen) { send_error(fd, "bad bucket name"); break; }
        uint8_t blen = payload[off++];
        if (blen == 0 || off + blen > plen) { send_error(fd, "bad bucket name"); break; }
        char bucket[65];
        memcpy(bucket, payload + off, blen); bucket[blen] = 0; off += blen;

        uint16_t klen = 0;
        if (off + 2 > plen) { send_error(fd, "bad key"); break; }
        klen = (uint16_t)payload[off] << 8 | payload[off+1]; off += 2;
        if (klen > 255 || off + klen > plen) { send_error(fd, "key too long"); break; }
        char key[256];
        memcpy(key, payload + off, klen); key[klen] = 0; off += klen;

        char *result = gb_kv_get(db, bucket, key);
        if (result) {
            size_t rlen = strlen(result);
            if (rlen > UINT32_MAX) rlen = UINT32_MAX;
            uint8_t rhdr[4];
            write_u32_be(rhdr, (uint32_t)rlen);
            send_header(fd, STATUS_OK, 4 + (uint32_t)rlen);
            write_all(fd, rhdr, 4);
            if (rlen > 0) write_all(fd, result, rlen);
            free(result);
        } else {
            send_error(fd, "not found");
        }
        break;
    }

    case OP_DEL: {
        if (off >= plen) { send_error(fd, "bad bucket name"); break; }
        uint8_t blen = payload[off++];
        if (blen == 0 || off + blen > plen) { send_error(fd, "bad bucket name"); break; }
        char bucket[65];
        memcpy(bucket, payload + off, blen); bucket[blen] = 0; off += blen;

        uint16_t klen = 0;
        if (off + 2 > plen) { send_error(fd, "bad key"); break; }
        klen = (uint16_t)payload[off] << 8 | payload[off+1]; off += 2;
        if (klen > 255 || off + klen > plen) { send_error(fd, "key too long"); break; }
        char key[256];
        memcpy(key, payload + off, klen); key[klen] = 0; off += klen;

        gb_schema *tbl = gb_open_table(db, bucket);
        if (!tbl) { send_error(fd, "bucket not found"); break; }

        int count = gb_row_count(db, tbl);
        int deleted = 0;
        for (int i = count - 1; i >= 0; i--) {
            uint8_t row[4096];
            if (!gb_select(db, tbl, i, row)) continue;
            if (memcmp(row, key, strlen(key) + 1) == 0) {
                if (gb_delete_row(db, tbl, i)) deleted++;
            }
        }
        gb_close_table(tbl);

        uint8_t r[4];
        write_u32_be(r, (uint32_t)deleted);
        send_header(fd, STATUS_OK, 4);
        write_all(fd, r, 4);
        break;
    }

    case OP_ENCRYPT: {
        size_t pwlen = plen - off;
        if (pwlen == 0) {
            gb_encrypt_off(db);
            send_header(fd, STATUS_OK, 0);
        } else {
            char pw[256];
            size_t copylen = pwlen < 255 ? pwlen : 255;
            memcpy(pw, payload + off, copylen);
            pw[copylen] = 0;
            gb_encrypt_set(db, pw);
            send_header(fd, STATUS_OK, 0);
        }
        break;
    }

    case OP_TABLES: {
        /* Read directory page 0 directly */
        gb_page page;
        if (!gb_page_read(db, 0, &page)) {
            send_error(fd, "can't read directory");
            break;
        }
        if (page.data[0] != GB_PAGE_DIR) {
            send_error(fd, "bad directory page");
            break;
        }
        uint32_t count;
        memcpy(&count, page.data + 4, sizeof(count));
        uint8_t resp[4096];
        size_t rp = 0;
        resp[rp++] = (uint8_t)(count > 255 ? 255 : count);
        uint32_t max = count > 255 ? 255 : count;
        for (uint32_t i = 0; i < max; i++) {
            char *name = (char *)(page.data + 8 + i * (GB_MAX_NAME + 4));
            size_t nl = strlen(name);
            uint8_t nl8 = nl < 255 ? (uint8_t)nl : 255;
            resp[rp++] = nl8;
            memcpy(resp + rp, name, nl8);
            rp += nl8;
        }
        send_header(fd, STATUS_OK, (uint32_t)rp);
        write_all(fd, resp, rp);
        break;
    }

    case OP_SCHEMA: {
        if (off >= plen) { send_error(fd, "bad table name"); break; }
        uint8_t tlen = payload[off++];
        if (tlen == 0 || off + tlen > plen) { send_error(fd, "bad table name"); break; }
        char tname[65];
        memcpy(tname, payload + off, tlen); tname[tlen] = 0; off += tlen;

        gb_schema *tbl = gb_open_table(db, tname);
        if (!tbl) { send_error(fd, "no such table"); break; }

        uint8_t resp[4096];
        size_t rp = 0;
        size_t nl = strlen(tbl->name);
        uint8_t nl8 = nl < 255 ? (uint8_t)nl : 255;
        resp[rp++] = nl8;
        memcpy(resp + rp, tbl->name, nl8); rp += nl8;
        resp[rp++] = (uint8_t)tbl->col_count;
        for (uint32_t i = 0; i < tbl->col_count; i++) {
            size_t cl = strlen(tbl->cols[i].name);
            uint8_t cl8 = cl < 255 ? (uint8_t)cl : 255;
            resp[rp++] = cl8;
            memcpy(resp + rp, tbl->cols[i].name, cl8); rp += cl8;
            resp[rp++] = (uint8_t)tbl->cols[i].type;
        }
        send_header(fd, STATUS_OK, (uint32_t)rp);
        write_all(fd, resp, rp);
        gb_close_table(tbl);
        break;
    }

    case OP_EXIT:
        *done = 1;
        send_header(fd, STATUS_OK, 0);
        break;

    default:
        send_error(fd, "unknown opcode");
        break;
    }
    return 0;
}

static int run_server(const char *db_path, int port)
{
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 16) < 0) {
        perror("listen");
        close(server_fd);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    gb_storage *db = gb_open(db_path);
    if (!db) {
        fprintf(stderr, "Failed to open database: %s\n", db_path);
        close(server_fd);
        return 1;
    }

    printf("googdb server listening on port %d (db: %s)\n", port, db_path);
    fflush(stdout);

    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }

        int done = 0;
        while (handle_binary_request(db, client_fd, &done) >= 0 && !done)
            ;
        close(client_fd);
    }

    gb_close(db);
    close(server_fd);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        print_usage();
        return 1;
    }

    if (strcmp(argv[1], "--serve") == 0) {
        if (argc < 3) { print_usage(); return 1; }
        char *db_path = argv[2];
        int port = (argc >= 4) ? atoi(argv[3]) : DEFAULT_PORT;
        if (port <= 0) { fprintf(stderr, "Invalid port\n"); return 1; }
        return run_server(db_path, port);
    }

    gb_storage *db = gb_open(argv[1]);
    if (!db) {
        fprintf(stderr, "Failed to open database: %s\n", argv[1]);
        return 1;
    }

    run_cli(db, true);
    gb_close(db);
    return 0;
}
