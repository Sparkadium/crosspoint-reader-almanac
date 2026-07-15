#!/usr/bin/env python3
# calc_cases.py -- generates cases.txt for calc_test.cpp.
#
# Each case is a device-syntax expression, an x value, and the value Python's
# math library computes for an INDEPENDENTLY WRITTEN translation of the same
# expression. calc_engine.h is correct exactly insofar as it agrees with this
# file; nothing here is derived from the C++ implementation.
#
# Output: cases.txt, lines of  device_expr <TAB> x <TAB> expected
#         (expected = "nan" / "inf" / "-inf" where Python raises or overflows)

import math

# IEEE-754 semantics, which is what soft-float on the device does and what the
# plot layer consumes ("every non-finite value is a gap"). Python's exceptions
# are a language choice, not the spec.
def fdiv(a, b):
    if b != 0: return a / b
    return math.nan if a == 0 else math.copysign(math.inf, a) * math.copysign(1.0, b)

def flog(fn, v):
    if v > 0: return fn(v)
    return -math.inf if v == 0 else math.nan

# (device syntax, python lambda) -- the lambda is the authority.
CASES = [
    ("2+3*4",             lambda x: 2 + 3 * 4),
    ("(2+3)*4",           lambda x: (2 + 3) * 4),
    ("2^3^2",             lambda x: 2 ** 3 ** 2),            # right assoc: 512
    ("-3^2",              lambda x: -(3 ** 2)),               # -9, not 9
    ("2^-3",              lambda x: 2 ** -3),
    ("--4",               lambda x: 4),
    ("7/2",               lambda x: 7 / 2),
    ("1/x",               lambda x: fdiv(1, x)),
    ("x^2-2x+1",          lambda x: x**2 - 2*x + 1),          # implicit mult
    ("2(x+1)",            lambda x: 2 * (x + 1)),
    ("(x+1)(x-1)",        lambda x: (x + 1) * (x - 1)),
    ("x sin(x)",          lambda x: x * math.sin(x)),
    ("xsin(x)",           lambda x: x * math.sin(x)),         # greedy lexing
    ("3x^2",              lambda x: 3 * x**2),                # 3*(x^2)
    ("2pi",               lambda x: 2 * math.pi),
    ("e^x",               lambda x: math.e ** x),
    ("sin(x)^2+cos(x)^2", lambda x: math.sin(x)**2 + math.cos(x)**2),
    ("tan(x)",            lambda x: math.tan(x)),
    ("asin(0.5)",         lambda x: math.asin(0.5)),
    ("acos(x/10)",        lambda x: math.acos(x / 10)),
    ("atan(x)",           lambda x: math.atan(x)),
    ("sinh(x/5)",         lambda x: math.sinh(x / 5)),
    ("cosh(x/5)",         lambda x: math.cosh(x / 5)),
    ("tanh(x)",           lambda x: math.tanh(x)),
    ("ln(x)",             lambda x: flog(math.log, x)),
    ("log(x)",            lambda x: flog(math.log10, x)),
    ("log2(x)",           lambda x: flog(math.log2, x)),
    ("sqrt(x)",           lambda x: math.sqrt(x)),
    ("cbrt(x)",           lambda x: math.copysign(abs(x) ** (1/3), x)),
    ("abs(x-2)",          lambda x: abs(x - 2)),
    ("exp(-x^2)",         lambda x: math.exp(-(x**2))),
    ("floor(x/2)",        lambda x: math.floor(x / 2)),
    ("ceil(x/2)",         lambda x: math.ceil(x / 2)),
    ("round(x/3)",        lambda x: round(x / 3)),            # see note below
    ("sqrt(1-x^2/100)",   lambda x: math.sqrt(1 - x**2/100)),
    ("1/(x-1)+1/(x+1)",   lambda x: fdiv(1, x-1) + fdiv(1, x+1)),
    ("(x+1)2",            lambda x: (x + 1) * 2),
    (".5x+.25",           lambda x: 0.5 * x + 0.25),
    ("SIN(X)+Pi",         lambda x: math.sin(x) + math.pi),   # case-insensitive
    ("x/0",               lambda x: math.copysign(math.inf, x) if x != 0 else math.nan),
    ("sqrt(-4)",          lambda x: math.nan),
    ("ln(0)",             lambda x: -math.inf),
    ("ln(-1)",            lambda x: math.nan),
]

XS = [-7.5, -2.0, -1.0, -0.5, 0.0, 0.5, 1.0, 2.0, 3.3, 7.5]

def main():
    lines = []
    for expr, fn in CASES:
        for x in XS:
            try:
                v = fn(x)
            except (ValueError, ZeroDivisionError):
                v = math.nan
            except OverflowError:
                v = math.inf
            # Python's round() is banker's rounding; C roundf() rounds half
            # away from zero. Skip the half-integer inputs so the case tests
            # round(), not a tie-breaking convention no calculator user hits
            # at random x. (2.5/3 etc. are not halves; x/3 = k+0.5 only when
            # x = 3k+1.5, i.e. x=-7.5 and 7.5 here.)
            if expr.startswith("round") and abs((x / 3) % 1 - 0.5) < 1e-9:
                continue
            if isinstance(v, complex):
                v = math.nan
            lines.append(f"{expr}\t{x!r}\t{v!r}")
    with open("cases.txt", "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"wrote {len(lines)} cases")

if __name__ == "__main__":
    main()
