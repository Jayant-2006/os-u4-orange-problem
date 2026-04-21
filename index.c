// index.c — Staging area implementation

#include "index.h"
#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec ||
                st.st_size != (off_t)index->entries[i].size) {
                printf("  modified:   %s\n", index->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes") == 0) continue;
            if (strstr(ent->d_name, ".o") != NULL) continue;

            int is_tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1;
                    break;
                }
            }

            if (!is_tracked) {
                struct stat st;
                stat(ent->d_name, &st);
                if (S_ISREG(st.st_mode)) {
                    printf("  untracked:  %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }
    if (untracked_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    return 0;
}

// ─── IMPLEMENTED ─────────────────────────────────────────────────────────────

static int compare_index_entries(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path, ((const IndexEntry *)b)->path);
}

int index_load(Index *index) {
    index->count = 0;

    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) return 0;  // No index file yet — empty index, not an error

    char hex[HASH_HEX_SIZE + 1];
    unsigned int mode;
    unsigned long long mtime;
    unsigned int size;
    char path[512];

    while (fscanf(f, "%o %64s %llu %u %511s\n",
                  &mode, hex, &mtime, &size, path) == 5) {
        if (index->count >= MAX_INDEX_ENTRIES) break;

        IndexEntry *e = &index->entries[index->count];
        e->mode = (uint32_t)mode;
        e->mtime_sec = (uint64_t)mtime;
        e->size = (uint32_t)size;
        strncpy(e->path, path, sizeof(e->path) - 1);
        e->path[sizeof(e->path) - 1] = '\0';

        if (hex_to_hash(hex, &e->hash) != 0) {
            fclose(f);
            return -1;
        }
        index->count++;
    }

    fclose(f);
    return 0;
}

int index_save(const Index *index) {
    Index sorted = *index;
    qsort(sorted.entries, sorted.count, sizeof(IndexEntry), compare_index_entries);

    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp_XXXXXX", INDEX_FILE);
    int fd = mkstemp(tmp_path);
    if (fd < 0) return -1;

    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        unlink(tmp_path);
        return -1;
    }

    char hex[HASH_HEX_SIZE + 1];
    for (int i = 0; i < sorted.count; i++) {
        const IndexEntry *e = &sorted.entries[i];
        hash_to_hex(&e->hash, hex);
        fprintf(f, "%o %s %llu %u %s\n",
                e->mode, hex,
                (unsigned long long)e->mtime_sec,
                e->size, e->path);
    }

    fflush(f);
    fsync(fileno(f));
    fclose(f);

    if (rename(tmp_path, INDEX_FILE) != 0) {
        unlink(tmp_path);
        return -1;
    }

    return 0;
}

int index_add(Index *index, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s'\n", path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *data = malloc(file_size > 0 ? file_size : 1);
    if (!data) {
        fclose(f);
        return -1;
    }

    if (file_size > 0 && fread(data, 1, file_size, f) != (size_t)file_size) {
        fclose(f);
        free(data);
        return -1;
    }
    fclose(f);

    ObjectID hash;
    if (object_write(OBJ_BLOB, data, file_size, &hash) != 0) {
        free(data);
        return -1;
    }
    free(data);

    struct stat st;
    if (lstat(path, &st) != 0) return -1;

    IndexEntry *existing = index_find(index, path);
    if (existing) {
        existing->hash = hash;
        existing->mtime_sec = st.st_mtime;
        existing->size = (uint32_t)st.st_size;
        existing->mode = (st.st_mode & S_IXUSR) ? 0100755 : 0100644;
    } else {
        if (index->count >= MAX_INDEX_ENTRIES) return -1;
        IndexEntry *e = &index->entries[index->count++];
        e->hash = hash;
        e->mtime_sec = (uint64_t)st.st_mtime;
        e->size = (uint32_t)st.st_size;
        e->mode = (st.st_mode & S_IXUSR) ? 0100755 : 0100644;
        strncpy(e->path, path, sizeof(e->path) - 1);
        e->path[sizeof(e->path) - 1] = '\0';
    }

    return index_save(index);
}
