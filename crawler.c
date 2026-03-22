#include "mgit.h"
#include <dirent.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

// Helper to calculate SHA256 using system utility
void compute_hash(const char *path, uint8_t *output) {
  // Set up a pipe to capture the output of the sha256sum command.
  int pipefd[2];
  if (pipe(pipefd) == -1) {
    perror("pipe");
    exit(1);
  }

  // Fork a child process.
  int pid = fork();

  if (pid == -1) {
    perror("fork");
    exit(1);
  } else if (pid == 0) {
    // Child process
    close(pipefd[0]);               // Close read end
    dup2(pipefd[1], STDOUT_FILENO); // Redirect STDOUT to pipe
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull == -1) {
      perror("open");
      exit(1);
    }
    dup2(devnull, STDERR_FILENO); // Redirect STDERR to /dev/null
    close(devnull);               // Close the /dev/null file descriptor

    execlp("sha256sum", "sha256sum", path, NULL);
    perror("execlp"); // If execlp returns, it's an error
    exit(1);
  } else {
    // Parent process
    close(pipefd[1]); // Close write end

    // Read exactly 64 characters (the hex string) from the read end.
    char hash_hex[65]; // 64 chars + null terminator
    ssize_t bytes_read = read(pipefd[0], hash_hex, 64);
    if (bytes_read != 64) {
      fprintf(stderr,
              "Error: Expected to read 64 characters for hash, but got %zd\n",
              bytes_read);
      exit(1);
    }
    hash_hex[64] = '\0'; // Null-terminate the string

    // Convert the hex string into 32 bytes and store it in 'output'.
    for (int i = 0; i < 32; i++) {
      sscanf(&hash_hex[i * 2], "%2hhx", &output[i]);
    }

    close(pipefd[0]); // Close read end
    wait(NULL);       // Wait for child to finish
  }
}

// Check if file matches previous snapshot (Quick Check)
FileEntry *find_in_prev(FileEntry *prev, const char *path) {
  // Iterate through the 'prev' linked list.
  while (prev) {
    if (strcmp(prev->path, path) == 0) {
      return prev; // Found a match in the previous snapshot
    }
    prev = prev->next;
  }
  return NULL;
}

// HELPER: Check if an inode already exists in the current snapshot's list
FileEntry *find_in_current_by_inode(FileEntry *head, ino_t inode) {
  while (head) {
    if (!head->is_directory && head->inode == inode)
      return head;
    head = head->next;
  }
  return NULL;
}

FileEntry *build_file_list_bfs(const char *root, FileEntry *prev_snap_files) {
  FileEntry *head = NULL, *tail = NULL;

  // Initialize the Root directory "." and add it to your BFS
  // queue/list.

  // construct the root FileEntry
  FileEntry *root_entry = malloc(sizeof(FileEntry));
  if (!root_entry) {
    fprintf(stderr, "Error: Could not allocate memory for root entry.\n");
    return NULL;
  }

  struct stat root_stat;
  if (stat(root, &root_stat) == -1) {
    fprintf(stderr, "Error: Could not stat root directory %s.\n", root);
    free(root_entry);
    return NULL;
  }

  strncpy(root_entry->path, ".", 256);
  root_entry->is_directory = 1;
  root_entry->size = root_stat.st_size;
  root_entry->mtime = root_stat.st_mtime;
  root_entry->inode = root_stat.st_ino;
  root_entry->num_blocks = 0;
  root_entry->chunks = NULL;
  root_entry->next = NULL;
  head = tail = root_entry;

  // implement Level Order Traversal (BFS) using a queue
  FileEntry *current = head;

  while (current) {

    // Process the current directory entry
    if (current->is_directory) {
      // Construct the full file path safely to avoid buffer overflows.
      char dir_path[4097];
      memset(dir_path, 0, sizeof(dir_path));
      snprintf(dir_path, sizeof(dir_path), "%s/%s", root, current->path);

      DIR *dir = opendir(dir_path);
      if (!dir) {
        fprintf(stderr, "Error: Could not open directory %s.\n", dir_path);
        continue;
      }

      struct dirent *entry;
      while ((entry = readdir(dir)) != NULL) {
        // Ignore "." and ".." and the ".mgit" folder.
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0 ||
            strcmp(entry->d_name, ".mgit") == 0) {
          continue;
        }

        // Construct the full file path safely to avoid buffer overflows.
        char file_path[4356];
        memset(file_path, 0, sizeof(file_path));
        if (strcmp(current->path, ".") == 0) {
          snprintf(file_path, sizeof(file_path), "%s/%s", root, entry->d_name);
        } else {
          snprintf(file_path, sizeof(file_path), "%s/%s/%s", root,
                   current->path, entry->d_name);
        }
        // snprintf(file_path, sizeof(file_path), "%s/%s/%s", root,
        // current->path, entry->d_name);

        struct stat file_stat;
        if (stat(file_path, &file_stat) == -1) {
          fprintf(stderr, "Error: Could not stat file %s.\n", file_path);
          continue;
        }
        // Construct a new FileEntry for this file/directory
        FileEntry *new_entry = malloc(sizeof(FileEntry));
        if (!new_entry) {
          fprintf(stderr, "Error: Could not allocate memory for file entry.\n");
          continue;
        }

        // Store relative path
        strncpy(new_entry->path, file_path + strlen(root) + 1, 256);
        new_entry->is_directory = S_ISDIR(file_stat.st_mode);
        new_entry->size = file_stat.st_size;
        new_entry->mtime = file_stat.st_mtime;
        new_entry->inode = file_stat.st_ino;
        new_entry->num_blocks = 0;
        new_entry->chunks = NULL;
        new_entry->next = NULL;

        // Deduplication (Quick Check)
        FileEntry *match;
        // First, check if the inode was already seen in the CURRENT snapshot
        match = find_in_current_by_inode(head, new_entry->inode);
        if (!match) {
          // Next, check if the file matches the PREVIOUS snapshot (mtime & size
          // match).
          match = find_in_prev(prev_snap_files, new_entry->path);
        }

        if (match) {
          // if either match is found, copy its offset and size. DO NOT write
          // twice!
          new_entry->num_blocks = match->num_blocks;
          new_entry->chunks =
              malloc(sizeof(BlockTable) * new_entry->num_blocks);
          memcpy(new_entry->chunks, match->chunks,
                 sizeof(BlockTable) * new_entry->num_blocks);
          memcpy(new_entry->checksum, match->checksum, 32);

        } else if (!new_entry->is_directory && new_entry->size > 0) {

          // If the file is modified or new, use compute_hash() to
          // generate the SHA- 256.
          compute_hash(file_path, new_entry->checksum);

          // Allocate the BlockTable (chunks). Note: physical_offset is set
          // later in storage.c.
          new_entry->chunks = malloc(sizeof(BlockTable));

          if (!new_entry->chunks) {
            fprintf(stderr,
                    "Error: Could not allocate memory for block table.\n");
            free(new_entry);
            continue;
          }

          new_entry->num_blocks = 1;
          new_entry->chunks[0].size = 0;
          new_entry->chunks[0].physical_offset = 0;
        }

        // Append new_entry to the linked list
        if (tail) {
          tail->next = new_entry;
          tail = new_entry;
        } else {
          head = tail = new_entry;
        }
      }
      closedir(dir);
    }
    // Move to the next entry in the queue
    current = current->next;
    if (!current)
      break;
  }

  // TODO: 2. Implement Level-Order Traversal (BFS)
  // - Open directories using opendir() and readdir().
  // - Ignore "." and ".." and the ".mgit" folder.
  // - Construct the full file path safely to avoid buffer overflows.
  // - Use stat() to gather size, mtime, inode, and directory status.

  // TODO: 3. Deduplication (Quick Check)
  // - First, check if the inode was already seen in the CURRENT snapshot
  // (Hard Link).
  // - Next, check if the file matches the PREVIOUS snapshot (mtime & size
  // match).
  // - If it matches, copy the checksum and block metadata. DO NOT
  // re-hash.

  // TODO: 4. Deep Check
  // - If the file is modified or new, use compute_hash() to generate the
  // SHA-256.
  // - Allocate the BlockTable (chunks). Note: physical_offset is set
  // later in storage.c.

  // TODO: 5. Append new FileEntry to your linked list.

  return head;
}

void free_file_list(FileEntry *head) {
  // Iterate through the linked list and free() each node,
  // including the dynamically allocated 'chunks' array within each node.
  while (head) {
    FileEntry *next = head->next;
    if (head->chunks) {
      free(head->chunks); // Free the chunks array if it exists
    }
    free(head);  // Free the FileEntry node
    head = next; // Move to the next node
  }
}

void mgit_show(const char *id_str) {
  // "It should printout current crawled in-memory metadata if not id is
  // specified. Otherwise print the snapshot’s metadata if existed"
  if (!id_str) { // print live directory view if no snapshot id specified
    printf("=== LIVE VIEW ===\n");
    FileEntry *liveFiles = build_file_list_bfs(".", NULL);
    if (!liveFiles) {
      fprintf(stderr, "Error crawling in-memory metadata\n");
      exit(1);
    }

    int count = 0;
    FileEntry *currentFile = liveFiles;
    while (currentFile) { // print each file/dir and specify type
      if (currentFile->is_directory) {
        printf("Directory:  %s\n", currentFile->path);
      } else {
        printf("File: %s (%ld bytes)\n", currentFile->path, currentFile->size);
      }
      count++;
      currentFile = currentFile->next;
    }

    printf("\nCurrent directory total: %d files\n",
           count); // print total file/dir count
    free_file_list(liveFiles);
  } else { // print snapshot view if id specified
    uint32_t id = atoi(id_str);
    Snapshot *snap = load_snapshot_from_disk(id);

    if (!snap) {
      fprintf(stderr, "Error: Snapshot %d not found.\n", id);
      return;
    }

    printf("=== SNAPSHOT %u ===\n", snap->snapshot_id);
    printf("Message: %s\n", snap->message);
    printf("File Count: %u\n", snap->file_count);

    FileEntry *currentFile = snap->files;
    while (currentFile) { // print each file/dir and specify type
      if (currentFile->is_directory) {
        printf("Directory:  %s\n", currentFile->path);
      } else {
        printf("File: %s (%ld bytes)\n", currentFile->path, currentFile->size);
      }
      currentFile = currentFile->next;
    }

    free_file_list(snap->files);
    free(snap);
  }
}