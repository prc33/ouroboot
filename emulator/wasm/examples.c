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
long long extend_signed(int x) { return x; }
unsigned long long extend_unsigned(unsigned int x) { return x; }
int truncate64(long long x) { return x; }
double i64_to_double(long long x) { return x; }
long long double_to_i64(double x) { return x; }
