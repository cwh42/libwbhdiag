#include "wbh.h"
#include <stdio.h>

int main(int argc, char **argv)
{
  wbh_interface_t *iface;
  wbh_device_t *dev;
  char buf[255];
  
  iface = wbh_init("/dev/rfcomm1");
  //iface = wbh_init("./pipe");
  if (!iface) {
    fprintf(stderr, "failed to connect to interface\n");
    return 1;
  }
  dev = wbh_connect(iface, 0x01);
  if (!dev) {
    fprintf(stderr, "failed to connect to device\n");
    return 2;
  }
  if (wbh_send_command(dev, "00", buf, 255, 30) > 0) {
    printf("result: %s\n", buf);
    return 0;
  }
  else {
    fprintf(stderr, "failed to send command\n");
    return 3;
  }
}
