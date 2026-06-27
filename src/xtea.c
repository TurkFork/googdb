#include "_storage.h"
#include <string.h>

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

void page_crypt(gb_page *page, uint32_t page_num, const uint32_t key[4])
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

/* ===== Public encryption API ===== */

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
