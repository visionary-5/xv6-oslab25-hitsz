// user/pingpong.c
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  int f2c[2]; // father -> child
  int c2f[2]; // child  -> father

  if (pipe(f2c) < 0 || pipe(c2f) < 0) {
    fprintf(2, "pingpong: pipe failed\n");
    exit(1);
  }

  int father_pid = getpid();
  int pid = fork();
  if (pid < 0) {
    fprintf(2, "pingpong: fork failed\n");
    exit(1);
  }

  if (pid == 0) {
    // child
    close(f2c[1]); // child doesn't write to f2c
    close(c2f[0]); // child doesn't read  from c2f

    int recv_father_pid = -1;
    if (read(f2c[0], &recv_father_pid, sizeof(recv_father_pid)) != sizeof(recv_father_pid)) {
      fprintf(2, "child: read failed\n");
      exit(1);
    }
    // 子进程打印
    printf("%d: received ping from pid %d\n", getpid(), recv_father_pid);

    int child_pid = getpid();
    if (write(c2f[1], &child_pid, sizeof(child_pid)) != sizeof(child_pid)) {
      fprintf(2, "child: write failed\n");
      exit(1);
    }

    close(f2c[0]);
    close(c2f[1]);
    exit(0);
  } else {
    // parent
    close(f2c[0]); // parent doesn't read  from f2c
    close(c2f[1]); // parent doesn't write to c2f

    // 把父 pid 发给子进程
    if (write(f2c[1], &father_pid, sizeof(father_pid)) != sizeof(father_pid)) {
      fprintf(2, "parent: write failed\n");
      exit(1);
    }
    close(f2c[1]); // 重要：写完就关，避免子读阻塞在 EOF 之后

    int recv_child_pid = -1;
    if (read(c2f[0], &recv_child_pid, sizeof(recv_child_pid)) != sizeof(recv_child_pid)) {
      fprintf(2, "parent: read failed\n");
      exit(1);
    }
    // 父进程打印
    printf("%d: received pong from pid %d\n", getpid(), recv_child_pid);

    close(c2f[0]);
    wait(0);
    exit(0);
  }
}
