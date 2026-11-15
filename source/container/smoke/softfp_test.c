/* softfp_test.c: does code built by Chromium's clang + our sysroot call the TV's soft-float-ABI libraries
 * correctly? Every line mixes doubles/floats across a library or helper boundary. The same file built for the host
 * gives the expected output; on the TV the output must be identical. A wrong float ABI shows up as garbage
 * numbers or a crash, never as a subtle difference. */
#include <complex.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef double (*dfn)(double);
struct pt { float x, y; };

__attribute__((noinline)) static struct pt mid(struct pt a, struct pt b) {
	struct pt r = { (a.x + b.x) / 2, (a.y + b.y) / 2 };
	return r;
}

__attribute__((noinline)) static double vsum(int n, ...) {
	va_list ap;
	double s = 0;
	va_start(ap, n);
	for (int i = 0; i < n; i++) s += va_arg(ap, double);
	va_end(ap);
	return s;
}

int main(int argc, char **argv) {
	volatile double a = 2.5, b = 3.1;          /* volatile: no constant folding */
	volatile float f = 1.75f;
	volatile int k = 7;
	dfn fns[] = { sin, cos, sqrt, log, exp };
	const char *names[] = { "sin", "cos", "sqrt", "log", "exp" };
	printf("pow %.10g\n", pow(a, b));
	printf("powf %.6g\n", (double)powf(f, 2.5f));
	printf("hypot %.10g\n", hypot(a, b));
	printf("fmod %.10g\n", fmod(b * 10, a));
	for (int i = 0; i < 5; i++) printf("%s %.10g\n", names[i], fns[i](a));
	printf("strtod %.10g\n", strtod("3.25e2", NULL));
	printf("strtof %.6g\n", (double)strtof("-0.125", NULL));
	printf("atof %.10g\n", atof("12.5"));
	printf("powi %.10g\n", __builtin_powi(a, k));          /* compiler helper (__powidf2) */
	double complex z = (a + b * I) * (b - a * I);           /* compiler helper (__muldc3) */
	printf("cmul %.10g %.10g\n", creal(z), cimag(z));
	int64_t big = (int64_t)(a * 1e15);                      /* __aeabi_d2lz */
	printf("d2l %lld\n", (long long)big);
	printf("l2d %.10g\n", (double)(big + 3));
	struct pt m = mid((struct pt){ 1.0f, 2.0f }, (struct pt){ 4.0f, 8.5f });
	printf("mid %.6g %.6g\n", (double)m.x, (double)m.y);
	printf("vsum %.10g\n", vsum(4, 1.5, a, b, -0.25));
	char buf[64];
	snprintf(buf, sizeof buf, "%e|%f|%g", a, b, (double)f);
	printf("snprintf %s\n", buf);
	printf("sscanf ");
	double x = 0; float y = 0;
	if (sscanf("6.5 0.75", "%lf %f", &x, &y) == 2) printf("%.10g %.6g\n", x, (double)y);
	printf("done %d\n", argc);
	(void)argv;
	return 0;
}
