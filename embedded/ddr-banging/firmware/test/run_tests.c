#include "check.h"

int check_failures = 0;

void test_ddr3(void);
void test_spd(void);
void test_bus(void);
void test_link(void);

int main(void)
{
    test_ddr3();
    test_spd();
    test_bus();
    test_link();
    if (check_failures) {
        printf("%d check(s) failed\n", check_failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
