int add(int a, int b)
{
    return a + b;
}

int factorial(int n)
{
    int result = 1;
    while (n > 1) {
        result *= n;
        n--;
    }
    return result;
}

static int square(int x)
{
    return x * x;
}

int sum_squares(int a, int b)
{
    return square(a) + square(b);
}

double polynomial(double x)
{
    return x * x + 2.0 * x + 1.0;
}

unsigned long long mix64(unsigned long long x)
{
    x ^= x >> 29;
    x *= 0x9e3779b97f4a7c15ULL;
    return x ^ (x << 17);
}

long long divide64(long long a, long long b)
{
    return a / b;
}

unsigned long long identity64(unsigned long long x) { return x; }
unsigned long long add64(unsigned long long a, unsigned long long b) { return a + b; }
unsigned long long shift64(unsigned long long x, int n) { return x >> n; }
unsigned long long multiply64(unsigned long long a, unsigned long long b) { return a * b; }
unsigned long long divide_u64(unsigned long long a, unsigned long long b) { return a / b; }
long long remainder64(long long a, long long b) { return a % b; }
unsigned long long shift_left64(unsigned long long x, int n) { return x << n; }
int less64(long long a, long long b) { return a < b; }
int less_u64(unsigned long long a, unsigned long long b) { return a < b; }
long long extend_signed(int x) { return x; }
unsigned long long extend_unsigned(unsigned int x) { return x; }
int truncate64(long long x) { return x; }
double i64_to_double(long long x) { return x; }
long long double_to_i64(double x) { return x; }
int copy_out(unsigned long long x, unsigned long long *out) { *out = x; return 1; }
unsigned long long test_copy_out(unsigned long long x) { unsigned long long out = 0; copy_out(x, &out); return out; }
static unsigned long long global64;
static unsigned long long echo64(unsigned long long x) { return x; }
unsigned long long test_global64(unsigned long long x) { global64 = x; return echo64(global64); }
static unsigned long long first4(unsigned long long a, unsigned int b, unsigned int c, unsigned long long *d) { *d = a; return a + b + c; }
unsigned long long test_four_args(unsigned long long a) { unsigned long long d; return first4(a, 4, 0, &d) + d; }
static unsigned char byte_array[4096];
unsigned int byte_array_ptr(void) { return (unsigned int)byte_array; }
unsigned int byte_array_read(unsigned int i) { return byte_array[i]; }
unsigned long long byte_array_read64(unsigned int i) { return *(unsigned long long *)(byte_array + i); }
