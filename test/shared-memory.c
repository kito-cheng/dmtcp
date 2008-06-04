/* Compile:
 * gcc -o shared-memory -Wl,--export-dynamic THIS_FILE
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/mman.h>

// For open()
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>


void reader(int fd);
void writer(int fd);

/* Compile with  -Wl,--export-dynamic to make these functions visible. */
void dmtcpHookPreCheckpoint() {
  printf("\n%s: %s: about to checkpoint\n", __FILE__, __func__);
}
void dmtcpHookPostCheckpoint() {
  printf("\n%s: %s: done checkpointing\n", __FILE__, __func__);
}
void dmtcpHookRestart() {
  printf("\n%s: %s: restarting\n", __FILE__, __func__);
}

int main() {
  char filename[] = "dmtcp-shared-memory.XXXXXX";
  int initValue = -1;
  int fd;

  fd = mkstemp(filename);
  //if (-1 == unlink(filename)) {
  //  perror("unlink");
  //  exit(1);
  //}
  //fd = open("/tmp/dmtcp-shared-memory.dat", O_RDWR | O_CREAT, S_IREAD|S_IWRITE);
  // if (fd == -1) perror("open");
  write(fd, &initValue, sizeof(int)); /* Extend file to needed size */
  printf("creating temporary file in local directory: %s\n", filename);
  //printf("creating file: %s\n", "/tmp/dmtcp-shared-memory.dat");

  if (fork())
    writer(fd);
  else
    reader(fd);
  return 0;
}

void reader(int fd) {
  int *sharedMemory;
  int i;
  sharedMemory = mmap(0, sizeof(int), PROT_READ, MAP_SHARED, fd, 0);
  if (sharedMemory == MAP_FAILED) {
    perror("mmap");
    exit(1);
  }
  for (i = -1; ; ) {
    if (*sharedMemory > i) {
      i = *sharedMemory;
      printf("%d ", i);
      fflush(stdout);
    }
    sleep(1);
  }
}

void writer(int fd) {
  int *sharedMemory;
  int i;
  sharedMemory = mmap(0, sizeof(int), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
  if (sharedMemory == MAP_FAILED) {
    perror("mmap");
    exit(1);
  }
  for (i = 0; ; i++) {
    *sharedMemory = i;
    sleep(2);
  }
}