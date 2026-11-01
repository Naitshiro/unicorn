#include <stdio.h>
#include <unicorn/unicorn.h>

#define STACK_ADDRESS 0x700000000000ULL
#define LARGE_ADDRESS 0x100000000000ULL
#define SMALL_ADDRESS 0x200000000000ULL
#define STACK_SIZE (1ULL << 20)
#define LARGE_SIZE (2ULL << 20)
#define SMALL_SIZE (256ULL << 10)

int main(void)
{
    uc_engine *uc;
    uc_err err;

    err = uc_open(UC_ARCH_X86, UC_MODE_64, &uc);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_open: %s\n", uc_strerror(err));
        return 1;
    }

    err = uc_mem_map(uc, STACK_ADDRESS, STACK_SIZE, UC_PROT_ALL);
    if (err == UC_ERR_OK)
        err = uc_mem_map(uc, LARGE_ADDRESS, LARGE_SIZE, UC_PROT_ALL);
    if (err == UC_ERR_OK)
        err = uc_mem_map(uc, SMALL_ADDRESS, SMALL_SIZE, UC_PROT_ALL);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_mem_map: %s\n", uc_strerror(err));
        uc_close(uc);
        return 1;
    }

    uc_close(uc);
    return 0;
}
