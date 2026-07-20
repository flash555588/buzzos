#include "libc.h"
#include "basm.h"

#define SRC_MAX 32768
#define TEXT_MAX 49152
#define DATA_MAX 12288
#define ELF_MAX 69632
#define MAX_SYMS 512
#define NAME_MAX 31
#define OPERAND_MAX 128
#define LINE_MAX 512

#define USER_BASE 0x02000000u
#define LOAD_OFF  0x1000u

enum section_id {
    SEC_TEXT = 0,
    SEC_DATA = 1,
    SEC_BSS = 2,
};

struct sym {
    int used;
    int is_equ;
    int section;
    uint32_t value;
    int defined_pass;
    char name[NAME_MAX + 1];
};

static char source[SRC_MAX + 1];
static uint8_t text_buf[TEXT_MAX];
static uint8_t data_buf[DATA_MAX];
static uint8_t elf_buf[ELF_MAX];
static struct sym syms[MAX_SYMS];

static int src_len;
static int text_len;
static int data_len;
static int bss_len;
static int final_text_len;
static int final_data_len;
static int cur_section;
static int pass_no;
static int line_no;
static int failed;
static char err_msg[96];
static char global_entry[NAME_MAX + 1] = "_start";

static int streq(const char *a, const char *b) {
    return strcmp(a, b) == 0;
}

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int is_name_start(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '.';
}

static int is_name_char(char c) {
    return is_name_start(c) || (c >= '0' && c <= '9');
}

static char lower_char(char c) {
    if (c >= 'A' && c <= 'Z')
        return (char)(c - 'A' + 'a');
    return c;
}

static void set_error(const char *msg) {
    if (failed)
        return;
    failed = 1;
    int i = 0;
    while (msg[i] && i < (int)sizeof(err_msg) - 1) {
        err_msg[i] = msg[i];
        i++;
    }
    err_msg[i] = 0;
}

static char *skip_ws(char *p) {
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

static void rtrim(char *s) {
    int n = (int)strlen(s);
    while (n > 0 && is_space(s[n - 1]))
        s[--n] = 0;
}

static void strip_comment(char *s) {
    int quote = 0;
    for (int i = 0; s[i]; i++) {
        if (s[i] == '"' && (i == 0 || s[i - 1] != '\\'))
            quote = !quote;
        if (!quote && s[i] == ';') {
            s[i] = 0;
            return;
        }
    }
}

static int parse_token(char **pp, char *out, int out_size) {
    char *p = skip_ws(*pp);
    int n = 0;
    if (!is_name_start(*p) && *p != '%')
        return 0;
    while ((is_name_char(*p) || *p == '%') && n < out_size - 1)
        out[n++] = lower_char(*p++);
    out[n] = 0;
    *pp = p;
    return n > 0;
}

static int parse_number(const char *s, uint32_t *out) {
    uint32_t v = 0;
    int i = 0;
    int base = 10;
    if (s[0] == '0' && lower_char(s[1]) == 'x') {
        base = 16;
        i = 2;
    }
    if (!s[i])
        return 0;
    for (; s[i]; i++) {
        char c = lower_char(s[i]);
        int d;
        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (c >= 'a' && c <= 'f')
            d = c - 'a' + 10;
        else
            return 0;
        if (d >= base)
            return 0;
        v = v * (uint32_t)base + (uint32_t)d;
    }
    *out = v;
    return 1;
}

static struct sym *find_sym(const char *name) {
    for (int i = 0; i < MAX_SYMS; i++) {
        if (syms[i].used && streq(syms[i].name, name))
            return &syms[i];
    }
    return 0;
}

static struct sym *get_sym_slot(const char *name) {
    struct sym *s = find_sym(name);
    if (s)
        return s;
    for (int i = 0; i < MAX_SYMS; i++) {
        if (!syms[i].used) {
            syms[i].used = 1;
            syms[i].is_equ = 0;
            syms[i].section = SEC_TEXT;
            syms[i].value = 0;
            int n = 0;
            while (name[n] && n < NAME_MAX) {
                syms[i].name[n] = name[n];
                n++;
            }
            syms[i].name[n] = 0;
            return &syms[i];
        }
    }
    set_error("too many symbols");
    return 0;
}

static void define_label(const char *name) {
    struct sym *s = get_sym_slot(name);
    if (!s)
        return;
    if (s->defined_pass == pass_no) {
        set_error("duplicate symbol");
        return;
    }
    s->defined_pass = pass_no;
    s->is_equ = 0;
    s->section = cur_section;
    s->value = (uint32_t)(cur_section == SEC_TEXT ? text_len :
                          cur_section == SEC_DATA ? data_len : bss_len);
}

static void define_equ(const char *name, uint32_t value) {
    struct sym *s = get_sym_slot(name);
    if (!s)
        return;
    if (s->defined_pass == pass_no) {
        set_error("duplicate symbol");
        return;
    }
    s->defined_pass = pass_no;
    s->is_equ = 1;
    s->section = SEC_TEXT;
    s->value = value;
}

static uint32_t section_offset(void) {
    return (uint32_t)(cur_section == SEC_TEXT ? text_len :
                      cur_section == SEC_DATA ? data_len : bss_len);
}

static uint32_t sym_runtime_value(const struct sym *s) {
    if (s->is_equ)
        return s->value;
    if (s->section == SEC_TEXT)
        return USER_BASE + s->value;
    if (s->section == SEC_DATA)
        return USER_BASE + (uint32_t)final_text_len + s->value;
    return USER_BASE + (uint32_t)final_text_len +
           (uint32_t)final_data_len + s->value;
}

static uint32_t current_runtime_addr(void);

struct expr_parser { const char *p; };

static void expr_ws(struct expr_parser *ep) {
    while (*ep->p == ' ' || *ep->p == '\t')
        ep->p++;
}

static uint32_t expr_or(struct expr_parser *ep);

static uint32_t expr_primary(struct expr_parser *ep) {
    expr_ws(ep);
    if (*ep->p == '(') {
        ep->p++;
        uint32_t value = expr_or(ep);
        expr_ws(ep);
        if (*ep->p != ')') {
            set_error("missing ')' in expression");
            return 0;
        }
        ep->p++;
        return value;
    }
    if (*ep->p == '$') {
        ep->p++;
        return current_runtime_addr();
    }
    if (*ep->p == '\'') {
        ep->p++;
        uint32_t value = (uint8_t)*ep->p++;
        if (value == '\\' && *ep->p) {
            char e = *ep->p++;
            value = e == 'n' ? '\n' : e == 'r' ? '\r' : e == 't' ? '\t' : (uint8_t)e;
        }
        if (*ep->p != '\'')
            set_error("unterminated character constant");
        else
            ep->p++;
        return value;
    }
    if ((*ep->p >= '0' && *ep->p <= '9')) {
        char number[32];
        int n = 0;
        while (((*ep->p >= '0' && *ep->p <= '9') ||
                (*ep->p >= 'a' && *ep->p <= 'f') ||
                (*ep->p >= 'A' && *ep->p <= 'F') ||
                *ep->p == 'x' || *ep->p == 'X') &&
               n + 1 < (int)sizeof(number))
            number[n++] = *ep->p++;
        number[n] = 0;
        uint32_t value = 0;
        if (!parse_number(number, &value))
            set_error("bad number");
        return value;
    }
    if (is_name_start(*ep->p)) {
        char name[NAME_MAX + 1];
        int n = 0;
        while (is_name_char(*ep->p) && n < NAME_MAX)
            name[n++] = lower_char(*ep->p++);
        name[n] = 0;
        struct sym *s = find_sym(name);
        if (!s) {
            if (pass_no == 1)
                return 0;
            set_error("undefined symbol");
            return 0;
        }
        return sym_runtime_value(s);
    }
    set_error("expected expression");
    return 0;
}

static uint32_t expr_unary(struct expr_parser *ep) {
    expr_ws(ep);
    if (*ep->p == '+') { ep->p++; return expr_unary(ep); }
    if (*ep->p == '-') { ep->p++; return 0u - expr_unary(ep); }
    if (*ep->p == '~') { ep->p++; return ~expr_unary(ep); }
    return expr_primary(ep);
}

static uint32_t expr_mul(struct expr_parser *ep) {
    uint32_t value = expr_unary(ep);
    for (;;) {
        expr_ws(ep);
        char op = *ep->p;
        if (op != '*' && op != '/' && op != '%') return value;
        ep->p++;
        uint32_t rhs = expr_unary(ep);
        if ((op == '/' || op == '%') && rhs == 0) {
            set_error("division by zero in expression");
            return 0;
        }
        value = op == '*' ? value * rhs : op == '/' ? value / rhs : value % rhs;
    }
}

static uint32_t expr_add(struct expr_parser *ep) {
    uint32_t value = expr_mul(ep);
    for (;;) {
        expr_ws(ep);
        char op = *ep->p;
        if (op != '+' && op != '-') return value;
        ep->p++;
        uint32_t rhs = expr_mul(ep);
        value = op == '+' ? value + rhs : value - rhs;
    }
}

static uint32_t expr_shift(struct expr_parser *ep) {
    uint32_t value = expr_add(ep);
    for (;;) {
        expr_ws(ep);
        if (ep->p[0] == '<' && ep->p[1] == '<') {
            ep->p += 2; value <<= (expr_add(ep) & 31u);
        } else if (ep->p[0] == '>' && ep->p[1] == '>') {
            ep->p += 2; value >>= (expr_add(ep) & 31u);
        } else return value;
    }
}

static uint32_t expr_and(struct expr_parser *ep) {
    uint32_t value = expr_shift(ep);
    for (;;) {
        expr_ws(ep);
        if (*ep->p != '&') return value;
        ep->p++; value &= expr_shift(ep);
    }
}

static uint32_t expr_xor(struct expr_parser *ep) {
    uint32_t value = expr_and(ep);
    for (;;) {
        expr_ws(ep);
        if (*ep->p != '^') return value;
        ep->p++; value ^= expr_and(ep);
    }
}

static uint32_t expr_or(struct expr_parser *ep) {
    uint32_t value = expr_xor(ep);
    for (;;) {
        expr_ws(ep);
        if (*ep->p != '|') return value;
        ep->p++; value |= expr_xor(ep);
    }
}

static int eval_expr(const char *expr, uint32_t *out) {
    struct expr_parser ep = {expr};
    *out = expr_or(&ep);
    expr_ws(&ep);
    if (*ep.p && !failed) {
        set_error("trailing tokens in expression");
        return 0;
    }
    return failed ? 0 : 1;
}

static void emit_byte(uint8_t b) {
    if (cur_section == SEC_TEXT) {
        if (text_len >= TEXT_MAX) {
            set_error("text too large");
            return;
        }
        if (pass_no == 2)
            text_buf[text_len] = b;
        text_len++;
    } else if (cur_section == SEC_DATA) {
        if (data_len >= DATA_MAX) {
            set_error("data too large");
            return;
        }
        if (pass_no == 2)
            data_buf[data_len] = b;
        data_len++;
    } else {
        (void)b;
        bss_len++;
        if (bss_len > DATA_MAX) {
            set_error("bss too large");
            return;
        }
    }
}

static void emit_u32(uint32_t v) {
    emit_byte((uint8_t)(v & 0xFF));
    emit_byte((uint8_t)((v >> 8) & 0xFF));
    emit_byte((uint8_t)((v >> 16) & 0xFF));
    emit_byte((uint8_t)((v >> 24) & 0xFF));
}

static void emit_u16(uint32_t v) {
    emit_byte((uint8_t)(v & 0xFF));
    emit_byte((uint8_t)((v >> 8) & 0xFF));
}

static uint32_t current_runtime_addr(void) {
    if (cur_section == SEC_TEXT)
        return USER_BASE + (uint32_t)text_len;
    if (cur_section == SEC_DATA)
        return USER_BASE + (uint32_t)final_text_len + (uint32_t)data_len;
    return USER_BASE + (uint32_t)final_text_len +
           (uint32_t)final_data_len + (uint32_t)bss_len;
}

static int split_operands(char *s, char ops[][OPERAND_MAX], int max_ops) {
    int count = 0;
    int quote = 0;
    int pos = 0;
    for (int i = 0;; i++) {
        char c = s[i];
        if (c == '"' && (i == 0 || s[i - 1] != '\\'))
            quote = !quote;
        if ((c == ',' && !quote) || c == 0) {
            if (count >= max_ops)
                return count;
            ops[count][pos] = 0;
            rtrim(ops[count]);
            char *p = skip_ws(ops[count]);
            if (p != ops[count]) {
                int j = 0;
                while (p[j]) {
                    ops[count][j] = p[j];
                    j++;
                }
                ops[count][j] = 0;
            }
            count++;
            pos = 0;
            if (c == 0)
                break;
            continue;
        }
        if (pos < OPERAND_MAX - 1)
            ops[count][pos++] = lower_char(c);
    }
    if (count == 1 && ops[0][0] == 0)
        return 0;
    return count;
}

enum operand_kind { OP_NONE, OP_REG, OP_IMM, OP_MEM };

struct operand {
    int kind;
    int width;
    int reg;
    int base;
    int index;
    int scale;
    int32_t disp;
    uint32_t imm;
};

static int reg_info(const char *s, int *width) {
    static const char *r32[] = {"eax","ecx","edx","ebx","esp","ebp","esi","edi"};
    static const char *r16[] = {"ax","cx","dx","bx","sp","bp","si","di"};
    static const char *r8[] = {"al","cl","dl","bl","ah","ch","dh","bh"};
    for (int i = 0; i < 8; i++) {
        if (streq(s, r32[i])) { *width = 32; return i; }
        if (streq(s, r16[i])) { *width = 16; return i; }
        if (streq(s, r8[i])) { *width = 8; return i; }
    }
    return -1;
}

static int parse_scale_reg(char *term, int *reg, int *scale) {
    char *star = 0;
    for (int i = 0; term[i]; i++)
        if (term[i] == '*') { star = term + i; break; }
    if (!star)
        return 0;
    *star = 0;
    char *a = skip_ws(term);
    char *b = skip_ws(star + 1);
    rtrim(a);
    rtrim(b);
    int width = 0;
    int r = reg_info(a, &width);
    uint32_t n = 0;
    if (r >= 0 && width == 32 && parse_number(b, &n)) {
        *reg = r; *scale = (int)n; return n == 1 || n == 2 || n == 4 || n == 8;
    }
    r = reg_info(b, &width);
    if (r >= 0 && width == 32 && parse_number(a, &n)) {
        *reg = r; *scale = (int)n; return n == 1 || n == 2 || n == 4 || n == 8;
    }
    return 0;
}

static int parse_memory(char *text, struct operand *op) {
    char *lb = 0;
    char *rb = 0;
    for (int i = 0; text[i]; i++) {
        if (text[i] == '[' && !lb) lb = text + i;
        if (text[i] == ']') rb = text + i;
    }
    if (!lb || !rb || rb < lb)
        return 0;
    *rb = 0;
    op->kind = OP_MEM;
    op->base = -1;
    op->index = -1;
    op->scale = 1;
    op->disp = 0;

    char *p = skip_ws(lb + 1);
    int sign = 1;
    while (*p) {
        p = skip_ws(p);
        if (*p == '+') { sign = 1; p++; continue; }
        if (*p == '-') { sign = -1; p++; continue; }
        char term[OPERAND_MAX];
        int n = 0;
        while (*p && *p != '+' && *p != '-' && n < (int)sizeof(term) - 1)
            term[n++] = *p++;
        term[n] = 0;
        rtrim(term);
        char *t = skip_ws(term);
        int width = 0;
        int r = reg_info(t, &width);
        if (r >= 0 && width == 32) {
            if (sign < 0) { set_error("register cannot be subtracted"); return 0; }
            if (op->base < 0)
                op->base = r;
            else if (op->index < 0 && r != 4) {
                op->index = r;
                op->scale = 1;
            } else {
                set_error("too many address registers");
                return 0;
            }
        } else {
            int sr = -1;
            int scale = 1;
            if (parse_scale_reg(t, &sr, &scale)) {
                if (sign < 0 || sr == 4 || op->index >= 0) {
                    set_error("bad scaled index");
                    return 0;
                }
                op->index = sr;
                op->scale = scale;
            } else {
                uint32_t value = 0;
                if (!eval_expr(t, &value))
                    return 0;
                op->disp += sign > 0 ? (int32_t)value : -(int32_t)value;
            }
        }
        sign = 1;
    }
    return 1;
}

static int parse_operand(char *text, struct operand *op) {
    memset(op, 0, sizeof(*op));
    op->kind = OP_NONE;
    op->width = 0;
    char *p = skip_ws(text);
    if (p[0] == 'b' && p[1] == 'y' && p[2] == 't' && p[3] == 'e' && is_space(p[4])) {
        op->width = 8; p = skip_ws(p + 4);
    } else if (p[0] == 'w' && p[1] == 'o' && p[2] == 'r' && p[3] == 'd' && is_space(p[4])) {
        op->width = 16; p = skip_ws(p + 4);
    } else if (p[0] == 'd' && p[1] == 'w' && p[2] == 'o' && p[3] == 'r' &&
               p[4] == 'd' && is_space(p[5])) {
        op->width = 32; p = skip_ws(p + 5);
    }
    if (p[0] == 'p' && p[1] == 't' && p[2] == 'r' && is_space(p[3]))
        p = skip_ws(p + 3);
    int width = 0;
    int reg = reg_info(p, &width);
    if (reg >= 0) {
        op->kind = OP_REG;
        op->reg = reg;
        op->width = width;
        return 1;
    }
    if (parse_memory(p, op)) {
        if (!op->width) op->width = 32;
        return 1;
    }
    if (!eval_expr(p, &op->imm))
        return 0;
    op->kind = OP_IMM;
    if (!op->width) op->width = 32;
    return 1;
}

static void emit_prefix(int width) {
    if (width == 16)
        emit_byte(0x66);
}

static int scale_bits(int scale) {
    if (scale == 2) return 1;
    if (scale == 4) return 2;
    if (scale == 8) return 3;
    return 0;
}

static void emit_modrm(int field, const struct operand *op) {
    if (op->kind == OP_REG) {
        emit_byte((uint8_t)(0xC0 | ((field & 7) << 3) | op->reg));
        return;
    }
    if (op->kind != OP_MEM) {
        set_error("expected register or memory");
        return;
    }
    if (op->base < 0 && op->index < 0) {
        emit_byte((uint8_t)(((field & 7) << 3) | 5));
        emit_u32((uint32_t)op->disp);
        return;
    }
    int need_sib = op->index >= 0 || op->base == 4 || op->base < 0;
    if (op->base < 0) {
        emit_byte((uint8_t)(((field & 7) << 3) | 4));
        emit_byte((uint8_t)((scale_bits(op->scale) << 6) |
                            ((op->index < 0 ? 4 : op->index) << 3) | 5));
        emit_u32((uint32_t)op->disp);
        return;
    }
    emit_byte((uint8_t)(0x80 | ((field & 7) << 3) | (need_sib ? 4 : op->base)));
    if (need_sib)
        emit_byte((uint8_t)((scale_bits(op->scale) << 6) |
                            ((op->index < 0 ? 4 : op->index) << 3) | op->base));
    emit_u32((uint32_t)op->disp);
}

static void emit_db(char *args) {
    char *p = args;
    while (*p) {
        p = skip_ws(p);
        if (*p == ',') {
            p++;
            continue;
        }
        if (!*p)
            break;
        if (*p == '"') {
            p++;
            while (*p && *p != '"') {
                char c = *p++;
                if (c == '\\' && *p) {
                    char e = *p++;
                    if (e == 'n') c = '\n';
                    else if (e == 'r') c = '\r';
                    else if (e == 't') c = '\t';
                    else c = e;
                }
                emit_byte((uint8_t)c);
            }
            if (*p == '"')
                p++;
        } else {
            char term[OPERAND_MAX];
            int n = 0;
            while (*p && *p != ',' && n < (int)sizeof(term) - 1)
                term[n++] = *p++;
            term[n] = 0;
            uint32_t v = 0;
            if (!eval_expr(term, &v))
                return;
            emit_byte((uint8_t)v);
        }
        p = skip_ws(p);
        if (*p == ',')
            p++;
    }
}

static int condition_code(const char *mn) {
    static const char *names[] = {
        "jo","jno","jb","jnae","jc","jnb","jae","jnc",
        "je","jz","jne","jnz","jbe","jna","ja","jnbe",
        "js","jns","jp","jpe","jnp","jpo","jl","jnge",
        "jge","jnl","jle","jng","jg","jnle"
    };
    static const uint8_t codes[] = {
        0,1,2,2,2,3,3,3,4,4,5,5,6,6,7,7,
        8,9,10,10,11,11,12,12,13,13,14,14,15,15
    };
    for (int i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++)
        if (streq(mn, names[i]))
            return codes[i];
    return -1;
}

static int operand_width(const struct operand *a, const struct operand *b) {
    if (a && a->kind == OP_REG) return a->width;
    if (b && b->kind == OP_REG) return b->width;
    if (a && a->kind == OP_MEM) return a->width;
    return b ? b->width : 32;
}

static void emit_mov(const struct operand *dst, const struct operand *src) {
    int width = operand_width(dst, src);
    if (dst->kind == OP_REG && src->kind == OP_IMM) {
        emit_prefix(width);
        emit_byte((uint8_t)((width == 8 ? 0xB0 : 0xB8) + dst->reg));
        if (width == 8) emit_byte((uint8_t)src->imm);
        else if (width == 16) emit_u16(src->imm);
        else emit_u32(src->imm);
        return;
    }
    if ((dst->kind == OP_REG) && (src->kind == OP_REG || src->kind == OP_MEM)) {
        if (dst->width != width) { set_error("mov width mismatch"); return; }
        emit_prefix(width);
        emit_byte((uint8_t)(width == 8 ? 0x8A : 0x8B));
        emit_modrm(dst->reg, src);
        return;
    }
    if (dst->kind == OP_MEM && src->kind == OP_REG) {
        if (src->width != width) { set_error("mov width mismatch"); return; }
        emit_prefix(width);
        emit_byte((uint8_t)(width == 8 ? 0x88 : 0x89));
        emit_modrm(src->reg, dst);
        return;
    }
    if (dst->kind == OP_MEM && src->kind == OP_IMM) {
        emit_prefix(width);
        emit_byte((uint8_t)(width == 8 ? 0xC6 : 0xC7));
        emit_modrm(0, dst);
        if (width == 8) emit_byte((uint8_t)src->imm);
        else if (width == 16) emit_u16(src->imm);
        else emit_u32(src->imm);
        return;
    }
    set_error("unsupported mov operands");
}

struct bin_encoding {
    const char *name;
    uint8_t rm_r;
    uint8_t r_rm;
    uint8_t group;
};

static int find_bin_encoding(const char *mn, struct bin_encoding *out) {
    static const struct bin_encoding table[] = {
        {"add",0x01,0x03,0}, {"or",0x09,0x0B,1},
        {"adc",0x11,0x13,2}, {"sbb",0x19,0x1B,3},
        {"and",0x21,0x23,4}, {"sub",0x29,0x2B,5},
        {"xor",0x31,0x33,6}, {"cmp",0x39,0x3B,7},
    };
    for (int i = 0; i < (int)(sizeof(table) / sizeof(table[0])); i++) {
        if (streq(mn, table[i].name)) {
            *out = table[i];
            return 1;
        }
    }
    return 0;
}

static void emit_binary(const struct bin_encoding *enc,
                        const struct operand *dst, const struct operand *src) {
    int width = operand_width(dst, src);
    emit_prefix(width);
    if ((dst->kind == OP_REG || dst->kind == OP_MEM) && src->kind == OP_IMM) {
        emit_byte((uint8_t)(width == 8 ? 0x80 : 0x81));
        emit_modrm(enc->group, dst);
        if (width == 8) emit_byte((uint8_t)src->imm);
        else if (width == 16) emit_u16(src->imm);
        else emit_u32(src->imm);
        return;
    }
    if (dst->kind == OP_REG && (src->kind == OP_REG || src->kind == OP_MEM)) {
        if (dst->width != width) { set_error("operand width mismatch"); return; }
        emit_byte((uint8_t)(enc->r_rm - (width == 8 ? 1 : 0)));
        emit_modrm(dst->reg, src);
        return;
    }
    if (dst->kind == OP_MEM && src->kind == OP_REG) {
        if (src->width != width) { set_error("operand width mismatch"); return; }
        emit_byte((uint8_t)(enc->rm_r - (width == 8 ? 1 : 0)));
        emit_modrm(src->reg, dst);
        return;
    }
    set_error("unsupported arithmetic operands");
}

static void emit_reserve(int unit, char *args) {
    uint32_t count = 0;
    if (!eval_expr(args, &count)) return;
    if (count > (uint32_t)DATA_MAX || count * (uint32_t)unit > (uint32_t)DATA_MAX) {
        set_error("reserve too large");
        return;
    }
    for (uint32_t i = 0; i < count * (uint32_t)unit; i++)
        emit_byte(0);
}

static void emit_words(char *args, int width) {
    char ops[16][OPERAND_MAX];
    int n = split_operands(args, ops, 16);
    for (int i = 0; i < n; i++) {
        uint32_t v = 0;
        if (!eval_expr(ops[i], &v)) return;
        if (width == 2) emit_u16(v); else emit_u32(v);
    }
}

static void emit_instruction(const char *mn, char *args) {
    char ops[3][OPERAND_MAX];
    int n = split_operands(args, ops, 3);
    struct operand a, b;

    if (streq(mn, "db")) {
        if (cur_section == SEC_BSS) set_error("initialized data is not allowed in bss");
        else emit_db(args);
        return;
    }
    if (streq(mn, "dw")) {
        if (cur_section == SEC_BSS) set_error("initialized data is not allowed in bss");
        else emit_words(args, 2);
        return;
    }
    if (streq(mn, "dd")) {
        if (cur_section == SEC_BSS) set_error("initialized data is not allowed in bss");
        else emit_words(args, 4);
        return;
    }
    if (streq(mn, "resb")) { emit_reserve(1, args); return; }
    if (streq(mn, "resw")) { emit_reserve(2, args); return; }
    if (streq(mn, "resd")) { emit_reserve(4, args); return; }
    if (streq(mn, "align")) {
        uint32_t boundary = 0;
        if (!eval_expr(args, &boundary) || !boundary ||
            (boundary & (boundary - 1u))) {
            set_error("align needs a power of two");
            return;
        }
        while (section_offset() & (boundary - 1u))
            emit_byte(0);
        return;
    }
    if (streq(mn, "times")) {
        char *p = skip_ws(args);
        char count_text[32];
        int k = 0;
        while (*p && !is_space(*p) && k < (int)sizeof(count_text) - 1)
            count_text[k++] = *p++;
        count_text[k] = 0;
        uint32_t count = 0;
        if (!eval_expr(count_text, &count) || count > 65536u) {
            set_error("bad times count");
            return;
        }
        p = skip_ws(p);
        char nested[16];
        if (!parse_token(&p, nested, sizeof(nested))) {
            set_error("times needs an instruction");
            return;
        }
        p = skip_ws(p);
        for (uint32_t i = 0; i < count && !failed; i++)
            emit_instruction(nested, p);
        return;
    }
    if (cur_section != SEC_TEXT) {
        set_error("instructions are only allowed in text");
        return;
    }

    if (streq(mn, "nop")) { emit_byte(0x90); return; }
    if (streq(mn, "leave")) { emit_byte(0xC9); return; }
    if (streq(mn, "pusha")) { emit_byte(0x60); return; }
    if (streq(mn, "popa")) { emit_byte(0x61); return; }
    if (streq(mn, "pushf")) { emit_byte(0x9C); return; }
    if (streq(mn, "popf")) { emit_byte(0x9D); return; }
    if (streq(mn, "cdq")) { emit_byte(0x99); return; }
    if (streq(mn, "cld")) { emit_byte(0xFC); return; }
    if (streq(mn, "std")) { emit_byte(0xFD); return; }
    if (streq(mn, "cli")) { emit_byte(0xFA); return; }
    if (streq(mn, "sti")) { emit_byte(0xFB); return; }
    if (streq(mn, "hlt")) { emit_byte(0xF4); return; }
    if (streq(mn, "ret")) {
        if (!n) emit_byte(0xC3);
        else {
            if (!parse_operand(ops[0], &a) || a.kind != OP_IMM) {
                set_error("ret needs an immediate"); return;
            }
            emit_byte(0xC2); emit_u16(a.imm);
        }
        return;
    }
    if (streq(mn, "int")) {
        if (n != 1 || !parse_operand(ops[0], &a) || a.kind != OP_IMM) {
            set_error("int needs an immediate"); return;
        }
        emit_byte(0xCD); emit_byte((uint8_t)a.imm); return;
    }
    if (n > 0 && !parse_operand(ops[0], &a)) return;
    if (n > 1 && !parse_operand(ops[1], &b)) return;

    if (streq(mn, "mov")) {
        if (n != 2) { set_error("mov needs two operands"); return; }
        emit_mov(&a, &b); return;
    }
    if (streq(mn, "lea")) {
        if (n != 2 || a.kind != OP_REG || a.width != 32 || b.kind != OP_MEM) {
            set_error("lea needs reg32, memory"); return;
        }
        emit_byte(0x8D); emit_modrm(a.reg, &b); return;
    }
    struct bin_encoding enc;
    if (find_bin_encoding(mn, &enc)) {
        if (n != 2) { set_error("arithmetic needs two operands"); return; }
        emit_binary(&enc, &a, &b); return;
    }
    if (streq(mn, "test")) {
        if (n != 2) { set_error("test needs two operands"); return; }
        int width = operand_width(&a, &b);
        emit_prefix(width);
        if (b.kind == OP_IMM) {
            emit_byte((uint8_t)(width == 8 ? 0xF6 : 0xF7));
            emit_modrm(0, &a);
            if (width == 8) emit_byte((uint8_t)b.imm);
            else if (width == 16) emit_u16(b.imm);
            else emit_u32(b.imm);
        } else if (b.kind == OP_REG) {
            emit_byte((uint8_t)(width == 8 ? 0x84 : 0x85));
            emit_modrm(b.reg, &a);
        } else set_error("unsupported test operands");
        return;
    }
    if (streq(mn, "xchg")) {
        if (n != 2 || a.kind != OP_REG ||
            (b.kind != OP_REG && b.kind != OP_MEM)) {
            set_error("xchg needs register and register/memory"); return;
        }
        emit_prefix(a.width);
        emit_byte((uint8_t)(a.width == 8 ? 0x86 : 0x87));
        emit_modrm(a.reg, &b); return;
    }
    if (streq(mn, "push")) {
        if (n != 1) { set_error("push needs one operand"); return; }
        if (a.kind == OP_REG && a.width == 32) emit_byte((uint8_t)(0x50 + a.reg));
        else if (a.kind == OP_IMM) { emit_byte(0x68); emit_u32(a.imm); }
        else if (a.kind == OP_MEM) { emit_byte(0xFF); emit_modrm(6, &a); }
        else set_error("unsupported push operand");
        return;
    }
    if (streq(mn, "pop")) {
        if (n != 1) { set_error("pop needs one operand"); return; }
        if (a.kind == OP_REG && a.width == 32) emit_byte((uint8_t)(0x58 + a.reg));
        else if (a.kind == OP_MEM) { emit_byte(0x8F); emit_modrm(0, &a); }
        else set_error("unsupported pop operand");
        return;
    }
    if (streq(mn, "inc") || streq(mn, "dec")) {
        if (n != 1) { set_error("inc/dec needs one operand"); return; }
        if (a.kind == OP_REG && a.width == 32)
            emit_byte((uint8_t)((streq(mn, "inc") ? 0x40 : 0x48) + a.reg));
        else {
            emit_prefix(a.width);
            emit_byte((uint8_t)(a.width == 8 ? 0xFE : 0xFF));
            emit_modrm(streq(mn, "inc") ? 0 : 1, &a);
        }
        return;
    }
    if (streq(mn, "not") || streq(mn, "neg") || streq(mn, "mul") ||
        streq(mn, "div") || streq(mn, "idiv")) {
        if (n != 1) { set_error("unary instruction needs one operand"); return; }
        int group = streq(mn, "not") ? 2 : streq(mn, "neg") ? 3 :
                    streq(mn, "mul") ? 4 : streq(mn, "div") ? 6 : 7;
        emit_prefix(a.width);
        emit_byte((uint8_t)(a.width == 8 ? 0xF6 : 0xF7));
        emit_modrm(group, &a); return;
    }
    if (streq(mn, "imul")) {
        if (n == 1) {
            emit_prefix(a.width); emit_byte((uint8_t)(a.width == 8 ? 0xF6 : 0xF7));
            emit_modrm(5, &a);
        } else if (n == 2 && a.kind == OP_REG &&
                   (b.kind == OP_REG || b.kind == OP_MEM)) {
            emit_prefix(a.width); emit_byte(0x0F); emit_byte(0xAF);
            emit_modrm(a.reg, &b);
        } else set_error("unsupported imul operands");
        return;
    }
    if (streq(mn, "shl") || streq(mn, "sal") || streq(mn, "shr") ||
        streq(mn, "sar") || streq(mn, "rol") || streq(mn, "ror")) {
        if (n != 2) { set_error("shift needs two operands"); return; }
        int group = (streq(mn, "rol") ? 0 : streq(mn, "ror") ? 1 :
                     (streq(mn, "shl") || streq(mn, "sal")) ? 4 :
                     streq(mn, "shr") ? 5 : 7);
        emit_prefix(a.width);
        if (b.kind == OP_REG && b.reg == 1 && b.width == 8) {
            emit_byte((uint8_t)(a.width == 8 ? 0xD2 : 0xD3));
            emit_modrm(group, &a);
        } else if (b.kind == OP_IMM) {
            emit_byte((uint8_t)(a.width == 8 ? 0xC0 : 0xC1));
            emit_modrm(group, &a); emit_byte((uint8_t)b.imm);
        } else set_error("shift count must be cl or immediate");
        return;
    }
    if (streq(mn, "movzx") || streq(mn, "movsx")) {
        if (n != 2 || a.kind != OP_REG || a.width != 32 ||
            (b.kind != OP_REG && b.kind != OP_MEM) ||
            (b.width != 8 && b.width != 16)) {
            set_error("movzx/movsx needs reg32, byte/word operand"); return;
        }
        emit_byte(0x0F);
        emit_byte((uint8_t)(streq(mn, "movzx") ?
                  (b.width == 8 ? 0xB6 : 0xB7) :
                  (b.width == 8 ? 0xBE : 0xBF)));
        emit_modrm(a.reg, &b); return;
    }
    if (streq(mn, "call") || streq(mn, "jmp")) {
        if (n != 1) { set_error("call/jmp needs one operand"); return; }
        if (a.kind == OP_IMM) {
            uint32_t cur = current_runtime_addr();
            emit_byte(streq(mn, "call") ? 0xE8 : 0xE9);
            emit_u32(a.imm - (cur + 5));
        } else {
            emit_byte(0xFF);
            emit_modrm(streq(mn, "call") ? 2 : 4, &a);
        }
        return;
    }
    int cc = condition_code(mn);
    if (cc >= 0) {
        if (n != 1 || a.kind != OP_IMM) {
            set_error("conditional jump needs a label/immediate"); return;
        }
        uint32_t cur = current_runtime_addr();
        emit_byte(0x0F); emit_byte((uint8_t)(0x80 + cc));
        emit_u32(a.imm - (cur + 6)); return;
    }
    if (mn[0] == 's' && mn[1] == 'e' && mn[2] == 't') {
        char jump_name[16];
        jump_name[0] = 'j';
        int ji = 1;
        while (mn[ji + 2] && ji + 1 < (int)sizeof(jump_name)) {
            jump_name[ji] = mn[ji + 2];
            ji++;
        }
        jump_name[ji] = 0;
        cc = condition_code(jump_name);
        if (cc < 0 || n != 1 || (a.kind != OP_REG && a.kind != OP_MEM) ||
            a.width != 8) {
            set_error("bad setcc operand"); return;
        }
        emit_byte(0x0F); emit_byte((uint8_t)(0x90 + cc));
        emit_modrm(0, &a); return;
    }

    set_error("unsupported instruction");
}

static int handle_equ_line(char *line) {
    char *p = line;
    char name[NAME_MAX + 1];
    if (!parse_token(&p, name, sizeof(name)))
        return 0;
    char *after_name = p;
    char tok[16];
    if (!parse_token(&p, tok, sizeof(tok)))
        return 0;
    if (!streq(tok, "equ"))
        return 0;
    uint32_t v = 0;
    if (!eval_expr(p, &v))
        return 1;
    define_equ(name, v);
    (void)after_name;
    return 1;
}

static void process_line(char *line) {
    strip_comment(line);
    rtrim(line);
    char *p = skip_ws(line);
    if (!*p)
        return;

    if (p[0] == '%' && p[1]) {
        char tok[32];
        parse_token(&p, tok, sizeof(tok));
        if (streq(tok, "%define")) {
            char name[NAME_MAX + 1];
            if (!parse_token(&p, name, sizeof(name))) {
                set_error("bad define");
                return;
            }
            uint32_t v = 0;
            if (!eval_expr(p, &v))
                return;
            define_equ(name, v);
            return;
        }
        set_error("unsupported directive");
        return;
    }

    if (handle_equ_line(p))
        return;

    char first[NAME_MAX + 1];
    char *label_scan = p;
    if (parse_token(&label_scan, first, sizeof(first))) {
        label_scan = skip_ws(label_scan);
        if (*label_scan == ':') {
            define_label(first);
            p = skip_ws(label_scan + 1);
            if (!*p)
                return;
        }
    }

    char mnemonic[32];
    if (!parse_token(&p, mnemonic, sizeof(mnemonic)))
        return;
    p = skip_ws(p);

    if (streq(mnemonic, "bits")) {
        uint32_t bits = 0;
        if (!eval_expr(p, &bits) || bits != 32)
            set_error("basm only emits 32-bit i386 code");
        return;
    }
    if (streq(mnemonic, "global")) {
        char name[NAME_MAX + 1];
        if (!parse_token(&p, name, sizeof(name)) || *skip_ws(p))
            set_error("global needs exactly one symbol");
        else if (pass_no == 1)
            strcpy(global_entry, name);
        return;
    }
    if (streq(mnemonic, "extern")) {
        set_error("extern is invalid when emitting a standalone executable");
        return;
    }
    if (streq(mnemonic, "section")) {
        if (p[0] == '.')
            p++;
        if (streq(p, "text"))
            cur_section = SEC_TEXT;
        else if (streq(p, "rodata") || streq(p, "data"))
            cur_section = SEC_DATA;
        else if (streq(p, "bss"))
            cur_section = SEC_BSS;
        else
            set_error("unknown section");
        return;
    }

    emit_instruction(mnemonic, p);
}

static int assemble_pass(void) {
    text_len = 0;
    data_len = 0;
    bss_len = 0;
    cur_section = SEC_TEXT;
    line_no = 1;
    int pos = 0;

    while (pos < src_len && !failed) {
        char line[LINE_MAX];
        int n = 0;
        while (pos < src_len && source[pos] != '\n' && n < (int)sizeof(line) - 1)
            line[n++] = source[pos++];
        while (pos < src_len && source[pos] != '\n')
            pos++;
        if (pos < src_len && source[pos] == '\n')
            pos++;
        line[n] = 0;
        process_line(line);
        line_no++;
    }
    return failed ? -1 : 0;
}

static void put16(int off, uint16_t v) {
    elf_buf[off] = (uint8_t)(v & 0xFF);
    elf_buf[off + 1] = (uint8_t)((v >> 8) & 0xFF);
}

static void put32(int off, uint32_t v) {
    elf_buf[off] = (uint8_t)(v & 0xFF);
    elf_buf[off + 1] = (uint8_t)((v >> 8) & 0xFF);
    elf_buf[off + 2] = (uint8_t)((v >> 16) & 0xFF);
    elf_buf[off + 3] = (uint8_t)((v >> 24) & 0xFF);
}

static int build_elf(void) {
    int have_data_segment = data_len > 0 || bss_len > 0;
    int data_file_off = (int)LOAD_OFF + final_text_len;
    int file_size = have_data_segment ? data_file_off + data_len :
                                        (int)LOAD_OFF + text_len;
    if (file_size > ELF_MAX) {
        set_error("ELF too large for /fs");
        return -1;
    }

    memset(elf_buf, 0, sizeof(elf_buf));
    elf_buf[0] = 0x7F;
    elf_buf[1] = 'E';
    elf_buf[2] = 'L';
    elf_buf[3] = 'F';
    elf_buf[4] = 1;
    elf_buf[5] = 1;
    elf_buf[6] = 1;
    put16(0x10, 2);
    put16(0x12, 3);
    put32(0x14, 1);

    struct sym *entry = find_sym(global_entry);
    if (!entry || entry->is_equ || entry->section != SEC_TEXT) {
        set_error("global entry symbol is missing or not in text");
        return -1;
    }
    uint32_t entry_addr = sym_runtime_value(entry);
    put32(0x18, entry_addr);
    put32(0x1C, 52);
    put32(0x20, 0);
    put32(0x24, 0);
    put16(0x28, 52);
    put16(0x2A, 32);
    put16(0x2C, (uint16_t)(have_data_segment ? 2 : 1));

    int ph = 52;
    put32(ph + 0, 1);
    put32(ph + 4, LOAD_OFF);
    put32(ph + 8, USER_BASE);
    put32(ph + 12, USER_BASE);
    put32(ph + 16, (uint32_t)text_len);
    put32(ph + 20, (uint32_t)final_text_len);
    put32(ph + 24, 5);
    put32(ph + 28, 0x1000);

    if (have_data_segment) {
        ph += 32;
        put32(ph + 0, 1);
        put32(ph + 4, (uint32_t)data_file_off);
        put32(ph + 8, USER_BASE + (uint32_t)final_text_len);
        put32(ph + 12, USER_BASE + (uint32_t)final_text_len);
        put32(ph + 16, (uint32_t)data_len);
        put32(ph + 20, (uint32_t)(data_len + bss_len));
        put32(ph + 24, 6);
        put32(ph + 28, 0x1000);
    }

    memcpy(elf_buf + LOAD_OFF, text_buf, (size_t)text_len);
    if (data_len > 0)
        memcpy(elf_buf + data_file_off, data_buf, (size_t)data_len);
    return file_size;
}

#ifndef BASM_LIBRARY
static int read_file(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    src_len = 0;
    for (;;) {
        int room = SRC_MAX - src_len;
        if (room <= 0) {
            close(fd);
            return -1;
        }
        int n = read(fd, source + src_len, (size_t)room);
        if (n <= 0)
            break;
        src_len += n;
    }
    close(fd);
    source[src_len] = 0;
    return 0;
}
#endif

static int write_file(const char *path, const uint8_t *buf, int len) {
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY);
    if (fd < 0)
        return -1;
    int off = 0;
    while (off < len) {
        int n = write(fd, buf + off, (size_t)(len - off));
        if (n <= 0) {
            close(fd);
            return -1;
        }
        off += n;
    }
    close(fd);
    return 0;
}

#ifndef BASM_LIBRARY
static void default_output_path(const char *src, char *out, int out_size) {
    int n = 0;
    while (src[n] && n < out_size - 1) {
        out[n] = src[n];
        n++;
    }
    out[n] = 0;
    if (n > 4 && out[n - 4] == '.' && out[n - 3] == 'a' &&
        out[n - 2] == 's' && out[n - 1] == 'm') {
        out[n - 4] = 0;
    }
}
#endif

static int compile_loaded_source(const char *name, const char *out_path, int verbose) {
    memset(syms, 0, sizeof(syms));
    failed = 0;
    pass_no = 1;
    if (assemble_pass() < 0)
        goto fail;
    final_text_len = (text_len + 0xFFF) & ~0xFFF;
    final_data_len = data_len;

    pass_no = 2;
    if (assemble_pass() < 0)
        goto fail;

    int elf_size = build_elf();
    if (elf_size < 0)
        goto fail;
    if (write_file(out_path, elf_buf, elf_size) < 0) {
        puts("basm: write failed");
        return 1;
    }

    if (verbose)
        printf("basm: wrote %s (%d bytes)\n", out_path, elf_size);
    return 0;

fail:
    printf("%s: line %d: %s\n", name ? name : "basm", line_no,
           err_msg[0] ? err_msg : "error");
    return 1;
}

int basm_compile_source(const char *name, const char *input, int input_len,
                        const char *output_path, int verbose) {
    if (!input || input_len < 0 || input_len > SRC_MAX || !output_path) {
        puts("basm: source too large");
        return 1;
    }
    memcpy(source, input, (size_t)input_len);
    source[input_len] = 0;
    src_len = input_len;
    return compile_loaded_source(name, output_path, verbose);
}

#ifndef BASM_LIBRARY
int main(int argc, char **argv) {
    if (argc < 2) {
        puts("usage: basm <input.asm> [output]");
        puts("example: basm /fs/demo.asm /fs/demo");
        return 1;
    }

    const char *in_path = argv[1];
    char out_path[128];
    if (argc >= 3) {
        int i = 0;
        while (argv[2][i] && i < (int)sizeof(out_path) - 1) {
            out_path[i] = argv[2][i];
            i++;
        }
        out_path[i] = 0;
    } else {
        default_output_path(in_path, out_path, sizeof(out_path));
    }

    if (read_file(in_path) < 0) {
        puts("basm: read failed");
        return 1;
    }
    return compile_loaded_source("basm", out_path, 1);
}
#endif
