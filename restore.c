#include "mgit.h"

// Helper: Check if a path exists in the target snapshot
int path_in_snapshot(Snapshot *snap, const char *path) {
  // Iterate over snap->files and return 1 if the path matches, 0 otherwise.
  FileEntry *currentFile = snap->files;
    while (currentFile) {
        if (strcmp(currentFile->path, path) == 0) {
            return 1;
        }
        currentFile = currentFile->next;
    }
    return 0;
}

// Helper: Reverse the linked list
FileEntry *reverse_list(FileEntry *head) {
  // Standard linked list reversal.
  FileEntry *previous = NULL;
  FileEntry *ptr = head;
  FileEntry *next;

  while (ptr) {
    next = ptr->next; // save next node
    ptr->next = previous; // reverse link
    previous = ptr; // traverse list
    ptr = next; // move to next node
  }
  return previous;
}

void mgit_restore(const char *id_str) {
  if (!id_str)
    return;
  uint32_t id = atoi(id_str);

  // 1. Load Target Snapshot
  Snapshot *target_snap = load_snapshot_from_disk(id);
  if (!target_snap) {
    fprintf(stderr, "Error: Snapshot %d not found.\n", id);
    exit(1);
  }

  // --- PHASE 1: SANITIZATION (The Purge) ---
  // Remove files that exist currently but NOT in the target snapshot.
  FileEntry *current_files = build_file_list_bfs(".", NULL);
  FileEntry *reversed = reverse_list(current_files);

  // Iterate through 'reversed'.
  for (FileEntry *curr = reversed; curr != NULL; curr = curr->next) {
    // If a file/dir exists on disk (but is not ".") AND is not in target_snap:
    if (strcmp(curr->path, ".") != 0 &&
        !path_in_snapshot(target_snap, curr->path)) {
      if (curr->is_directory) {
        // Use rmdir() if it's a directory.
        rmdir(curr->path); // Remove directory
      } else {
        // Use unlink() if it's a file.
        unlink(curr->path); // Remove file
      }
    }
  }

  free_file_list(reversed);

  // --- PHASE 2: RECONSTRUCTION & INTEGRITY ---
  // Iterate through target_snap->files.
  for (FileEntry *curr = target_snap->files; curr != NULL; curr = curr->next) {
    // optimization: skip if a file/dir already exists on disk with the same
    // size and mtime
    struct stat buf;
    if (stat(curr->path, &buf) == 0) {
      if (buf.st_size == curr->size && buf.st_mtime == curr->mtime) {
        uint8_t hash[32];
        compute_hash(curr->path, hash);
        if (memcmp(hash, curr->checksum, 32) == 0) {
            continue;  // Skip this file/dir as it seems unchanged
        }
      }
    } 
    
    // If it's a directory (and not "."), recreate it using mkdir() with 0755.
    if (curr->is_directory) {
      if (strcmp(curr->path, ".") != 0) {
        mkdir(curr->path, 0755); // Create directory
      }
    } else {
      // If it's a file, open it for writing ("wb").
      FILE *fp = fopen(curr->path, "wb"); // Open file for writing
      if (!fp) {
        fprintf(stderr, "Error: Could not open file %s for writing.\n",
                curr->path);
        continue;
      }
      // For each block in curr->chunks, call read_blob_from_vault() to write
      // the data back to disk.
      for (int i = 0; i < curr->num_blocks; i++) {
        read_blob_from_vault(curr->chunks[i].physical_offset,
                             curr->chunks[i].size, fileno(fp));
      }

      // --- INTEGRITY CHECK (Corruption Detection) ---
      uint8_t computed_hash[32];
      compute_hash(curr->path, computed_hash);

      if (memcmp(computed_hash, curr->checksum, 32) != 0) {
        fprintf(stderr,
                "Error: Corruption detected in file %s. Hash mismatch.\n",
                curr->path);
        unlink(curr->path); // Remove the corrupted file
        fclose(fp);
        exit(1); // Abort the restore process
      }

      fclose(fp);
    }
  }

  // Cleanup
  free_file_list(target_snap->files);
  free(target_snap);
}
