#include "check.h"

int check_failures = 0;

void test_ddr3(void);
void test_spd(void);
void test_bus(void);
void test_link(void);
void test_jedec(void);
void test_pio_enc(void);
void test_pio_cmd(void);

int main(void)
{
    test_ddr3();
    test_spd();
    test_bus();
    test_link();
    test_jedec();
    test_pio_enc();
    test_pio_cmd();
    if (check_failures) {
        printf("%d check(s) failed\n", check_failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
