#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "storage.h"

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

int handle_binary_request(gb_storage *db, int fd, int *done)
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
