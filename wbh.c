#include <stdlib.h>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <math.h>
#include "wbh.h"

//#define DEBUG

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

uint8_t *wbh_scan_devices(wbh_interface_t *iface, uint8_t start, uint8_t end)
{
  uint8_t i;
  uint8_t *devices = NULL;
  int device_count = 0;
  for (i = start; i != end; i++) {
#ifdef DEBUG
    fprintf(stderr, "trying device %02X... ", i);
#endif
    wbh_device_t *dev;
    dev = wbh_connect(iface, i);
    if (dev) {
#ifdef DEBUG
      fprintf(stderr, "success!\n");
#endif
      devices = realloc(devices, device_count + 1);
      devices[device_count] = i;
      device_count++;
      wbh_disconnect(dev);
    }
#ifdef DEBUG
    else {
      fprintf(stderr, "failed\n");
    }
#endif
  }
  devices = realloc(devices, device_count + 1);
  devices[device_count] = 0;
  return devices;
}

void wbh_free_devices(uint8_t *devices)
{
  free(devices);
}

int wbh_actuator_diagnosis(wbh_device_t *dev)
{
  char buf[BUFSIZE];
  int rc;
  if ((rc = wbh_send_command(dev, "03", buf, BUFSIZE, 30)) < 0)
    return rc;
  if (!strncmp("END", buf, 3))
    return 0;
  return strtol(buf, NULL, 16);
}

/** measurement value calculation formula function type */
typedef void(*formula_func_t)(uint8_t, uint8_t, wbh_measurement_t *, uint8_t);

/** macro for defining formula functions */
#define def_form(name, formula, eunit) \
static void form_ ## name (uint8_t a, uint8_t b, wbh_measurement_t *data, uint8_t form) \
{ \
  data->value = formula; \
  data->unit = eunit; \
  data->raw[0] = form; \
  data->raw[1] = a; \
  data->raw[2] = b; \
}

/* The various formulas as defined int the WBH-Diag Pro datasheet */
def_form(unknown, 0, UNIT_UNKNOWN)
def_form(1, .2 * a * b, UNIT_RPM)
def_form(2, a * .002 * b, UNIT_PERCENT)
def_form(3, .002 * a * b, UNIT_DEG)
def_form(4, fabs(b - 127.0) * .01 * a, UNIT_UNKNOWN /* FIXME */)
def_form(5, a * (b - 100.0) * .1, UNIT_CELSIUS)
def_form(6, .001 * a * b, UNIT_VOLT)
def_form(7, .01 * a * b, UNIT_KMH)
def_form(8, .1 * a * b, UNIT_NONE)
def_form(9, (b - 127.0) * .02 * a, UNIT_DEG)
def_form(10, b, UNIT_NONE /* FIXME: "cold"/"warm" */)
def_form(11, .0001 * a * (b - 128.0) + 1, UNIT_NONE)
def_form(12, .001 * a * b, UNIT_OHM)
def_form(13, (b - 127.0) * .001 * a, UNIT_MILLIMETER)
def_form(14, .005 * a * b, UNIT_BAR)
def_form(15, .01 * a * b, UNIT_MILLISECOND)
def_form(16, 0, UNIT_UNKNOWN /* FIXME: "Bit Wert"?? */)
def_form(17, 0, UNIT_CHARS)
def_form(18, .04 * a * b, UNIT_MILLIBAR)
def_form(19, a * b * .01, UNIT_UNKNOWN /* FIXME: l? I? 1? */)
def_form(20, a * (b - 128.0) / 128.0, UNIT_PERCENT)
def_form(21, .001 * a * b, UNIT_VOLT)
def_form(22, .001 * a * b, UNIT_MILLISECOND)
def_form(23, b / 256.0 * a, UNIT_PERCENT)
def_form(24, .001 * a * b, UNIT_AMPERE)
def_form(25, b * 1.421 + a / 182.0, UNIT_UNKNOWN /* FIXME: g/s? */)
def_form(26, b - a, UNIT_UNKNOWN /* FIXME: celsius? coulomb? */)
def_form(27, fabs(b - 128.0) * .01 * a, UNIT_UNKNOWN /* FIXME: ATDC/BTDC? */)
def_form(28, b - a, UNIT_NONE)
def_form(29, b < a, UNIT_UNKNOWN /* FIXME: 1./2. Kennfeld? */)
def_form(30, b / 12.0 * a, UNIT_DEG_KW)
def_form(31, b / 2560.0 * a, UNIT_CELSIUS)
def_form(32, (b > 128) ? (b - 256.0) : b, UNIT_NONE)
def_form(33, a == 0 ? (100.0 * b) : (100.0 * b) / a, UNIT_PERCENT)
def_form(34, (b - 128.0) * .01 * a, UNIT_KW)
def_form(35, .01 * a * b, UNIT_LITERS_PER_HOUR)
def_form(36, a * 2560.0 + b * 10.0, UNIT_KM)
def_form(38, (b - 128.0) * .001 * a, UNIT_DEG_KW)
def_form(39, b / 256.0 * a, UNIT_MILLIGRAMS_PER_HOUR)
def_form(40, b * .01 + (25.5 * a) - 400, UNIT_AMPERE)
def_form(41, b + a * 255.0, UNIT_AMPERE_HOUR)
def_form(42, b * .1 + (25.5 * a) - 400, UNIT_UNKNOWN /* FIXME: Kw == kW? */)
def_form(43, b * .1 + (25.5 * a), UNIT_VOLT)
def_form(44, 0, UNIT_TIME)
def_form(45, .1 * a * b / 100.0, UNIT_NONE)
def_form(46, (a * b - 3200.0) * .0027, UNIT_DEG_KW)
def_form(47, (b - 128.0) * a, UNIT_MILLISECOND)
def_form(48, b + a * 255.0, UNIT_NONE)
def_form(49, (b / 4.0) * .1 * a, UNIT_MILLIGRAMS_PER_HOUR)
def_form(50, a == 0 ? (b - 128.0) / .01 : (b - 128.0) / (.01 * a), UNIT_MILLIBAR)
def_form(51, ((b - 128.0) / 255.0) * a, UNIT_MILLIGRAMS_PER_HOUR)
def_form(52, b * .02 * a - a, UNIT_NM)
def_form(53, (b - 128.0) * 1.4222 + .006 * a, UNIT_GS)
def_form(54, a * 256.0 + b, UNIT_NONE)
def_form(55, a * b / 200.0, UNIT_SECOND)
def_form(56, a * 256.0 + b, UNIT_UNKNOWN /* FIXME: WSC? */)
def_form(57, a * 256.0 + b + 65536.0, UNIT_UNKNOWN /* FIXME: WSC? */)
def_form(58, b > 128 ? 1.0225 * (256.0 - b) : 1.0225 * b, UNIT_UNKNOWN /* FIXME: \s? */)
def_form(59, (a * 256.0 + b) / 32768.0, UNIT_NONE)
def_form(60, (a * 256.0 + b) * .01, UNIT_SECOND)
def_form(61, a == 0 ? (b - 128.0) : (b - 128.0) / a, UNIT_NONE)
def_form(62, .256 * a * b, UNIT_UNKNOWN /* FIXME: (capital) S? */)
def_form(63, 0, UNIT_CHARS /* FIXME: with a question mark? */)
def_form(64, a + b, UNIT_OHM)
def_form(65, .01 * a * (b - 127.0), UNIT_MILLIMETER)
def_form(66, (a * b) / 511.12, UNIT_VOLT)
def_form(67, (640.0 * a) + b * 2.5, UNIT_DEG)
def_form(68, (256.0 * a + b) / 7.365, UNIT_DEG_PER_SECOND)
def_form(69, (256.0 * a + b) * .3254, UNIT_BAR)
def_form(70, (256.0 * a + b) * .192, UNIT_METERS_PER_SECOND_SQUARED)

#undef def_form

/** array of implementations of the various formulas */
static formula_func_t formulas[] = {
  form_unknown,
  form_1,
  form_2,
  form_3,
  form_4,
  form_5,
  form_6,
  form_7,
  form_8,
  form_9,
  form_10,
  form_11,
  form_12,
  form_13,
  form_14,
  form_15,
  form_16,
  form_17,
  form_18,
  form_19,
  form_20,
  form_21,
  form_22,
  form_23,
  form_24,
  form_25,
  form_26,
  form_27,
  form_28,
  form_29,
  form_30,
  form_31,
  form_32,
  form_33,
  form_34,
  form_35,
  form_36,
  form_unknown,
  form_38,
  form_39,
  form_40,
  form_41,
  form_42,
  form_43,
  form_44,
  form_45,
  form_46,
  form_47,
  form_48,
  form_49,
  form_50,
  form_51,
  form_52,
  form_53,
  form_54,
  form_55,
  form_56,
  form_57,
  form_58,
  form_59,
  form_60,
  form_61,
  form_62,
  form_63,
  form_64,
  form_65,
  form_66,
  form_67,
  form_68,
  form_69,
  form_70,
};

wbh_measurement_t *wbh_read_measurements(wbh_device_t *dev, uint8_t group)
{
  wbh_measurement_t *data = NULL;
  int data_count = 0;
  char buf[BUFSIZE];
  int rc;
  sprintf(buf, "08%02X", group);
  if ((rc = wbh_send_command(dev, buf, buf, BUFSIZE, 30)) < 0)
    return NULL;
  if (buf[0] > '4') {
    wbh_error = "parsing of this device's response not implemented yet";
    return NULL;
  }
  char *bbuf = buf;
  uint8_t formula, a, b;
  while (sscanf(bbuf, "%02hhX %02hhX %02hhX\n", &formula, &a, &b) == 3) {
    data = realloc(data, (data_count + 1) * sizeof(wbh_measurement_t));
    if (formula < sizeof(formulas) / sizeof(formula_func_t))
      formulas[formula](a, b, &data[data_count], formula);
    else
      form_unknown(a, b, &data[data_count], formula);
    data_count++;
    bbuf += 9;
  }
  data = realloc(data, (data_count + 1) * sizeof(wbh_measurement_t));
  memset(&data[data_count], 0, sizeof(wbh_measurement_t));
  return data;
}

/** human-readable names of units */
static const char *unit_names[] = {
  [UNIT_ENDOFLIST] = "(end of list)",
  [UNIT_RPM] = "RPM",
  [UNIT_PERCENT] = "%",
  [UNIT_DEG] = "°",
  [UNIT_CELSIUS] = "°C",
  [UNIT_VOLT] = "V",
  [UNIT_KMH] = "km/h",
  [UNIT_OHM] = "Ohm",
  [UNIT_MILLIMETER] = "mm",
  [UNIT_BAR] = "bar",
  [UNIT_MILLISECOND] = "ms",
  [UNIT_MILLIBAR] = "mbar",
  [UNIT_AMPERE] = "A",
  [UNIT_DEG_KW] = "Deg k/w",
  [UNIT_KW] = "kW",
  [UNIT_LITERS_PER_HOUR] = "l/h",
  [UNIT_KM] = "km",
  [UNIT_MILLIGRAMS_PER_HOUR] = "mg/h",
  [UNIT_AMPERE_HOUR] = "Ah",
  [UNIT_TIME] = "h",
  [UNIT_NM] = "Nm",
  [UNIT_SECOND] = "s",
  [UNIT_METERS_PER_SECOND_SQUARED] = "m/s^2",
  [UNIT_CHARS] = "",
  [UNIT_GS] = "g/s",
  [UNIT_DEG_PER_SECOND] = "deg/s",
  [UNIT_NONE] = "",
  [UNIT_UNKNOWN] = "(unknown unit)",
};

const char *wbh_unit_name(wbh_unit_t unit)
{
  return unit_names[unit];
}
