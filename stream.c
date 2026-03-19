#include "mgit.h"
#include <arpa/inet.h> // For htonl/ntohl
#include <errno.h>
#include <zstd.h>

// --- Safe I/O Helpers ---
// These are essential for handling partial reads/writes in Pipes/Sockets.
ssize_t read_all(int fd, void *buf, size_t count) {
  size_t total = 0;
  while (total < count) {
    ssize_t ret = read(fd, (char *)buf + total, count - total);
    if (ret == 0)
      break; // EOF
    if (ret < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    total += ret;
  }
  return total;
}

ssize_t write_all(int fd, const void *buf, size_t count) {
  size_t total = 0;
  while (total < count) {
    ssize_t ret = write(fd, (const char *)buf + total, count - total);
    if (ret < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    total += ret;
  }
  return total;
}

// --- Serialization Helper ---
void *serialize_snapshot(Snapshot *snap, size_t *out_len) {
  size_t total_size = sizeof(uint32_t) * 2 + 256;
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

  *out_len = total_size;
  return buf;
}

void mgit_send(const char *id_str) {
  // 1. Handshake Phase
  // Send the MAGIC_NUMBER (0x4D474954) to STDOUT.
  uint32_t magic = htonl(MAGIC_NUMBER);
  // Pass the address of the magic variable to write_all
  if (write_all(STDOUT_FILENO, &magic, 4) != 4) {
    fprintf(stderr, "Error: Failed to send handshake\n");
    exit(1);
  }

  // 2. Manifest Phase

  // if id_str is NULL, we send the latest snapshot. Otherwise, we send the
  // snapshot with the given id.
  uint32_t id = id_str ? atoi(id_str) : get_current_head();

  // Serialize the snapshot metadata
  Snapshot *snap = load_snapshot_from_disk(id);
  if (!snap) {
    fprintf(stderr, "Error: Snapshot %d not found.\n", id);
    exit(1);
  }
  size_t manifest_len;
  void *manifest_buf = serialize_snapshot(snap, &manifest_len);

  // Send its size followed by the buffer.
  if (write_all(STDOUT_FILENO, &manifest_len, 4) != 4) {
    fprintf(stderr, "Error: Failed to send manifest length\n");
    exit(1);
  }

  if (write_all(STDOUT_FILENO, manifest_buf, manifest_len) != manifest_len) {
    fprintf(stderr, "Error: Failed to send manifest buffer\n");
    exit(1);
  }

  // 3. Payload Phase
  FileEntry *currentFile = snap->files;
  while (currentFile) { // iterate through all files in the current snapshot
    if (!currentFile->is_directory && currentFile->num_blocks > 0) { // check if files contain blocks of data
      for (int i = 0; i<currentFile->num_blocks; i++) {
        FILE *storedData = fopen(".mgit/data.bin", "rb");
        fseek(storedData, currentFile->chunks[i].physical_offset, SEEK_SET); // read from stored offset

        void *chunkBuffer = malloc(currentFile->chunks[i].size);
        fread(chunkBuffer, 1, currentFile->chunks[i].size, storedData); // read into temporary chunk buffer before storing
        fclose(storedData);

        write_all(STDOUT_FILENO, chunkBuffer, currentFile->chunks[i].size); // write compressed data
        free(chunkBuffer);
      }
    }
    currentFile = currentFile->next;
  }
  free(manifest_buf);
}

void mgit_receive(const char *dest_path) {
  // Setup
  // mkdir(dest_path) and mgit_init() inside it.
  mkdir(dest_path, 0755);
  chdir(dest_path);
  mgit_init();

  // Handshake Phase
  uint32_t magic;
  if (read_all(STDIN_FILENO, &magic, 4) != 4) {
    exit(1);
  }
  if (ntohl(magic) != MAGIC_NUMBER) {
    fprintf(stderr, "Error: Invalid protocol\n");
    exit(1);
  }

  uint32_t net_len;
  if (read_all(STDIN_FILENO, &net_len, 4) != 4) {
    exit(1);
  }
  size_t manifest_len = ntohl(net_len);

  void *manifestBuffer = malloc(manifest_len); // create the manifest buffer and read in all data
  read_all(STDIN_FILENO, manifestBuffer, manifest_len);

  Snapshot *snap = malloc(sizeof(Snapshot));
  void *ptr = manifestBuffer;

  memcpy(&snap->snapshot_id, ptr, 4); // rebuild snapshot using data stored in buffer
  ptr += 4;
  memcpy(&snap->file_count, ptr, 4);
  ptr += 4;
  memcpy(&snap->message, ptr, 256);
  ptr += 256;

  FileEntry *head = NULL; 
  FileEntry *tail = NULL;
  for (int i = 0; i < snap->file_count; i++) {
    FileEntry *currentFile = malloc(sizeof(FileEntry));
    
    size_t fileBaseSize = sizeof(FileEntry) - sizeof(void*) * 2; // copying basic file data of fixed size
    memcpy(currentFile, ptr, fileBaseSize);
    ptr += fileBaseSize;
    
    // Allocate and copy blocks
    if (currentFile->num_blocks > 0) {
      currentFile->chunks = malloc(sizeof(BlockTable) * currentFile->num_blocks);
        size_t blockSize = sizeof(BlockTable) * currentFile->num_blocks;
        memcpy(currentFile->chunks, ptr, blockSize);
        ptr += blockSize;
    }
    
    currentFile->next = NULL;
    if (!head) head = currentFile;
    if (tail) tail->next = currentFile;
    tail = currentFile;
  }

  snap->files = head; // build new snapshot file list
  free(manifestBuffer);

  FILE *storedData = fopen(".mgit/data.bin", "ab");
  FileEntry *currentFile = snap->files;

  while (currentFile) {
    if (!currentFile->is_directory && currentFile->num_blocks > 0) { // iterate through all files with data blocks
        for (int i = 0; i < currentFile->num_blocks; i++) {
            uint64_t new_offset = ftell(storedData);
            
            void *chunk = malloc(currentFile->chunks[i].size);
            read_all(STDIN_FILENO, chunk, currentFile->chunks[i].size);
            
            fwrite(chunk, 1, currentFile->chunks[i].size, storedData); // write to stored data
            
            currentFile->chunks[i].physical_offset = new_offset; // update offset of altered chunk
            
            free(chunk);
        }
        
        FILE *output = fopen(currentFile->path, "wb");
        for (int i = 0; i < currentFile->num_blocks; i++) {
            fseek(storedData, currentFile->chunks[i].physical_offset, SEEK_SET);
            void *data = malloc(currentFile->chunks[i].size);
            fread(data, 1, currentFile->chunks[i].size, storedData);
            fwrite(data, 1, currentFile->chunks[i].size, output);
            free(data);
        }
        fclose(output);
    } else if (currentFile->is_directory && strcmp(currentFile->path, ".") != 0) {
        mkdir(currentFile->path, 0755);
    }
    
    currentFile = currentFile->next;
  }
  fclose(storedData);

  store_snapshot_to_disk(snap); // save new snapshot and update head with its id
  update_head(snap->snapshot_id);

  free_file_list(snap->files); // cleanup
  free(snap);
}
