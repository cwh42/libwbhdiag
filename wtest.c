#include "wbh.h"
#include <stdio.h>

#define PRINT_ERROR fprintf(stderr, "%s %d: %s\n", argv[0], __LINE__, wbh_get_error());

int main(int argc, char **argv)
{
  wbh_interface_t *iface;
  wbh_device_t *dev;
  char buf[255];
  
  iface = wbh_init("/dev/rfcomm1");
  //iface = wbh_init("./pipe");
  if (!iface) {
    PRINT_ERROR
    return 1;
  }
  
  wbh_force_baud_rate(iface, BAUD_9600);
  
  dev = wbh_connect(iface, 0x35);
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
  if ((dtc = wbh_get_dtc(dev))) {
    int i;
    for (i = 0; dtc[i].error_code; i++) {
      printf("error %d: %d/%d\n", i, dtc[i].error_code, dtc[i].status_code);
    }
    wbh_free_dtc(dtc);
  }
  wbh_disconnect(dev);
  
  dev = wbh_connect(iface, 0x35);
  for (;;) {
    int rc;
    rc = wbh_actuator_diagnosis(dev);
    if (rc <= 0)
      break;
    printf("actuator diagnosis component code %04X\n", rc);
  }
#if 0
  uint8_t *devices;
  if ((devices = wbh_scan_devices(iface, 1, 0x7f))) {
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
