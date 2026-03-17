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

  // TODO: 1. Initialize the Root directory "." and add it to your BFS
  // queue/list.

  // TODO: 2. Implement Level-Order Traversal (BFS)
  // - Open directories using opendir() and readdir().
  // - Ignore "." and ".." and the ".mgit" folder.
  // - Construct the full file path safely to avoid buffer overflows.
  // - Use stat() to gather size, mtime, inode, and directory status.

  // TODO: 3. Deduplication (Quick Check)
  // - First, check if the inode was already seen in the CURRENT snapshot (Hard
  // Link).
  // - Next, check if the file matches the PREVIOUS snapshot (mtime & size
  // match).
  // - If it matches, copy the checksum and block metadata. DO NOT re-hash.

  // TODO: 4. Deep Check
  // - If the file is modified or new, use compute_hash() to generate the
  // SHA-256.
  // - Allocate the BlockTable (chunks). Note: physical_offset is set later in
  // storage.c.

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
