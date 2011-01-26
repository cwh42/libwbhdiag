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
  dev = wbh_connect(iface, 0x01);
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
  wbh_disconnect(dev);
  wbh_reset(iface);
  return 0;
}
