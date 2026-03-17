#include "mgit.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>

int main(int argc, char *argv[]) {
  // Basic routing logic is provided.
  if (argc < 2)
    return 1;

  if (strcmp(argv[1], "init") == 0) {
    mgit_init();
  } else if (strcmp(argv[1], "snapshot") == 0) {
    if (argc < 3)
      return 1;
    mgit_snapshot(argv[2]);
  } else if (strcmp(argv[1], "send") == 0) {
    mgit_send(argc > 2 ? argv[2] : NULL);
  } else if (strcmp(argv[1], "receive") == 0) {
    if (argc < 3)
      return 1;
    mgit_receive(argv[2]);
  } else if (strcmp(argv[1], "show") == 0) {
    mgit_show(argc > 2 ? argv[2] : NULL);
  } else if (strcmp(argv[1], "restore") == 0) {
    if (argc < 3)
      return 1;
    mgit_restore(argv[2]);
  }
  return 0;
}

void mgit_init() {
  // initializing repo structure
  struct stat buf;

  if (stat(".mgit", &buf) == 0) { // checks if mgit exists
    return;
  } else {
    if (errno != ENOENT) { // if error is not due to mgit not existing, print
                           // error and return
      fprintf(stderr, "Error initializing mgit\n");
      return;
    }
  }

  // create .mgit directory
  if (mkdir(".mgit", 0755) == -1) { // main mgit directory
    perror("mkdir");
    return;
  }

  // create .mgit/snapshots directory
  if (mkdir(".mgit/snapshots", 0755) == -1) {
    perror("mkdir");
    return;
  }

  // create vault file .mgit/data.bin
  int fd = open(".mgit/data.bin", O_WRONLY | O_CREAT, 0644);
  if (fd == -1) {
    perror("open");
    return;
  }
  close(fd);

  // create .mgit/HEAD file and writing "0" into it to initialize the snapshot
  // counter
  fd = open(".mgit/HEAD", O_WRONLY | O_CREAT, 0644);
  if (fd == -1) {
    perror("open");
    return;
  }

  if (write(fd, "0", 1) == -1) {
    perror("write");
    close(fd);
    return;
  }
  close(fd);
}
