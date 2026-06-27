#include "_storage.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/file.h>
#include <unistd.h>
#include <fcntl.h>

static bool acquire_lock(gb_storage *db)
{
    size_t llen = strlen(db->path) + 6;
    char *lpath = malloc(llen);
    if (!lpath) return false;
    snprintf(lpath, llen, "%s.lock", db->path);

    db->lock_fd = open(lpath, O_WRONLY | O_CREAT, 0666);
    if (db->lock_fd < 0) { free(lpath); return false; }
    free(lpath);

    if (flock(db->lock_fd, LOCK_EX | LOCK_NB) < 0) {
        close(db->lock_fd);
        db->lock_fd = -1;
        return false;
    }
    return true;
}

static void release_lock(gb_storage *db)
{
    if (db->lock_fd >= 0) {
        flock(db->lock_fd, LOCK_UN);
        close(db->lock_fd);
        db->lock_fd = -1;
        size_t llen = strlen(db->path) + 6;
        char *lpath = malloc(llen);
        if (lpath) {
            snprintf(lpath, llen, "%s.lock", db->path);
            unlink(lpath);
            free(lpath);
        }
    }
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
    db->lock_fd = -1;

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

    if (!acquire_lock(db)) { gb_close(db); return NULL; }
    return db;
}

void gb_close(gb_storage *db)
{
    if (!db) return;
    release_lock(db);
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
