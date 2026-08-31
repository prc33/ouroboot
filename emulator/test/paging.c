#include "../rv64.c"
#define TABLE(n) (RAM_BASE + (n) * 0x1000)
#define VA 0x40000000ULL
static u64 *word(u64 pa) { return (u64 *)(ram + (u32)(pa - RAM_BASE)); }
static void map(u64 flags) {
    *word(TABLE(1) + 8) = (TABLE(2) >> 12) << 10 | 1;
    *word(TABLE(2)) = (TABLE(3) >> 12) << 10 | 1;
    *word(TABLE(3)) = (TABLE(4) >> 12) << 10 | flags;
    csr[SATP] = 8ULL << 60 | TABLE(1) >> 12;
    privilege = 0; csr[SSTATUS] = 0; flush_tlb();
}
static int separated(u64 flags, u32 allowed, u32 denied) {
    u64 pa;
    map(flags);
    return translate(VA, allowed, &pa) && !translate(VA, denied, &pa);
}
int main(void) {
    u64 pa;
    if (!separated(1 | 2, 1, 0) || !separated(1 | 8, 0, 1)) return 1;
    map(1 | 8 | 16); csr[SSTATUS] = STATUS_SUM;
    if (translate(VA, 0, &pa)) return 2;
    map(1 | 2 | 16); csr[SSTATUS] = STATUS_SUM;
    if (!translate(VA, 1, &pa)) return 3;
    map(1 | 2 | 4);
    if (!translate(VA, 2, &pa) || (*word(TABLE(3)) & 0xc0) != 0xc0) return 4;
    *word(TABLE(1) + 8) = (TABLE(4) >> 12) << 10 | 3; flush_tlb();
    return translate(VA, 1, &pa) ? 5 : 0;
}
