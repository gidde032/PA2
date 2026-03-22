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
    
    if (curr->is_directory) {
        if (strcmp(curr->path, ".") != 0) {
            mkdir(curr->path, 0755);
        }
    } else {
        int fd = open(curr->path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd == -1) {
            fprintf(stderr, "Error: Could not open file %s for writing\n",
                    curr->path);
            exit(1);
        }
        
        for (int i = 0; i < curr->num_blocks; i++) { // read block data from vault
            read_blob_from_vault(curr->chunks[i].physical_offset,
                                curr->chunks[i].size, fd);
        }
        
        close(fd);

        // Integrity check
        uint8_t computed_hash[32];
        compute_hash(curr->path, computed_hash);
        
        if (memcmp(computed_hash, curr->checksum, 32) != 0) {
            fprintf(stderr, "Error: Corruption detected in file %s.\n",
                    curr->path);
            unlink(curr->path);
            exit(1);
        }
    }
}

  // Cleanup
  free_file_list(target_snap->files);
  free(target_snap);
}
