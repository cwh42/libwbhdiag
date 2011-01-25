#include <stdlib.h>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <sys/ioctl.h>
#include "wbh.h"

#define DEBUG

#define ERROR(f, p...) fprintf(stderr, "%s: " f, __FUNCTION__, p)

/** Convert carriage return to line feed.
    @param buf data to be converted
    @param size size of buf
 */
static void crtolf(char *buf, size_t size)
{
  int i;
  for (i = 0; i < size; i++) {
    if (buf[i] == '\r')
      buf[i] = '\n';
  }
}

/** Get response from serial port .
    @param fd serial port file descriptor
    @param buf buffer the input data will be written to
    @param size size of buf
    @param timeout timeout (seconds) before aborting read
    @param expect character signaling end of transmission
    @return number of bytes read or -1 on error
 */
static int serial_read(int fd, char *buf, size_t size, int timeout, int expect)
{
  int rc;
  int osize = size;
  memset(buf, 0, size); /* just want to make sure that stale
                           data is not misinterpreted */
  while (size > 0) {
    /* check if there is data to read */
    int bytes;
    for (; timeout > 0;) {
      ioctl(fd, FIONREAD, &bytes);
      if (bytes > 0) break;
      sleep(1); timeout--;
    }
    if (timeout <= 0) {
      /* read timeout */
      return -1;
    }
    
    rc = read(fd, buf, bytes > size ? size : bytes);
    if (rc < 0) {
      perror("serial_read");
      return -1;
    }
    crtolf(buf, rc);
    
#ifdef DEBUG
    fprintf(stderr, "READ: %s", buf);
#endif

    size -= rc;
    buf += rc;

#ifdef DEBUG
    if (rc >= 1)
      fprintf(stderr, "last char 0x%x\n", buf[-1]);
#endif
    /* check for end-of-transmission character */
    if (expect && rc >= 1 && buf[-1] == expect) {
      return osize - size;
    }
  }
  return osize;
}

/** Send command to serial port.
    @param fd serial port file descriptor
    @param buf command buffer
    @param size size of buf
    @return number of bytes written or -1 on error
 */
static int serial_write(int fd, char *buf, size_t size)
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
  
  handle->fd = open(tty, O_RDWR|O_NOCTTY|O_NDELAY);
  if (handle->fd < 0)
    return NULL;
  
  fcntl(handle->fd, F_SETFL, 0);	/* return immediately even if no data to read */
  tcflush(handle->fd, TCIOFLUSH);	/* flush stale serial buffers */
    
  serial_write(handle->fd, "\r", 1);
  serial_read(handle->fd, buf, 2048, 60, '>');
  
  /* try to elicit an identifying response from WBH interface */
  int i;
  for (i = 0; i < 5; i++) {
    serial_write(handle->fd, "ATI\r", 4);
    serial_read(handle->fd, buf, 255, 150, '>');
    if (!strncmp("WBH-Diag", buf, 8))
      break;
  }
  if (i == 5) {
    ERROR("no response to ATI: %s", buf);
    return NULL;
  }
  
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
  
  /* dial M for murder^Wmotor */
  sprintf(cmd, "ATD%02X\r", device);
  serial_write(iface->fd, cmd, strlen(cmd));

  /* see if we could connect; takes a while, hence the long timeout */
  rc = serial_read(iface->fd, buf, BUFSIZE, 100, '>');
  if (rc < 0) {
    ERROR("failed to connect to device %02X: %s\n", device, buf);
    goto error;
  }
  
  /* check for error conditions */
  if (strncmp("ERROR", buf, 5) == 0) {
    ERROR("received ERROR connecting to device %02X\n", device);
    goto error;
  }
  if (strncmp("CONNECT: ", buf, 9) != 0) {
    ERROR("unexpected response when connecting to device %02X: %s\n", device, buf);
    goto error;
  }
  
  /* successful, fill in the device structure */
  wbh_device_t *handle = calloc(1, sizeof(wbh_device_t));
  if (!handle) {
    perror("calloc");
    goto error;
  }
  handle->baudrate = buf[9] - '0';
  handle->protocol = buf[11] - '0';
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

int wbh_send_command(wbh_device_t *dev, char *cmd, char *data,
                     size_t data_size, int timeout)
{
  int rc;
  
  /* send command plus carriage return */
  rc = serial_write(dev->iface->fd, cmd, strlen(cmd));
  rc |= serial_write(dev->iface->fd, "\r", 1);
  if (rc < 0) {
    perror("serial_write");
    return -1;
  }
  
  /* read response */
  rc = serial_read(dev->iface->fd, data, data_size, timeout, '>');
  if (rc < 0) {
    perror("serial_read");
    return -1;
  }
  crtolf(data, rc);
  
  /* clip trailing '>' */
  if (rc > 0 && data [rc - 1] == '>')
    data[rc - 1] = 0;
  
  return rc;
}
