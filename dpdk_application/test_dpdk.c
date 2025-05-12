#include <rte_debug.h>
#include <rte_eal.h>
#include <rte_version.h> // For getting DPDK version
#include <stdio.h>

int main(int argc, char *argv[]) {
  int ret;

  // Initialize the Environment Abstraction Layer (EAL)
  ret = rte_eal_init(argc, argv);
  if (ret < 0) {
    rte_panic("ERROR: Cannot init EAL\n");
  }

  // If EAL initialization is successful, print a success message
  // and the DPDK version.
  printf("EAL initialized successfully.\n");
  printf("DPDK Version: %s\n", rte_version());

  // Clean up the EAL
  // This should be called when the application is exiting.
  rte_eal_cleanup();

  printf("EAL cleaned up.\n");

  return 0;
}
