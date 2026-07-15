#pragma once
//
// calc_engine.h — expression compiler and evaluator for the Almanac's graphing
// calculator. Pure logic, no display, no Arduino: it compiles unchanged on the
// host, where calc_test.cpp verifies it against Python's math library the same
// way sky_math.h was verified against astropy.
//
// Design constraints, in order:
//   1. The ESP32-C3 has NO floating-point hardware; every operation is a
//      soft-float library call. float is roughly twice as fast as double in
//      software and a plot only needs pixel precision, so the engine is float
//      throughout.
//   2. No heap. An expression compiles once into a fixed bytecode array and is
//      then evaluated hundreds of times per redraw (once per pixel column).
//      All buffers below are fixed-size members; a CalcExpr can live in an
//      Activity object without fragmenting anything.
//   3. -fno-exceptions. Errors are reported by return value plus a static
//      message string and an input position for the UI to point at.
//
// Grammar:
//   expr    := term (('+'|'-') term)*
//   term    := unary (('*'|'/') unary)*        (implicit '*' is inserted by
//   unary   := '-' unary | power                the tokenizer, see below)
//   power   := primary ('^' unary)?            ('^' is right-associative and
//   primary := number | 'x' | const             binds tighter than unary '-',
//            | func '(' expr ')' | '(' expr ')' so -x^2 = -(x^2), 2^-3 works)
//
// Tokenizer notes:
//   - Identifiers are matched longest-known-name-first, so "xsin(x)" lexes as
//     x, sin, ( — there is no whitespace requirement.
//   - Implicit multiplication is inserted wherever a value ends and another
//     begins: "2x", "2(x+1)", "x sin(x)", "(x+1)(x-1)" all work.
//   - A function name must be followed by '(' — "sin x" is a compile error,
//     because "sinx^2" would otherwise be ambiguous to a human reader.
//   - Numbers are plain decimal ("3", "0.5", ".5"). There is no exponent
//     literal: 'e' is Euler's number, so write 3*10^-4, not 3e-4.
//
// Angle mode is an eval-time argument, not baked into the bytecode, so a
// radians/degrees toggle rescales the same compiled expression instantly.
//
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

class CalcExpr {
 public:
  static constexpr int MAX_CODE = 64;   // bytecode ops
  static constexpr int MAX_STACK = 24;  // eval + compile operator stack depth

  // One byte-code op. NUM pushes `val`; VARX pushes x; the rest pop 1 or 2.
  enum Op : uint8_t {
    NUM, VARX,
    ADD, SUB, MUL, DIV, POW, NEG,
    // one-argument functions; keep FN_FIRST/FN_LAST in sync with kFuncs[]
    FN_SIN, FN_COS, FN_TAN, FN_ASIN, FN_ACOS, FN_ATAN,
    FN_SINH, FN_COSH, FN_TANH,
    FN_LN, FN_LOG, FN_LOG2, FN_SQRT, FN_CBRT, FN_ABS, FN_EXP,
    FN_FLOOR, FN_CEIL, FN_ROUND,
  };
  static constexpr uint8_t FN_FIRST = FN_SIN;
  static constexpr uint8_t FN_LAST = FN_ROUND;

  bool ok() const { return codeLen_ > 0 && err_ == nullptr; }
  bool empty() const { return codeLen_ == 0 && err_ == nullptr; }
  bool usesX() const { return usesX_; }
  const char* error() const { return err_; }  // nullptr when ok
  int errorPos() const { return errPos_; }    // byte offset into the source

  void clear() {
    codeLen_ = 0;
    err_ = nullptr;
    errPos_ = 0;
    usesX_ = false;
  }

  // Compile `src`. Returns ok(). An empty/blank source clears the slot and
  // returns false with no error — that is how a slot is switched off.
  bool compile(const char* src);

  // Evaluate at x. degrees=true makes trig take/return degrees. Domain errors
  // (log of a negative, 0/0, ...) come back as NaN or infinity from soft-float
  // itself — the caller treats every non-finite result as "no point here".
  float eval(float x, bool degrees) const;

 private:
  struct Tok {          // one parsed token
    uint8_t kind;       // TK_*
    uint8_t op;         // for TK_OP/TK_FUNC: an Op
    float val;          // for TK_NUM
    uint16_t pos;       // source offset, for error messages
  };
  enum : uint8_t { TK_NUM, TK_X, TK_OP, TK_FUNC, TK_LPAR, TK_RPAR };

  static constexpr int MAX_TOKENS = 80;

  bool fail(const char* msg, int pos) {
    codeLen_ = 0;
    err_ = msg;
    errPos_ = pos;
    return false;
  }
  bool emit(uint8_t op, float v = 0.0f);

  int tokenize(const char* src, Tok* out);  // returns count, or -1 (err_ set)

  uint8_t code_[MAX_CODE];
  float vals_[MAX_CODE];
  uint8_t codeLen_ = 0;
  bool usesX_ = false;
  const char* err_ = nullptr;
  int errPos_ = 0;
};

// ---------------------------------------------------------------------------
// implementation (header-only, like sky_math.h)
// ---------------------------------------------------------------------------

namespace calc_detail {

struct Name {
  const char* s;
  uint8_t len;
  uint8_t op;    // Op for functions, 0xFF for constants and x
  float val;     // constants only
};

// Longest names FIRST: the tokenizer takes the first entry that matches, so
// "floor" must be tried before "log" would ever get a chance to mis-match, and
// "asin" before "sin". (Ordering by length is sufficient; ties are disjoint.)
inline const Name* nameTable(int& n) {
  static const Name k[] = {
      {"floor", 5, CalcExpr::FN_FLOOR, 0},
      {"round", 5, CalcExpr::FN_ROUND, 0},
      {"asin", 4, CalcExpr::FN_ASIN, 0},
      {"acos", 4, CalcExpr::FN_ACOS, 0},
      {"atan", 4, CalcExpr::FN_ATAN, 0},
      {"sinh", 4, CalcExpr::FN_SINH, 0},
      {"cosh", 4, CalcExpr::FN_COSH, 0},
      {"tanh", 4, CalcExpr::FN_TANH, 0},
      {"sqrt", 4, CalcExpr::FN_SQRT, 0},
      {"cbrt", 4, CalcExpr::FN_CBRT, 0},
      {"ceil", 4, CalcExpr::FN_CEIL, 0},
      {"log2", 4, CalcExpr::FN_LOG2, 0},
      {"sin", 3, CalcExpr::FN_SIN, 0},
      {"cos", 3, CalcExpr::FN_COS, 0},
      {"tan", 3, CalcExpr::FN_TAN, 0},
      {"abs", 3, CalcExpr::FN_ABS, 0},
      {"exp", 3, CalcExpr::FN_EXP, 0},
      {"log", 3, CalcExpr::FN_LOG, 0},  // log = log10, ln = natural
      {"ln", 2, CalcExpr::FN_LN, 0},
      {"pi", 2, 0xFF, 3.14159265358979323846f},
      {"e", 1, 0xFF, 2.71828182845904523536f},
      {"x", 1, 0xFE, 0},
  };
  n = (int)(sizeof(k) / sizeof(k[0]));
  return k;
}

inline int precedence(uint8_t op) {
  switch (op) {
    case CalcExpr::POW: return 4;
    case CalcExpr::NEG: return 3;
    case CalcExpr::MUL:
    case CalcExpr::DIV: return 2;
    default: return 1;  // ADD, SUB
  }
}
inline bool rightAssoc(uint8_t op) { return op == CalcExpr::POW || op == CalcExpr::NEG; }

}  // namespace calc_detail

inline bool CalcExpr::emit(uint8_t op, float v) {
  if (codeLen_ >= MAX_CODE) return fail("Expression too long", 0);
  code_[codeLen_] = op;
  vals_[codeLen_] = v;
  codeLen_++;
  return true;
}

inline int CalcExpr::tokenize(const char* src, Tok* out) {
  int n = 0, tabN = 0;
  const calc_detail::Name* tab = calc_detail::nameTable(tabN);
  int i = 0;

  auto push = [&](uint8_t kind, uint8_t op, float val, int pos) -> bool {
    if (n >= MAX_TOKENS) { fail("Expression too long", pos); return false; }
    out[n].kind = kind;
    out[n].op = op;
    out[n].val = val;
    out[n].pos = (uint16_t)pos;
    n++;
    return true;
  };
  // A value just ended if the previous token can end an operand; a value is
  // about to start — that adjacency is an implicit multiplication.
  auto valueEnded = [&]() -> bool {
    if (n == 0) return false;
    const uint8_t k = out[n - 1].kind;
    return k == TK_NUM || k == TK_X || k == TK_RPAR;
  };

  while (src[i]) {
    const char c = src[i];
    if (c == ' ' || c == '\t') { i++; continue; }

    if ((c >= '0' && c <= '9') || c == '.') {
      // "(x+1)2" is implicit multiplication, but "2 3" and "x2" are almost
      // certainly typos (x2 for x^2), so only ')' earns the inserted '*'.
      // Without it, two adjacent operands fail as "Incomplete expression".
      if (n > 0 && out[n - 1].kind == TK_RPAR && !push(TK_OP, MUL, 0, i)) return -1;
      char* endp = nullptr;
      const float v = strtof(src + i, &endp);
      if (endp == src + i) { fail("Bad number", i); return -1; }
      if (!push(TK_NUM, 0, v, i)) return -1;
      i = (int)(endp - src);
      continue;
    }

    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
      int m = -1;
      for (int t = 0; t < tabN; t++) {
        bool match = true;
        for (int k = 0; k < tab[t].len; k++) {
          const char lc = src[i + k] >= 'A' && src[i + k] <= 'Z' ? src[i + k] + 32 : src[i + k];
          if (lc != tab[t].s[k]) { match = false; break; }
        }
        if (match) { m = t; break; }
      }
      if (m < 0) { fail("Unknown name", i); return -1; }
      if (valueEnded() && !push(TK_OP, MUL, 0, i)) return -1;  // "2x", "x sin(x)"
      const auto& e = tab[m];
      if (e.op == 0xFE) {
        if (!push(TK_X, 0, 0, i)) return -1;
      } else if (e.op == 0xFF) {
        if (!push(TK_NUM, 0, e.val, i)) return -1;
      } else {
        if (!push(TK_FUNC, e.op, 0, i)) return -1;
      }
      i += e.len;
      continue;
    }

    switch (c) {
      case '(':
        if (valueEnded() && !push(TK_OP, MUL, 0, i)) return -1;  // "2(x+1)"
        if (!push(TK_LPAR, 0, 0, i)) return -1;
        break;
      case ')': if (!push(TK_RPAR, 0, 0, i)) return -1; break;
      case '+': if (!push(TK_OP, ADD, 0, i)) return -1; break;
      case '*': if (!push(TK_OP, MUL, 0, i)) return -1; break;
      case '/': if (!push(TK_OP, DIV, 0, i)) return -1; break;
      case '^': if (!push(TK_OP, POW, 0, i)) return -1; break;
      case '-': {
        // Unary when nothing that can end an operand precedes it.
        const bool unary = !valueEnded();
        if (!push(TK_OP, unary ? NEG : SUB, 0, i)) return -1;
        break;
      }
      default: fail("Unexpected character", i); return -1;
    }
    i++;
  }
  return n;
}

inline bool CalcExpr::compile(const char* src) {
  clear();
  if (!src) return false;

  Tok toks[MAX_TOKENS];
  const int nTok = tokenize(src, toks);
  if (nTok < 0) return false;
  if (nTok == 0) return false;  // blank: slot off, not an error

  // Shunting-yard. opStack holds Ops plus two markers.
  constexpr uint8_t MK_LPAR = 0xFE, MK_FUNC_BASE = 0x80;
  uint8_t opStack[MAX_STACK];
  uint16_t opPos[MAX_STACK];
  int sp = 0;
  int operands = 0;  // running operand count; each binary op consumes one net

  auto popToOutput = [&]() -> bool {
    const uint8_t op = opStack[--sp];
    if (op >= MK_FUNC_BASE && op != MK_LPAR) {
      if (operands < 1) return fail("Function needs a value", opPos[sp]);
      return emit(op - MK_FUNC_BASE);
    }
    if (op == NEG) {
      if (operands < 1) return fail("Nothing to negate", opPos[sp]);
      return emit(NEG);
    }
    if (operands < 2) return fail("Operator needs two values", opPos[sp]);
    operands--;
    return emit(op);
  };
  auto pushOp = [&](uint8_t op, uint16_t pos) -> bool {
    if (sp >= MAX_STACK) return fail("Expression too deep", pos);
    opStack[sp] = op;
    opPos[sp] = pos;
    sp++;
    return true;
  };

  for (int t = 0; t < nTok; t++) {
    const Tok& tk = toks[t];
    switch (tk.kind) {
      case TK_NUM:
        if (!emit(NUM, tk.val)) return false;
        operands++;
        break;
      case TK_X:
        if (!emit(VARX)) return false;
        usesX_ = true;
        operands++;
        break;
      case TK_FUNC:
        if (t + 1 >= nTok || toks[t + 1].kind != TK_LPAR)
          return fail("Function needs ( )", tk.pos);
        if (!pushOp(MK_FUNC_BASE + tk.op, tk.pos)) return false;
        break;
      case TK_LPAR:
        if (!pushOp(MK_LPAR, tk.pos)) return false;
        break;
      case TK_RPAR: {
        while (sp > 0 && opStack[sp - 1] != MK_LPAR)
          if (!popToOutput()) return false;
        if (sp == 0) return fail("Unmatched )", tk.pos);
        sp--;  // discard the '('
        // A function marker sits directly beneath its '('.
        if (sp > 0 && opStack[sp - 1] >= MK_FUNC_BASE && opStack[sp - 1] != MK_LPAR)
          if (!popToOutput()) return false;
        break;
      }
      case TK_OP: {
        // A prefix operator applies to the operand that FOLLOWS it; popping
        // anything now would consume the operand to its LEFT ("2^-3" would
        // try to pop POW with only one value). Push it and move on.
        if (tk.op == NEG) {
          if (!pushOp(NEG, tk.pos)) return false;
          break;
        }
        const int p = calc_detail::precedence(tk.op);
        while (sp > 0 && opStack[sp - 1] != MK_LPAR && opStack[sp - 1] < MK_FUNC_BASE) {
          const int q = calc_detail::precedence(opStack[sp - 1]);
          if (q > p || (q == p && !calc_detail::rightAssoc(tk.op))) {
            if (!popToOutput()) return false;
          } else {
            break;
          }
        }
        if (!pushOp(tk.op, tk.pos)) return false;
        break;
      }
    }
  }
  while (sp > 0) {
    if (opStack[sp - 1] == MK_LPAR) return fail("Unmatched (", opPos[sp - 1]);
    if (!popToOutput()) return false;
  }
  if (operands != 1) return fail("Incomplete expression", nTok ? toks[nTok - 1].pos : 0);
  return true;
}

inline float CalcExpr::eval(float x, bool degrees) const {
  constexpr float D2R = 3.14159265358979323846f / 180.0f;
  constexpr float R2D = 180.0f / 3.14159265358979323846f;
  float st[MAX_STACK];
  int sp = 0;

  for (int i = 0; i < codeLen_; i++) {
    switch (code_[i]) {
      case NUM: st[sp++] = vals_[i]; break;
      case VARX: st[sp++] = x; break;
      case ADD: sp--; st[sp - 1] += st[sp]; break;
      case SUB: sp--; st[sp - 1] -= st[sp]; break;
      case MUL: sp--; st[sp - 1] *= st[sp]; break;
      case DIV: sp--; st[sp - 1] /= st[sp]; break;
      case POW: sp--; st[sp - 1] = powf(st[sp - 1], st[sp]); break;
      case NEG: st[sp - 1] = -st[sp - 1]; break;
      case FN_SIN: st[sp - 1] = sinf(degrees ? st[sp - 1] * D2R : st[sp - 1]); break;
      case FN_COS: st[sp - 1] = cosf(degrees ? st[sp - 1] * D2R : st[sp - 1]); break;
      case FN_TAN: st[sp - 1] = tanf(degrees ? st[sp - 1] * D2R : st[sp - 1]); break;
      case FN_ASIN: st[sp - 1] = asinf(st[sp - 1]); if (degrees) st[sp - 1] *= R2D; break;
      case FN_ACOS: st[sp - 1] = acosf(st[sp - 1]); if (degrees) st[sp - 1] *= R2D; break;
      case FN_ATAN: st[sp - 1] = atanf(st[sp - 1]); if (degrees) st[sp - 1] *= R2D; break;
      case FN_SINH: st[sp - 1] = sinhf(st[sp - 1]); break;
      case FN_COSH: st[sp - 1] = coshf(st[sp - 1]); break;
      case FN_TANH: st[sp - 1] = tanhf(st[sp - 1]); break;
      case FN_LN: st[sp - 1] = logf(st[sp - 1]); break;
      case FN_LOG: st[sp - 1] = log10f(st[sp - 1]); break;
      case FN_LOG2: st[sp - 1] = log2f(st[sp - 1]); break;
      case FN_SQRT: st[sp - 1] = sqrtf(st[sp - 1]); break;
      case FN_CBRT: st[sp - 1] = cbrtf(st[sp - 1]); break;
      case FN_ABS: st[sp - 1] = fabsf(st[sp - 1]); break;
      case FN_EXP: st[sp - 1] = expf(st[sp - 1]); break;
      case FN_FLOOR: st[sp - 1] = floorf(st[sp - 1]); break;
      case FN_CEIL: st[sp - 1] = ceilf(st[sp - 1]); break;
      case FN_ROUND: st[sp - 1] = roundf(st[sp - 1]); break;
      default: return NAN;
    }
  }
  return sp == 1 ? st[0] : NAN;
}
