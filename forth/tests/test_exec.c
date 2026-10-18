/*
 * test_exec.c — compile programs and actually run them on this host.
 *
 * The execution harness (host/exec.c) maps each image executable and
 * calls its entry point with an ordinary buffer standing in for the BAR
 * scratch/result region. Only runs on x86-64 hosts; skipped elsewhere.
 */
#include <inttypes.h>
#include <string.h>

#include "check.h"
#include "exec.h"

static _Alignas(16) uint8_t region[FORTH_REGION_SIZE];

static int64_t cell(unsigned off)
{
    int64_t v;
    memcpy(&v, region + off, sizeof v);
    return v;
}

static void set_cell(unsigned off, int64_t v)
{
    memcpy(region + off, &v, sizeof v);
}

/* Run an already-compiled image; region is left for inspection. */
static int run_image(const char *src, const uint8_t *img, size_t len,
                     size_t entry, forth_run_status want)
{
    forth_run_status rs = forth_host_run(img, len, entry, region, 5);
    CHECK(rs == want, "\"%.60s\": run gave %s, expected %s", src,
          forth_run_status_name(rs), forth_run_status_name(want));
    return rs == want;
}

/* Compile and run `src` without clearing the region first. */
static int exec_keep(const char *src)
{
    forth_ctx ctx;
    const uint8_t *img = compile_ok(src, &ctx);
    return img != NULL &&
           run_image(src, img, ctx.code_len, 0, FORTH_RUN_OK);
}

static int exec(const char *src)
{
    memset(region, 0, sizeof region);
    return exec_keep(src);
}

/* Run `src`, which must call result! and leave the stack empty. */
static void expect_result(const char *src, int64_t want)
{
    if (!exec(src)) {
        return;
    }
    CHECK(cell(FORTH_REG_DONE) == 1, "\"%.60s\": done flag not set", src);
    CHECK(cell(FORTH_REG_RESULT) == want,
          "\"%.60s\": result %" PRId64 ", expected %" PRId64, src,
          cell(FORTH_REG_RESULT), want);
    CHECK(cell(FORTH_REG_DEPTH) == 0, "\"%.60s\": stack depth %" PRId64
          " at exit, expected 0", src, cell(FORTH_REG_DEPTH));
}

/* Run `src` and compare the whole data stack, bottom first. */
static void expect_stack(const char *src, int n, const int64_t *want)
{
    if (!exec(src)) {
        return;
    }
    int64_t depth = cell(FORTH_REG_DEPTH);
    CHECK(depth == n, "\"%s\": depth %" PRId64 ", expected %d", src, depth, n);
    if (depth != n) {
        return;
    }
    for (int k = 0; k < n; k++) {
        /* Item k from the top: the TOS register, then upwards from rbp. */
        int64_t got = k == 0 ? cell(FORTH_REG_TOS)
                             : cell(FORTH_DSTACK_TOP - 8u * (unsigned)n +
                                    8u * (unsigned)(k - 1));
        CHECK(got == want[n - 1 - k], "\"%s\": item %d from top is %" PRId64
              ", expected %" PRId64, src, k, got, want[n - 1 - k]);
    }
}

#define STACK(src, ...)                                                    \
    do {                                                                   \
        static const int64_t want_[] = { __VA_ARGS__ };                    \
        expect_stack(src, (int)(sizeof want_ / sizeof want_[0]), want_);   \
    } while (0)

/* ---- Reference semantics for the cross-check --------------------------- */

static const int64_t values[] = {
    0, 1, -1, 2, -2, 7, -7, 13, 1000003, 0x1234567890,
    INT64_MAX, INT64_MIN,
};
#define NVALUES (sizeof values / sizeof values[0])

static int64_t wrap(uint64_t v)
{
    int64_t r;
    memcpy(&r, &v, sizeof r);
    return r;
}

/* Returns the expected result; sets *div0 if the flag should be raised. */
static int64_t ref_binary(const char *op, int64_t a, int64_t b, int *div0)
{
    uint64_t ua = (uint64_t)a, ub = (uint64_t)b;
    *div0 = 0;
    if (!strcmp(op, "+"))   return wrap(ua + ub);
    if (!strcmp(op, "-"))   return wrap(ua - ub);
    if (!strcmp(op, "*"))   return wrap(ua * ub);
    if (!strcmp(op, "/")) {
        if (b == 0) { *div0 = 1; return 0; }
        if (b == -1) return wrap(0 - ua);
        return a / b;
    }
    if (!strcmp(op, "mod")) {
        if (b == 0) { *div0 = 1; return a; }
        if (b == -1) return 0;
        return a % b;
    }
    if (!strcmp(op, "min")) return a < b ? a : b;
    if (!strcmp(op, "max")) return a > b ? a : b;
    if (!strcmp(op, "and")) return wrap(ua & ub);
    if (!strcmp(op, "or"))  return wrap(ua | ub);
    if (!strcmp(op, "xor")) return wrap(ua ^ ub);
    if (!strcmp(op, "="))   return a == b ? -1 : 0;
    if (!strcmp(op, "<>"))  return a != b ? -1 : 0;
    if (!strcmp(op, "<"))   return a < b ? -1 : 0;
    if (!strcmp(op, ">"))   return a > b ? -1 : 0;
    if (!strcmp(op, "lshift")) return ub >= 64 ? 0 : wrap(ua << ub);
    if (!strcmp(op, "rshift")) return ub >= 64 ? 0 : wrap(ua >> ub);
    return 0x5EED;
}

static int64_t ref_unary(const char *op, int64_t a)
{
    uint64_t ua = (uint64_t)a;
    if (!strcmp(op, "negate")) return wrap(0 - ua);
    if (!strcmp(op, "abs"))    return a < 0 ? wrap(0 - ua) : a;
    if (!strcmp(op, "1+"))     return wrap(ua + 1);
    if (!strcmp(op, "1-"))     return wrap(ua - 1);
    if (!strcmp(op, "invert")) return wrap(~ua);
    if (!strcmp(op, "0="))     return a == 0 ? -1 : 0;
    if (!strcmp(op, "0<"))     return a < 0 ? -1 : 0;
    return 0x5EED;
}

static void check_operators(void)
{
    static const char *const binary[] = {
        "+", "-", "*", "/", "mod", "min", "max", "and", "or", "xor",
        "=", "<>", "<", ">", "lshift", "rshift",
    };
    static const char *const unary[] = {
        "negate", "abs", "1+", "1-", "invert", "0=", "0<",
    };
    char src[128];

    for (size_t o = 0; o < sizeof binary / sizeof binary[0]; o++) {
        for (size_t i = 0; i < NVALUES; i++) {
            for (size_t j = 0; j < NVALUES; j++) {
                int div0;
                int64_t want = ref_binary(binary[o], values[i], values[j],
                                          &div0);
                snprintf(src, sizeof src, "%" PRId64 " %" PRId64 " %s result!",
                         values[i], values[j], binary[o]);
                expect_result(src, want);
                CHECK(cell(FORTH_REG_FLAGS) ==
                          (div0 ? FORTH_FLAG_DIV_ZERO : 0),
                      "\"%s\": flags %" PRId64, src, cell(FORTH_REG_FLAGS));
            }
        }
    }
    for (size_t o = 0; o < sizeof unary / sizeof unary[0]; o++) {
        for (size_t i = 0; i < NVALUES; i++) {
            snprintf(src, sizeof src, "%" PRId64 " %s result!", values[i],
                     unary[o]);
            expect_result(src, ref_unary(unary[o], values[i]));
        }
    }
}

static void check_stack_words(void)
{
    STACK("1 dup", 1, 1);
    STACK("1 2 drop", 1);
    STACK("1 2 swap", 2, 1);
    STACK("1 2 over", 1, 2, 1);
    STACK("1 2 3 rot", 2, 3, 1);
    STACK("1 2 3 rot rot rot", 1, 2, 3);
    STACK("5 >r 7 r@ r> + +", 17);
    STACK("1 2 >r >r r> r>", 1, 2);
    STACK("$7fffffffffffffff -9223372036854775808 4294967296 -2147483649",
          INT64_MAX, INT64_MIN, 4294967296, -2147483649LL);
}

/* Fixed-point (12 fractional bits) Mandelbrot: total iterations over a
 * 32 x 16 grid covering re -2..1, im -1..1, at most 50 per point. */
static const char mandelbrot_src[] =
    "\\ cells in program memory\n"
    ": zr 64 ; : zi 72 ; : cre 80 ; : cim 88 ; : n 96 ; : total 104 ;\n"
    ": fx* ( a b -- a*b ) * 4096 / ;\n"
    ": escaped? ( -- f ) zr @ dup fx* zi @ dup fx* + 16384 > ;\n"
    ": iterate ( -- count )\n"
    "  0 zr ! 0 zi ! 0 n !\n"
    "  begin escaped? 0= n @ 50 < and while\n"
    "    zr @ dup fx* zi @ dup fx* - cre @ +   ( new zr )\n"
    "    zr @ zi @ fx* 2 * cim @ + zi !\n"
    "    zr !\n"
    "    n @ 1+ n !\n"
    "  repeat n @ ;\n"
    ": mandel 0 total !\n"
    "  16 0 do i 512 * 4096 - cim !\n"
    "    32 0 do i 384 * 8192 - cre ! iterate total @ + total ! loop\n"
    "  loop total @ ;\n"
    "mandel result!\n";

static int64_t mandelbrot_reference(void)
{
    int64_t total = 0;
    for (int64_t y = 0; y < 16; y++) {
        int64_t cim = y * 512 - 4096;
        for (int64_t x = 0; x < 32; x++) {
            int64_t cre = x * 384 - 8192, zr = 0, zi = 0, n = 0;
            while (!(zr * zr / 4096 + zi * zi / 4096 > 16384) && n < 50) {
                int64_t nzr = zr * zr / 4096 - zi * zi / 4096 + cre;
                zi = zr * zi / 4096 * 2 + cim;
                zr = nzr;
                n++;
            }
            total += n;
        }
    }
    return total;
}

static void check_programs(void)
{
    expect_result(": answer 6 7 * result! ; answer", 42);
    expect_result(": sq dup * ; : cube dup sq * ; : f 3 cube 2 sq + ; "
                  "f result!", 31);
    /* A redefinition shadows the primitive for code compiled after it. */
    expect_result(": sub - ; : + sub ; 10 3 + result!", 7);
    expect_result("2 3 SWAP - Result!", 1);
    expect_result(": sign dup 0< if drop -1 else 0= if 0 else 1 then then ;"
                  " -5 sign 100 * 0 sign 10 * + 7 sign + result!", -99);
    expect_result("0 101 1 do i + loop result!", 5050);
    /* Nested loops; `i` is the innermost index, the outer one is kept in
     * memory (no `j` yet). */
    expect_result("0 5 0 do i 64 ! 4 0 do 64 @ i * + loop loop result!", 60);
    expect_result(": fact 1 swap begin dup while swap over * swap 1- repeat"
                  " drop ; 20 fact result!", 2432902008176640000LL);
    expect_result(": gcd begin dup while swap over mod repeat drop ;"
                  " 1071 462 gcd result!", 21);
    expect_result("0 10 begin swap 1+ swap 1- dup 0= until drop result!", 10);
    expect_result("3 0 do loop 99 result!", 99);
    /* Return-stack words interleave correctly with a loop frame. */
    expect_result("0 4 0 do i >r r@ r> + + loop result!", 12);
    expect_result(": sieve 1000 0 do 1 i 256 + c! loop\n"
                  "  1000 2 do i 256 + c@ if\n"
                  "    i i * begin dup 1000 < while 0 over 256 + c! i + repeat"
                  " drop then loop\n"
                  "  0 1000 2 do i 256 + c@ + loop ;\n"
                  "sieve result!", 168);
    expect_result(mandelbrot_src, mandelbrot_reference());

    /* Slice 2 (SINT-104). */
    expect_result("1 63 lshift 63 rshift 1 1 lshift 1 rshift + result!", 2);
    expect_result("0 3 0 do 4 0 do j 10 * i + + loop loop result!", 138);
    expect_result("0 10 0 do i + 3 +loop result!", 18);        /* 0 3 6 9 */
    expect_result("0 0 10 do i + -1 +loop result!", 55);       /* 10 .. 0 */
    expect_result("0 0 10 do i + -3 +loop result!", 22);       /* 10 7 4 1 */
    /* Starting at the limit, a negative step crosses it at once. */
    expect_result("0 7 7 do 1+ -5 +loop result!", 1);
    expect_result("0 -9223372036854775808 9223372036854775807 do 1+ 4 +loop"
                  " result!", 1);
    expect_result("0 9223372036854775807 -9223372036854775808 do 1+"
                  " 4611686018427387904 +loop result!", 4);
    expect_result(": fib dup 2 < if exit then dup 1- recurse swap 2 - recurse"
                  " + ; 20 fib result!", 6765);
    expect_result(": fact dup 1 > if dup 1- recurse * then ; 20 fact result!",
                  2432902008176640000LL);
    /* exit from inside nested loops leaves no loop frames behind. */
    expect_result(": find ( n -- i*j ) 10 0 do 10 0 do dup i j * = if drop"
                  " i j * exit then loop loop drop -1 ;"
                  " 0 >r 42 find r> + result!", 42);
}

static void check_memory(void)
{
    /* Read from and write to memory prepared by the "ESP32". */
    memset(region, 0, sizeof region);
    for (unsigned k = 0; k < 8; k++) {
        set_cell(0x100 + 8 * k, (int64_t)(k + 1) * 1000 + k);
    }
    if (exec_keep("0 8 0 do i 8 * 256 + @ + loop dup 512 ! result!")) {
        CHECK(cell(FORTH_REG_RESULT) == 36028, "memory sum %" PRId64,
              cell(FORTH_REG_RESULT));
        CHECK(cell(0x200) == 36028, "memory store %" PRId64, cell(0x200));
    }

    if (exec("16 0 do i 3 * i 1024 + c! loop "
             "0 16 0 do i 1024 + c@ + loop result!")) {
        CHECK(cell(FORTH_REG_RESULT) == 360, "byte sum %" PRId64,
              cell(FORTH_REG_RESULT));
        for (unsigned k = 0; k < 16; k++) {
            CHECK(region[1024 + k] == 3 * k, "byte %u is %u", k,
                  region[1024 + k]);
        }
        CHECK(region[1024 + 16] == 0, "c! wrote past its byte");
    }
    expect_result("300 1100 c! 1100 c@ result!", 44);
    expect_result("-1 1096 ! 1096 c@ 1103 c@ + result!", 510);

    /* Addresses are masked into the region: nothing escapes it. */
    expect_result("42 4096 64 + ! 64 @ result!", 42);
    expect_result("43 64 ! 67 @ result!", 43);              /* aligned down */
    expect_result("7 -4032 c! 64 c@ result!", 7);           /* -4032 & 0xfff */
    expect_result("$1122334455667788 64 ! 64 @ result!", 0x1122334455667788);
}

static void check_region_protocol(void)
{
    /* The prologue clears done and error flags left by a previous run. */
    memset(region, 0, sizeof region);
    set_cell(FORTH_REG_DONE, 1);
    set_cell(FORTH_REG_FLAGS, 0xFF);
    set_cell(FORTH_REG_RESULT, 1234);
    if (exec_keep("1 2 3")) {
        CHECK(cell(FORTH_REG_DONE) == 0, "done flag not cleared");
        CHECK(cell(FORTH_REG_FLAGS) == 0, "error flags not cleared");
        CHECK(cell(FORTH_REG_RESULT) == 1234, "result touched without result!");
        CHECK(cell(FORTH_REG_DEPTH) == 3, "depth %" PRId64,
              cell(FORTH_REG_DEPTH));
        CHECK(cell(FORTH_REG_TOS) == 3, "tos %" PRId64, cell(FORTH_REG_TOS));
    }
    if (exec("7 0 / result!")) {
        CHECK(cell(FORTH_REG_FLAGS) == FORTH_FLAG_DIV_ZERO,
              "division by zero not flagged");
        CHECK(cell(FORTH_REG_RESULT) == 0, "7/0 gave %" PRId64,
              cell(FORTH_REG_RESULT));
    }
    /* result! can be called more than once; the last value wins. */
    expect_result("1 result! 2 result!", 2);
    /* Underflow is not trapped (no runtime checks yet) but is visible to
     * the ESP32 afterwards as a negative depth. */
    if (exec("drop drop")) {
        CHECK(cell(FORTH_REG_DEPTH) == -2, "underflow depth %" PRId64,
              cell(FORTH_REG_DEPTH));
    }
}

static void check_harness_and_image(void)
{
    static uint8_t shifted[TEST_IMAGE_MAX + 4099];
    const char *src = ": sq dup * ; 0 10 0 do i sq + loop result!";
    forth_ctx ctx;
    const uint8_t *img = compile_ok(src, &ctx);

    if (img == NULL) {
        return;
    }
    /* Position independence: the same bytes run at an odd offset. */
    memset(shifted, 0xCC, sizeof shifted);
    memcpy(shifted + 4099, img, ctx.code_len);
    memset(region, 0, sizeof region);
    if (run_image(src, shifted, 4099 + ctx.code_len, 4099, FORTH_RUN_OK)) {
        CHECK(cell(FORTH_REG_RESULT) == 285, "shifted image gave %" PRId64,
              cell(FORTH_REG_RESULT));
    }
    /* Running off the end of the program lands in the UD2 guard. */
    run_image("guard tail", img, ctx.code_len,
              ctx.code_len - FORTH_GUARD_BYTES, FORTH_RUN_SIGILL);
    /* A runaway program is stopped rather than hanging the harness. */
    {
        forth_ctx c2;
        const uint8_t *loop = compile_ok("begin 0 until", &c2);
        if (loop != NULL) {
            forth_run_status rs = forth_host_run(loop, c2.code_len, 0, region,
                                                 1);
            CHECK(rs == FORTH_RUN_TIMEOUT, "infinite loop gave %s",
                  forth_run_status_name(rs));
        }
    }
}

void test_exec(void)
{
    if (!forth_host_can_execute()) {
        check_skip("exec", "host cannot execute x86-64 code");
        return;
    }
    check_operators();
    check_stack_words();
    check_programs();
    check_memory();
    check_region_protocol();
    check_harness_and_image();
}
