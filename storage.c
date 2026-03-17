#include "mgit.h"
#include <errno.h>
#include <stdio.h>
#include <zstd.h>

#define BUF_SIZE 100

// --- Helper Functions ---
uint32_t get_current_head() {
  // opening HEAD to read integer
  int fd = open(".mgit/HEAD", O_RDONLY);
  if (fd == -1) {
    perror("open");
    return 0;
  }

  // reading integer from fd and converting to uint32_t
  char buf[BUF_SIZE];
  ssize_t bytesRead = read(fd, buf, BUF_SIZE);
  if (bytesRead == -1) {
    perror("read");
    close(fd);
    return 0;
  }
  buf[bytesRead] = '\0';

  uint32_t headInt = atoi(buf);

  close(fd);
  return headInt;
}

void update_head(uint32_t new_id) {
  // opening HEAD to write integer
  int fd = open(".mgit/HEAD", O_WRONLY | O_TRUNC);
  if (fd == -1) {
    perror("open");
    return;
  }

  // construct string from new_id to update HEAD with
  char buf[BUF_SIZE];
  snprintf(buf, BUF_SIZE, "%u", new_id);

  // write new_id to head
  if (write(fd, buf, strlen(buf)) == -1) {
    perror("write");
    close(fd);
    return;
  }

  close(fd);
  return;
}

// --- Blob Storage (Raw) ---
void write_blob_to_vault(const char *filepath, BlockTable *block) {
  // Open `filepath` for reading (rb).
  FILE *fp = fopen(filepath, "rb");
  if (!fp) {
    fprintf(stderr, "Error: Could not open file %s for reading.\n", filepath);
    return;
  }

  // Open `.mgit/data.bin` for APPENDING (ab).
  FILE *vault_fp = fopen(".mgit/data.bin", "ab");
  if (!vault_fp) {
    fprintf(stderr, "Error: Could not open vault for appending.\n");
    fclose(fp);
    return;
  }

  // Use ftell() to record the current end of the vault into
  // block->physical_offset.
  block->physical_offset = ftell(vault_fp);

  // Read the file bytes and write them into the vault.
  char buf[BUF_SIZE];
  size_t bytesRead;
  while ((bytesRead = fread(buf, 1, BUF_SIZE, fp)) > 0) {
    if (fwrite(buf, 1, bytesRead, vault_fp) != bytesRead) {
      fprintf(stderr, "Error: Failed to write to vault.\n");
      fclose(fp);
      fclose(vault_fp);
      return;
    }
    // Update block->size.
    block->size += bytesRead;
  }
}

void read_blob_from_vault(uint64_t offset, uint32_t size, int out_fd) {
  // Open the vault.
  FILE *vault_fp = fopen(".mgit/data.bin", "rb");
  if (!vault_fp) {
    fprintf(stderr, "Error: Could not open vault for reading.\n");
    return;
  }

  // fseek() to the physical_offset.
  if (fseek(vault_fp, offset, SEEK_SET) != 0) {
    fprintf(stderr, "Error: Failed to seek in vault.\n");
    fclose(vault_fp);
    return;
  }

  // Read `size` bytes and write them to `out_fd` using the write_all()
  // helper.
  char buf[BUF_SIZE];
  size_t bytesToRead = size;
  while (bytesToRead > 0) {
    size_t chunkSize = bytesToRead < BUF_SIZE ? bytesToRead : BUF_SIZE;
    size_t bytesRead = fread(buf, 1, chunkSize, vault_fp);
    if (bytesRead == 0) {
      if (feof(vault_fp)) {
        break; // End of file reached
      } else {
        fprintf(stderr, "Error: Failed to read from vault.\n");
        break;
      }
    }

    if (write_all(out_fd, buf, bytesRead) != bytesRead) {
      fprintf(stderr, "Error: Failed to write to output file.\n");
      break;
    }
  }
}

// --- Snapshot Management ---
void store_snapshot_to_disk(Snapshot *snap) {

  // Serialize the Snapshot struct and its linked list of
  // FileEntry/BlockTables into a binary file inside
  // `.mgit/snapshots/snap_XXX.bin`.

  // create file path string for snap_XXX.bin
  char filepath[BUF_SIZE];
  snprintf(filepath, BUF_SIZE, ".mgit/snapshots/snap_%03u.bin",
           snap->snapshot_id);

  // open snap_XXX.bin for writing
  FILE *fp = fopen(filepath, "wb");
  if (!fp) {
    fprintf(stderr, "Error: Could not open snapshot file for writing.\n");
    return;
  }

  // yes I copied this straight from `stream.c`

  // get total size needed for serialization
  size_t total_size = sizeof(uint32_t) * 2 + 256 * sizeof(char);
  FileEntry *curr = snap->files;
  while (curr) {
    total_size += (sizeof(FileEntry) - sizeof(void *) * 2);
    if (curr->num_blocks > 0)
      total_size += (sizeof(BlockTable) * curr->num_blocks);
    curr = curr->next;
  }

  void *buf = malloc(total_size);
  void *ptr = buf;

  memcpy(ptr, &snap->snapshot_id, sizeof(uint32_t));
  ptr += sizeof(uint32_t);
  memcpy(ptr, &snap->file_count, sizeof(uint32_t));
  ptr += sizeof(uint32_t);
  memcpy(ptr, snap->message, 256);
  ptr += 256;

  curr = snap->files;
  while (curr) {
    size_t fixed_size = sizeof(FileEntry) - sizeof(void *) * 2;
    memcpy(ptr, curr, fixed_size);
    ptr += fixed_size;
    if (curr->num_blocks > 0) {
      size_t blocks_size = sizeof(BlockTable) * curr->num_blocks;
      memcpy(ptr, curr->chunks, blocks_size);
      ptr += blocks_size;
    }
    curr = curr->next;
  }

  // write the buffer to snap_XXX.bin
  if (fwrite(buf, 1, total_size, fp) != total_size) {
    fprintf(stderr, "Error: Failed to write snapshot to disk.\n");
    fclose(fp);
    free(buf);
    return;
  }
  fclose(fp);
  free(buf);
}

Snapshot *load_snapshot_from_disk(uint32_t id) {

  // Read a `snap_XXX.bin` file and reconstruct the Snapshot struct
  // and its FileEntry linked list in heap memory.
  FILE *fp;
  char filepath[BUF_SIZE];
  snprintf(filepath, BUF_SIZE, ".mgit/snapshots/snap_%03u.bin", id);
  fp = fopen(filepath, "rb");
  if (!fp) {
    fprintf(stderr, "Error: Could not open snapshot file for reading.\n");
    return NULL;
  }

  FILE *vault_fp = fopen(".mgit/data.bin", "rb");
  if (!vault_fp) {
    fprintf(stderr, "Error: Could not open vault for reading.\n");
    fclose(fp);
    return NULL;
  }

  Snapshot *snap = malloc(sizeof(Snapshot));
  if (!snap) {
    fprintf(stderr, "Error: Could not allocate memory for snapshot.\n");
    fclose(fp);
    fclose(vault_fp);
    return NULL;
  }

  // read all of the basic data
  fread(&snap->snapshot_id, sizeof(uint32_t), 1, fp);
  fread(&snap->file_count, sizeof(uint32_t), 1, fp);
  fread(snap->message, sizeof(char), 256, fp);

  // read the file entries and block tables
  snap->files = NULL;
  for (uint32_t i = 0; i < snap->file_count; i++) {
    FileEntry *entry = malloc(sizeof(FileEntry));
    if (!entry) {
      fprintf(stderr, "Error: Could not allocate memory for file entry.\n");
      fclose(fp);
      fclose(vault_fp);
      return NULL;
    }
    fread(entry, sizeof(FileEntry) - sizeof(void *) * 2, 1, fp);
    if (entry->num_blocks > 0) {
      entry->chunks = malloc(sizeof(BlockTable) * entry->num_blocks);
      if (!entry->chunks) {
        fprintf(stderr, "Error: Could not allocate memory for block table.\n");
        free(entry);
        // free_snapshot(snap);
        fclose(fp);
        fclose(vault_fp);
        return NULL;
      }
      fread(entry->chunks, sizeof(BlockTable), entry->num_blocks, fp);
    } else {
      entry->chunks = NULL;
    }
    entry->next = snap->files;
    snap->files = entry;
  }

  return snap;
}

void chunks_recycle(uint32_t target_id) {
  // TODO: Garbage Collection (The Vacuum)
  // 1. Load the oldest snapshot (target_id) and the newest snapshot (HEAD).
  // 2. Iterate through the oldest snapshot's files.
  // 3. If a chunk's physical_offset is NOT being used by ANY file in the HEAD
  // snapshot,
  //    it is "stalled". Zero out those specific bytes in `data.bin`.
}

void mgit_snapshot(const char *msg) {
  // TODO: 1. Get current HEAD ID and calculate next_id. Load previous files
  // for crawling.
  // TODO: 2. Call build_file_list_bfs() to get the new directory state.

  // TODO: 3. Iterate through the new file list.
  // - If a file has data (chunks) but its size is 0, it needs to be written
  // to the vault.
  // - CRITICAL: Check for Hard Links! If another file in the *current* list
  // with the same
  //   inode was already written to the vault, copy its offset and size. DO
  //   NOT write twice!
  // - Call write_blob_to_vault() for new files.

  // TODO: 4. Call store_snapshot_to_disk() and update_head().
  // TODO: 5. Free memory.
  // TODO: 6. Enforce MAX_SNAPSHOT_HISTORY (5). If exceeded, call
  // chunks_recycle()
  //          and delete the oldest manifest file using remove().
}
