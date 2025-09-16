#include "kernel/types.h"
#include "user/user.h"

int main() {
  int c2f[2], f2c[2];
  pipe(c2f);
  pipe(f2c);

  int pid = fork();
  if (pid == 0) {
    close(f2c[1]);
    close(c2f[0]);

    int parent_pid;
    read(f2c[0], &parent_pid, sizeof(int));
    printf("%d: received ping from pid %d\n", getpid(), parent_pid);

    int child_pid = getpid();
    write(c2f[1], &child_pid, sizeof(int));

    close(f2c[0]);
    close(c2f[1]);
    exit(0);
  } else {
    close(f2c[0]);
    close(c2f[1]);

    int parent_pid = getpid();
    write(f2c[1], &parent_pid, sizeof(int));

    int child_pid;
    read(c2f[0], &child_pid, sizeof(int));
    printf("%d: received pong from pid %d\n", getpid(), child_pid);

    close(f2c[1]);
    close(c2f[0]);
    wait(0);
    exit(0);
  }
}