#include "_storage.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <signal.h>
#include <time.h>

#define LOCK_TIMEOUT 600
#define HEARTBEAT_INTERVAL 540

static gb_storage *signal_db;

static void hb_handler(int sig)
{
    (void)sig;
    if (signal_db) {
        time_t now = time(NULL);
        FILE *f = fopen(signal_db->lock_path, "we");
        if (f) {
            fprintf(f, "%lld\n", (long long)now);
            fclose(f);
        }
        signal_db->last_hb = now;
    }
    alarm(HEARTBEAT_INTERVAL);
}

static bool acquire_lock(gb_storage *db)
{
    struct stat st;

    if (stat(db->lock_path, &st) == 0) {
        FILE *f = fopen(db->lock_path, "re");
        if (f) {
            long long t = 0;
            if (fscanf(f, "%lld", &t) == 1) {
                if (time(NULL) - t < LOCK_TIMEOUT) {
                    fclose(f);
                    return false;
                }
            }
            fclose(f);
        }
    }

    FILE *f = fopen(db->lock_path, "we");
    if (!f) return false;
    time_t now = time(NULL);
    fprintf(f, "%lld\n", (long long)now);
    fclose(f);

    db->last_hb = now;
    signal_db = db;
    signal(SIGALRM, hb_handler);
    alarm(HEARTBEAT_INTERVAL);

    return true;
}

static void release_lock(gb_storage *db)
{
    signal_db = NULL;
    alarm(0);
    signal(SIGALRM, SIG_DFL);
    if (db->lock_path) {
        unlink(db->lock_path);
        free(db->lock_path);
        db->lock_path = NULL;
    }
}

void gb_lock_heartbeat(gb_storage *db)
{
    time_t now = time(NULL);
    if (now - db->last_hb < HEARTBEAT_INTERVAL) return;

    FILE *f = fopen(db->lock_path, "we");
    if (f) {
        fprintf(f, "%lld\n", (long long)now);
        fclose(f);
    }
    db->last_hb = now;
}

/* ===== Raw page I/O (no encryption) ===== */

bool read_page_data(gb_storage *db, uint32_t page_num, gb_page *page)
{
    if (page_num >= db->page_count) return false;
    long offset = GB_HEADER_SIZE + (long)page_num * GB_PAGE_SIZE;
    if (fseek(db->file, offset, SEEK_SET) != 0) return false;
    return fread(page->data, GB_PAGE_SIZE, 1, db->file) == 1;
}

bool write_page_data(gb_storage *db, uint32_t page_num, const gb_page *page)
{
    if (page_num >= db->page_count) return false;
    long offset = GB_HEADER_SIZE + (long)page_num * GB_PAGE_SIZE;
    if (fseek(db->file, offset, SEEK_SET) != 0) return false;
    if (fwrite(page->data, GB_PAGE_SIZE, 1, db->file) != 1) return false;
    return fflush(db->file) == 0;
}

/* ===== File header ===== */

bool read_header(gb_storage *db)
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

bool write_header(gb_storage *db)
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

    size_t llen = strlen(path) + 6;
    db->lock_path = malloc(llen);
    if (!db->lock_path) { free(db->path); free(db); return NULL; }
    snprintf(db->lock_path, llen, "%s.lock", path);

    FILE *check = fopen(path, "rb");
    bool exists = (check != NULL);
    if (check) fclose(check);

    db->file = fopen(path, "r+b");
    if (!db->file && exists) { gb_close(db); return NULL; }

    if (!db->file) {
        db->file = fopen(path, "w+b");
        if (!db->file) { gb_close(db); return NULL; }
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

    if (!acquire_lock(db)) { gb_close(db); return NULL; }
    return db;
}

void gb_close(gb_storage *db)
{
    if (!db) return;
    release_lock(db);
    if (db->file) fclose(db->file);
    free(db->path);
    free(db->lock_path);
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
    (void)db;
}

void gb_unlock(gb_storage *db)
{
    (void)db;
}
