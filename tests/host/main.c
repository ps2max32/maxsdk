#include "test.h"

int test_failures;
int test_malloc(void);

int main(void) {
  test_gen_fields();
  test_pkt();
  test_emit();
  test_chain();
  test_video();
  test_tex();
  test_vcache();
  test_failures += test_malloc();
  if (test_failures) {
    printf("host tests: %d FAILED\n", test_failures);
    return 1;
  }
  printf("host tests: all passed\n");
  return 0;
}
