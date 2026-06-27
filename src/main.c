#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "storage.h"

#define MAX_TOKENS 64

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

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: googdb <database.gdb>\n");
        return 1;
    }

    gb_storage *db = gb_open(argv[1]);
    if (!db) {
        fprintf(stderr, "Failed to open database: %s\n", argv[1]);
        return 1;
    }

    if (gb_is_encrypted(db)) {
        printf("Database is encrypted. Use ENCRYPT <password> to unlock.\n");
    }

    char line[4096];
    while (1) {
        printf("googdb> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;

        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        char *tok[MAX_TOKENS];
        int n = tokenize(line, tok, MAX_TOKENS);
        if (n == 0) continue;

        if (strcmp(tok[0], ".exit") == 0 || strcmp(tok[0], ".quit") == 0)
            break;
        else if (strcmp(tok[0], ".help") == 0)
            print_help();
        else if (strcmp(tok[0], ".tables") == 0)
            gb_list_tables(db);
        else if (strcmp_nocase(tok[0], "CREATE") == 0 && strcmp_nocase(tok[1], "TABLE") == 0)
            cmd_create(db, tok, n);
        else if (strcmp_nocase(tok[0], "INSERT") == 0)
            cmd_insert(db, tok, n);
        else if (strcmp_nocase(tok[0], "SELECT") == 0)
            cmd_select(db, tok, n);
        else if (strcmp_nocase(tok[0], "DELETE") == 0)
            cmd_delete(db, tok, n);
        else if (strcmp_nocase(tok[0], "PUT") == 0)
            cmd_put(db, tok, n);
        else if (strcmp_nocase(tok[0], "GET") == 0)
            cmd_get(db, tok, n);
        else if (strcmp_nocase(tok[0], "DEL") == 0)
            cmd_del(db, tok, n);
        else if (strcmp_nocase(tok[0], "ENCRYPT") == 0)
            cmd_encrypt(db, tok, n);
        else if (strcmp(tok[0], ".schema") == 0 && n >= 2) {
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
        } else
            printf("Unknown command. Try .help\n");
    }

    gb_close(db);
    return 0;
}
