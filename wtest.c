#include "wbh.h"
#include <stdio.h>

#define DEVICE "/dev/rfcomm1"
#define BAUDRATE BAUD_9600

#define CONNDEV 0x1
#define ACTUATORDEV 0x35

#define SCANSTART 1
#define SCANEND 0x7f

#define PRINT_ERROR fprintf(stderr, "%s %d: %s\n", argv[0], __LINE__, wbh_get_error());
#define INFO(x, y...) fprintf(stderr, "INFO " x "\n", y)

int main(int argc, char **argv)
{
  wbh_interface_t *iface;
  wbh_device_t *dev;
  char buf[255];
  
  INFO("connecting to %s", DEVICE);
  iface = wbh_init(DEVICE);
  //iface = wbh_init("./pipe");
  if (!iface) {
    PRINT_ERROR
    return 1;
  }
  
  INFO("forcing baud rate to %d", BAUDRATE);
  wbh_force_baud_rate(iface, BAUDRATE);
  
  INFO("connecting to device 0x%x", CONNDEV);
  dev = wbh_connect(iface, CONNDEV);

  if (!dev) {
    PRINT_ERROR
    return 2;
  }
  if (wbh_send_command(dev, "00", buf, 255, 30) > 0) {
    printf("result: %s\n", buf);
  }
  else {
    PRINT_ERROR
    return 3;
  }

  wbh_dtc_t *dtc;
  INFO("getting error codes from 0x%x", CONNDEV);
  if ((dtc = wbh_get_dtc(dev))) {
    int i;
    for (i = 0; dtc[i].error_code; i++) {
      printf("error %d: %d/%d\n", i, dtc[i].error_code, dtc[i].status_code);
    }
    wbh_free_dtc(dtc);
  }
  
  INFO("getting measurements group 1 from 0x%x", CONNDEV);
  wbh_measurement_t *data;
  if ((data = wbh_read_measurements(dev, 1))) {
    int i;
    for (i = 0; data[i].unit != UNIT_ENDOFLIST; i++) {
      printf("value %d: %f %s\n", i, data[i].value, wbh_unit_name(data[i].unit));
    }
  }
  else {
    PRINT_ERROR
  }
  
  INFO("disconnecting from 0x%x", CONNDEV);
  wbh_disconnect(dev);
  
  INFO("connecting to device 0x%x", ACTUATORDEV);
  dev = wbh_connect(iface, ACTUATORDEV);
  INFO("commencing actuator test on 0x%x", ACTUATORDEV);
  for (;;) {
    int rc;
    rc = wbh_actuator_diagnosis(dev);
    if (rc <= 0)
      break;
    printf("actuator diagnosis component code %04X\n", rc);
  }
  wbh_disconnect(dev);
#if 1
  uint8_t *devices;
  INFO("starting device scan from 0x%x to 0x%x", SCANSTART, SCANEND);
  if ((devices = wbh_scan_devices(iface, SCANSTART, SCANEND))) {
    int i;
    for (i = 0; devices[i]; i++) {
      printf("device %02X reachable\n", devices[i]);
    }
    wbh_free_devices(devices);
  }
#endif
  wbh_reset(iface);
  return 0;
}
