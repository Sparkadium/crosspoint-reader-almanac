// calc_test.cpp -- host-side verification of calc_engine.h against Python.
//
//   python3 calc_cases.py                 (writes cases.txt)
//   g++ -std=c++17 -fno-exceptions -Wall -Wextra -O2 calc_test.cpp -o calc_test
//   (run from tools/almanac/ -- the include path up to calc_engine.h is relative)
//   ./calc_test
//
// -fno-exceptions matches the firmware build. The engine is float; Python is
// double, so agreement is checked to a relative 2e-5 (float has ~7 digits and
// some expressions chain several operations).

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "../../src/activities/almanac/calc_engine.h"

static int failures = 0;
static int checks = 0;

static bool close(float got, double want) {
  if (std::isnan(want)) return std::isnan(got);
  if (std::isinf(want)) return std::isinf(got) && ((got > 0) == (want > 0));
  if (std::isnan(got) || std::isinf(got)) return false;
  const double a = std::fabs((double)got - want);
  return a <= 2e-5 * std::fmax(1.0, std::fabs(want));
}

static void expectError(const char* src) {
  checks++;
  CalcExpr e;
  if (e.compile(src)) {
    printf("FAIL  \"%s\" compiled but should be a parse error\n", src);
    failures++;
  }
}

static void expectDeg(const char* src, float x, double want) {
  checks++;
  CalcExpr e;
  if (!e.compile(src)) {
    printf("FAIL  deg \"%s\": %s\n", src, e.error());
    failures++;
    return;
  }
  const float got = e.eval(x, /*degrees=*/true);
  if (!close(got, want)) {
    printf("FAIL  deg \"%s\" x=%g: got %.9g want %.9g\n", src, x, got, want);
    failures++;
  }
}

int main() {
  FILE* f = fopen("cases.txt", "r");
  if (!f) {
    printf("cases.txt missing -- run calc_cases.py first\n");
    return 2;
  }

  char line[512];
  while (fgets(line, sizeof(line), f)) {
    char* t1 = strchr(line, '\t');
    if (!t1) continue;
    char* t2 = strchr(t1 + 1, '\t');
    if (!t2) continue;
    *t1 = *t2 = 0;
    const char* expr = line;
    const float x = strtof(t1 + 1, nullptr);
    const char* ws = t2 + 1;
    double want;
    if (!strncmp(ws, "nan", 3)) want = NAN;
    else if (!strncmp(ws, "inf", 3)) want = INFINITY;
    else if (!strncmp(ws, "-inf", 4)) want = -INFINITY;
    else want = strtod(ws, nullptr);

    checks++;
    CalcExpr e;
    if (!e.compile(expr)) {
      printf("FAIL  \"%s\": parse error \"%s\" at %d\n", expr, e.error(), e.errorPos());
      failures++;
      continue;
    }
    const float got = e.eval(x, /*degrees=*/false);
    if (!close(got, want)) {
      printf("FAIL  \"%s\" x=%g: got %.9g want %.9g\n", expr, x, got, want);
      failures++;
    }
  }
  fclose(f);

  // ---- degrees mode (hand-checked values, no Python needed) --------------
  expectDeg("sin(90)", 0, 1.0);
  expectDeg("cos(x)", 60, 0.5);
  expectDeg("tan(45)", 0, 1.0);
  expectDeg("asin(1)", 0, 90.0);
  expectDeg("acos(0.5)", 0, 60.0);
  expectDeg("atan(1)", 0, 45.0);
  expectDeg("sinh(1)", 0, 1.1752011936438014);  // hyperbolics ignore the mode
  expectDeg("ln(e)", 0, 1.0);                   // so does everything else

  // ---- things that must NOT compile ---------------------------------------
  expectError("2+");
  expectError("*3");
  expectError("(2+3");
  expectError("2+3)");
  expectError("sin");
  expectError("sin x");     // functions require parentheses
  expectError("sin()");
  expectError("2 3");       // numbers may not simply abut
  expectError("x2");        // almost certainly a typo for x^2
  expectError("foo(3)");
  expectError("x$2");
  expectError("2..5");
  expectError("()");

  // ---- empty is "slot off", not an error ----------------------------------
  {
    checks++;
    CalcExpr e;
    if (e.compile("") || e.error() != nullptr || !e.empty()) {
      printf("FAIL  empty source should clear silently\n");
      failures++;
    }
  }
  // ---- usesX ---------------------------------------------------------------
  {
    checks++;
    CalcExpr a, b;
    a.compile("2pi+1");
    b.compile("x+1");
    if (a.usesX() || !b.usesX()) {
      printf("FAIL  usesX misreported\n");
      failures++;
    }
  }

  printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
