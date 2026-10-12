#include "check.h"

int check_failures = 0;

void test_ddr3(void);
void test_spd(void);

int main(void)
{
    test_ddr3();
    test_spd();
    if (check_failures) {
        printf("%d check(s) failed\n", check_failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
