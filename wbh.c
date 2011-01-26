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

static char *wbh_error = NULL;

/** standard buffer size, saves us from thinking up a suitable number all
    the time... */
#define BUFSIZE 255

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
#ifdef DEBUG
  char *obuf = buf;
#endif
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
      wbh_error = "timeout reading from serial port";
      return -ERR_TIMEOUT;
    }
    
    rc = read(fd, buf, bytes > size ? size : bytes);
    if (rc < 0) {
      wbh_error = "I/O error reading from serial port";
      return -ERR_SERIAL;
    }
    crtolf(buf, rc);
    
    size -= rc;
    buf += rc;

    /* check for end-of-transmission character */
    if (expect && rc >= 1 && buf[-1] == expect) {
#ifdef DEBUG
      fprintf(stderr, "READ: %s\n", obuf);
#endif
      return osize - size;
    }
  }

#ifdef DEBUG
    fprintf(stderr, "READ: %s\n", obuf);
#endif
  return osize;
}

/** Wait for "ready" prompt ('>').
    @param fd serial port file descriptor
    @param timeout timeout in seconds
    @return number of bytes read or -1 on error
 */
static int wait_for_prompt(int fd, int timeout)
{
  char buf[BUFSIZE];
  return serial_read(fd, buf, BUFSIZE, timeout, '>');
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
  if (!handle) {
    wbh_error = "wbh_init: calloc() failed";
    return NULL;
  }
  
  handle->fd = open(tty, O_RDWR|O_NOCTTY|O_NDELAY);
  if (handle->fd < 0) {
    wbh_error = "failed to open TTY";
    return NULL;
  }
  
  fcntl(handle->fd, F_SETFL, 0);	/* return immediately even if no data to read */
  
  /* put TTY in raw mode */
  struct termios tio;
  tcgetattr(handle->fd, &tio);
  cfmakeraw(&tio);
  tcsetattr(handle->fd, TCSANOW, &tio);
  
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
    close(handle->fd);
    free(handle);
    wbh_error = "no response to ATI";
    return NULL;
  }
  
  handle->name = strdup(tty);
  
  return handle;
}

int wbh_shutdown(wbh_interface_t *iface)
{
  close(iface->fd);
  free(iface->name);
  free(iface);
  return 0;
}

wbh_device_t *wbh_connect(wbh_interface_t *iface, uint8_t device)
{
  char cmd[10];
  char *buf = calloc(1, BUFSIZE);
  if (!buf) {
    wbh_error = "wbh_connect: calloc() failed";
    return NULL;
  }
  int rc;
  
  /* dial M for murder^Wmotor */
  sprintf(cmd, "ATD%02X\r", device);
  serial_write(iface->fd, cmd, strlen(cmd));

  /* see if we could connect; takes a while, hence the long timeout */
  rc = serial_read(iface->fd, buf, BUFSIZE, 100, '>');
  if (rc < 0) {
    ERROR("failed to connect to device %02X, error code %d: %s\n", device, -rc, buf);
    wbh_error = "failed to connect to device";
    goto error;
  }
  
  /* check for error conditions */
  if (strncmp("ERROR", buf, 5) == 0) {
    ERROR("received ERROR connecting to device %02X\n", device);
    wbh_error = "received \"ERROR\" trying to connect to device";
    goto error;
  }
  if (strncmp("CONNECT: ", buf, 9) != 0) {
    ERROR("unexpected response when connecting to device %02X: %s\n", device, buf);
    wbh_error = "unexpected response when connecting to device";
    goto error;
  }
  
  /* successful, fill in the device structure */
  wbh_device_t *handle = calloc(1, sizeof(wbh_device_t));
  if (!handle) {
    wbh_error = "wbh_connect: calloc() failed";
    goto error;
  }
  handle->baudrate = buf[9] - '0';
  handle->protocol = buf[11] - '0';
  handle->specs = buf;
  handle->iface = iface;
  handle->id = device;
  return handle;

error:
  free(buf);
  return NULL;
}

int wbh_disconnect(wbh_device_t *dev)
{
  int rc;
  /* hang up and flush serial buffers */
  serial_write(dev->iface->fd, "ATH\r", 4);
  if ((rc = wait_for_prompt(dev->iface->fd, 10)) < 0) {
    ERROR("error %d while disconnecting from device %02X\n", -rc, dev->id);
    return rc;
  }
  tcflush(dev->iface->fd, TCIOFLUSH);
  
  /* free device handle */
  free((void *)(dev->specs));
  free(dev);
  
  return 0;
}

int wbh_reset(wbh_interface_t *iface)
{
  int rc;
  
  /* send ATZ */
  serial_write(iface->fd, "ATZ\r", 4);
  if ((rc = wait_for_prompt(iface->fd, 10)) < 0) {
    ERROR("error %d while resetting interface %s\n", -rc, iface->name);
    return rc;
  }
  return 0;
}

int wbh_send_command(wbh_device_t *dev, char *cmd, char *data,
                     size_t data_size, int timeout)
{
  int rc;
  
  /* send command plus carriage return */
  rc = serial_write(dev->iface->fd, cmd, strlen(cmd));
  if (rc < 0)
    return rc;
  rc = serial_write(dev->iface->fd, "\r", 1);
  if (rc < 0)
    return rc;
  
  /* read response */
  rc = serial_read(dev->iface->fd, data, data_size, timeout, '>');
  if (rc < 0)
    return rc;
  crtolf(data, rc);
  
  /* clip trailing '>' */
  if (rc > 0 && data [rc - 1] == '>')
    data[rc - 1] = 0;
  
  return rc;
}

int wbh_get_analog(wbh_interface_t *iface, uint8_t pin)
{
  char buf[BUFSIZE];
  int rc;

  /* pins 0..5 are valid */
  if (pin > 5) {
    wbh_error = "invalid analog pin";
    return -ERR_INVAL;
  }

  sprintf(buf, "ATA%d\r", pin);
  serial_write(iface->fd, buf, strlen(buf));

  rc = serial_read(iface->fd, buf, BUFSIZE, 3, '>');
  if (rc < 0)
    return rc;
  
  return atoi(buf);	/* FIXME: untested, is this really a decimal value? */
}

/** get BDT or IBT as desired */
static int wbh_get_xxt(wbh_interface_t *iface, const char *which_t)
{
  char buf[BUFSIZE];
  int rc;
  sprintf(buf, "AT%s?\r", which_t);
  serial_write(iface->fd, buf, strlen(buf));
  rc = serial_read(iface->fd, buf, BUFSIZE, 3, '>');
  if (rc < 0)
    return rc;
  
  return strtol(buf, NULL, 16);	/* FIXME: untested, is this really a hex value? */
}

int wbh_get_bdt(wbh_interface_t *iface)
{
  return wbh_get_xxt(iface, "BDT");
}
int wbh_get_ibt(wbh_interface_t *iface)
{
  return wbh_get_xxt(iface, "IBT");
}

/** set BDT or IBT as desired */
static int wbh_set_xxt (wbh_interface_t *iface, uint8_t xxt, const char *which_t)
{
  char buf[BUFSIZE];
  int rc;
  sprintf(buf, "AT%s%02X\r", which_t, xxt);
  serial_write(iface->fd, buf, strlen(buf));
  if ((rc = wait_for_prompt(iface->fd, 3)) < 0)
    return rc;
  return 0;
}

int wbh_set_bdt(wbh_interface_t *iface, uint8_t bdt)
{
  return wbh_set_xxt(iface, bdt, "BDT");
}
int wbh_set_ibt(wbh_interface_t *iface, uint8_t ibt)
{
  return wbh_set_xxt(iface, ibt, "IBT");
}

const char *wbh_get_error(void)
{
  return wbh_error;
}

int wbh_force_baud_rate(wbh_interface_t *iface, wbh_baudrate_t baudrate)
{
  char buf[BUFSIZE];
  int rc;
  if (baudrate < BAUD_AUTO || baudrate > BAUD_10400) {
    wbh_error = "invalid baud rate";
    return -ERR_INVAL;
  }
  sprintf(buf, "ATN%d\r", baudrate);
  serial_write(iface->fd, buf, strlen(buf));
  if ((rc = wait_for_prompt(iface->fd, 3)) < 0) {
    return rc;
  }
  return 0;
}

wbh_dtc_t *wbh_get_dtc(wbh_device_t *dev)
{
  char buf[BUFSIZE];
  int rc;
  serial_write(dev->iface->fd, "02\r", 3);
  if ((rc = serial_read(dev->iface->fd, buf, BUFSIZE, 100, '>')) < 0)
    return NULL;
  uint16_t error;
  uint8_t status;
  wbh_dtc_t *list = NULL;
  int list_size = 0;
  char *bbuf = buf;
  while (sscanf(bbuf, "%04hX %02hhX\n", &error, &status) == 2) {
    list = realloc(list, (list_size + 1) * sizeof(wbh_dtc_t));
    list[list_size].error_code = error;
    list[list_size].status_code = status;
    list_size++;
    bbuf += 8;
  }
  list = realloc(list, (list_size + 1) * sizeof(wbh_dtc_t));
  list[list_size].error_code = 0;
  list[list_size].status_code = 0;
  return list;
}

void wbh_free_dtc(wbh_dtc_t *dtc)
{
  free(dtc);
}
