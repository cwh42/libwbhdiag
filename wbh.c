#include <stdlib.h>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include "wbh.h"

#define DEBUG

static void crtolf(char *buf, size_t size)
{
  int i;
  for (i = 0; i < size; i++) {
    if (buf[i] == '\r')
      buf[i] = '\n';
  }
}

static int read_nonblock(int fd, char *buf, size_t size, int timeout, int expect)
{
  int rc;
  fd_set fds;
  struct timeval timev;
  timev.tv_usec = 0;
  timev.tv_sec = timeout;
  size_t osize = size;
  memset(buf, 0, size);
  while (size > 0) {
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    rc = select(fd + 1, &fds, NULL, NULL, &timev);
    if (rc < 0) {
      /* error */
      perror("select");
      return -1;
    }
    if (rc == 0) {
      /* timeout */
      return -1;
    }
    rc = read(fd, buf, size);
    if (rc < 0) {
      perror("read");
      return -1;
    }
    crtolf(buf, rc);
#ifdef DEBUG
    fprintf(stderr, "READ: %s", buf);
#endif
    size -= rc;
    buf += rc;
    if (rc >= 1)
      fprintf(stderr, "last char 0x%x\n", buf[-1]);
    if (expect && rc >= 1 && buf[-1] == expect) {
      return osize - size;
    }
  }
  return osize;
}

static int write_nonblock(int fd, char *buf, size_t size)
{
  int rc;
  rc = write(fd, buf, size);
#ifdef DEBUG
  char *buf2 = malloc(size);
  memcpy(buf2, buf, size);
  crtolf(buf2, size);
  fprintf(stderr, "WRITE: -%s- (%zd/%d)\n", buf2, size, rc);
  free(buf2);
#endif
  return rc;
}

wbh_interface_t *wbh_init(const char *tty)
{
  char buf[2048];
  wbh_interface_t *handle = calloc(1, sizeof(wbh_interface_t));
  if (!handle)
    return NULL;
  
  handle->fd = open(tty, O_RDWR|O_NOCTTY|O_NONBLOCK);
  if (handle->fd < 0)
    return NULL;
  
  while (read_nonblock(handle->fd, buf, 2048, 2, 0) > 0) {}
  //write_nonblock(handle->fd, "\r", 1);
  //read_nonblock(handle->fd, buf, 2048, 5);
  write_nonblock(handle->fd, "ATI\r", 4);
  read_nonblock(handle->fd, buf, 255, 15, '>');
  return handle;
}

int wbh_shutdown(wbh_interface_t *iface)
{
  close(iface->fd);
  free(iface);
  return 0;
}

#define BUFSIZE 255

wbh_device_t *wbh_connect(wbh_interface_t *iface, uint8_t device)
{
  char cmd[10];
  char *buf = calloc(1, BUFSIZE);
  if (!buf) {
    perror("calloc");
    return NULL;
  }
  int rc;
  sprintf(cmd, "ATD%02X\r", device);
  write_nonblock(iface->fd, cmd, strlen(cmd));
  memset(buf, 0, 255);
  rc = read_nonblock(iface->fd, buf, 255, 10, '>');
  if (rc < 0) {
    fprintf(stderr, "failed to connect to device %02X: %s\n", device, buf);
    goto error;
  }
  if (strncmp("ERROR", buf, 5) == 0) {
    fprintf(stderr, "ERROR connecting to device %02X\n", device);
    goto error;
  }
  if (strncmp("CONNECT ", buf, 8) != 0) {
    fprintf(stderr, "Unexpected response when connecting to device %02X: %s\n", device, buf);
    goto error;
  }
  wbh_device_t *handle = calloc(1, sizeof(wbh_device_t));
  if (!handle) {
    perror("calloc");
    goto error;
  }
  handle->baudrate = buf[8] - '0';
  handle->protocol = buf[10] - '0';
  handle->specs = buf;
  handle->iface = iface;
  return handle;

error:
  free(buf);
  return NULL;
}

int wbh_disconnect(wbh_device_t *dev)
{
  free((void *)(dev->specs));
  free(dev);
  return 0;
}

int wbh_send_command(wbh_device_t *dev, char *cmd, size_t cmd_size,
                     char *data, size_t data_size, int timeout)
{
  int rc;
  rc = write_nonblock(dev->iface->fd, cmd, cmd_size);
  rc |= write_nonblock(dev->iface->fd, "\r", 1);
  if (rc < 0) {
    perror("write_nonblock");
    return -1;
  }
  rc = read_nonblock(dev->iface->fd, data, data_size, timeout, '>');
  if (rc < 0) {
    perror("read_nonblock");
    return -1;
  }
  int i;
  for (i = 0; i < rc; i++) {
    if (data[i] == '\r')
      data[i] = '\n';
  }
  return rc;
}
