#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char** argv) {
  if (argc < 2) return 2;
  int fd = open(argv[1], O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd < 0) {
    perror("open");
    return 1;
  }

  struct termios tio;
  if (tcgetattr(fd, &tio) == 0) {
    cfsetspeed(&tio, B115200);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    tio.c_iflag &= ~(IXON | IXOFF | IXANY);
    tio.c_oflag &= ~OPOST;
    tcsetattr(fd, TCSANOW, &tio);
  }

  const char* request = "{\"qt\":1,\"id\":1,\"command\":\"get-telemetry\"}\n";
  write(fd, request, strlen(request));

  char buffer[4096];
  time_t start = time(NULL);
  while (time(NULL) - start < 10) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    struct timeval tv = {0, 250000};
    int ready = select(fd + 1, &set, NULL, NULL, &tv);
    if (ready > 0) {
      ssize_t n = read(fd, buffer, sizeof(buffer) - 1);
      if (n > 0) {
        buffer[n] = 0;
        fputs(buffer, stdout);
        fflush(stdout);
      }
    }
  }
  close(fd);
  return 0;
}
