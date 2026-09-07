#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdarg.h>
#ifdef __MVS__
#include <clibvsam.h>
#endif

#if defined(__MVS__) && defined(DBASE_TSO)
extern int dbtget(char *buf, int max) asm("DBTGET");
extern int dbtput(char *buf, int len) asm("DBTPUT");
#endif

#define MAX_LINE 512
#define MAX_NAME 16
#define MAX_FIELDS 12
#define MAX_INDEXES 8
#define MAX_WORK_AREAS 10
#define MAX_VALUE 32
#define MAX_ROWS 999
#define MAX_VARS 32
#define MAX_SCRIPT_LINES 200
#define MAX_DO_DEPTH 4
#define MAX_WHILE_DEPTH 16
#define KEY_LEN 64
#define DATA_LEN 960
#define DB_DD "DBASEV"
#define TYPE_CHAR 'C'
#define TYPE_NUM 'N'
#define TYPE_DATE 'D'
#define TYPE_LOGICAL 'L'

struct Field {
    char name[MAX_NAME + 1];
    char type;
    int len;
};

struct Table {
    char name[MAX_NAME + 1];
    int field_count;
    struct Field fields[MAX_FIELDS];
};

struct Rec {
    char key[KEY_LEN];
    char data[DATA_LEN];
};

struct IndexDef {
    char table[MAX_NAME + 1];
    char name[MAX_NAME + 1];
    char field[MAX_NAME + 1];
};

struct RelationDef {
    int active;
    char parent_table[MAX_NAME + 1];
    char parent_field[MAX_NAME + 1];
    char child_table[MAX_NAME + 1];
    char child_field[MAX_NAME + 1];
};

struct MemVar {
    char name[MAX_NAME + 1];
    char value[MAX_VALUE + 1];
};

struct WorkArea {
    int have_table;
    struct Table table;
    int recno;
    int have_index;
    struct IndexDef index;
    char alias[MAX_NAME + 1];
    char filter[MAX_LINE];
    int deleted_on;
};

static struct Table g_table;
static int g_have_table = 0;
static int g_recno = 0;
static int g_store_rc = 0;
static char g_locate_for[MAX_LINE];
static int g_locate_recno = 0;
static struct IndexDef g_index;
static int g_have_index = 0;
static struct RelationDef g_relation;
static struct WorkArea g_areas[MAX_WORK_AREAS];
static int g_area = 0;
static char g_filter[MAX_LINE];
static int g_deleted_on = 1;
static struct MemVar g_vars[MAX_VARS];
static int g_var_count = 0;
static int g_do_depth = 0;

static void say(const char *fmt, ...);
static int is_numeric_text(const char *s);

#if defined(__MVS__) && defined(DBASE_TSO)
static char g_out[MAX_LINE];
static int g_out_len = 0;
#endif
#ifdef __MVS__
static VSFILE *g_kv = NULL;
#else
static struct Rec g_host_kv[4096];
static int g_host_kv_count = 0;
#endif

static void rtrim(char *s)
{
    int n = (int)strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) {
        s[--n] = '\0';
    }
}

static void ltrim(char *s)
{
    int i = 0;
    while (s[i] && isspace((unsigned char)s[i])) {
        i++;
    }
    if (i > 0) {
        memmove(s, s + i, strlen(s + i) + 1);
    }
}

static void trim(char *s)
{
    rtrim(s);
    ltrim(s);
}

static void upcase(char *s)
{
    int i;
    for (i = 0; s[i]; i++) {
        s[i] = (char)toupper((unsigned char)s[i]);
    }
}

static int same_word(const char *a, const char *b)
{
    while (*a && *b) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static int starts_word(const char *s, const char *word)
{
    while (*word) {
        if (toupper((unsigned char)*s) != toupper((unsigned char)*word)) {
            return 0;
        }
        s++;
        word++;
    }
    return *s == '\0' || isspace((unsigned char)*s);
}

static int key_has_prefix(const char *key, const char *prefix, int prefix_len)
{
    return memcmp(key, prefix, prefix_len) == 0;
}

static void save_area(void)
{
    struct WorkArea *a = &g_areas[g_area];
    a->have_table = g_have_table;
    a->table = g_table;
    a->recno = g_recno;
    a->have_index = g_have_index;
    a->index = g_index;
    strncpy(a->filter, g_filter, sizeof(a->filter));
    a->filter[sizeof(a->filter) - 1] = '\0';
    a->deleted_on = g_deleted_on;
}

static void load_area(void)
{
    struct WorkArea *a = &g_areas[g_area];
    g_have_table = a->have_table;
    g_table = a->table;
    g_recno = a->recno;
    g_have_index = a->have_index;
    g_index = a->index;
    strncpy(g_filter, a->filter, sizeof(g_filter));
    g_filter[sizeof(g_filter) - 1] = '\0';
    g_deleted_on = a->deleted_on;
}

static void init_areas(void)
{
    int i;
    for (i = 0; i < MAX_WORK_AREAS; i++) {
        memset(&g_areas[i], 0, sizeof(g_areas[i]));
        g_areas[i].deleted_on = 1;
    }
    g_area = 0;
    load_area();
}

static const char *current_alias(void)
{
    if (g_areas[g_area].alias[0] != '\0') {
        return g_areas[g_area].alias;
    }
    if (g_have_table) {
        return g_table.name;
    }
    return "";
}

static void normalize_field_ref(char *name)
{
    char *arrow;
    trim(name);
    arrow = strstr(name, "->");
    if (arrow != NULL) {
        char alias[MAX_NAME + 1];
        int n = (int)(arrow - name);
        if (n > MAX_NAME) {
            n = MAX_NAME;
        }
        strncpy(alias, name, n);
        alias[n] = '\0';
        trim(alias);
        upcase(alias);
        if (!same_word(alias, current_alias())) {
            say("? alias is not selected in current work area: %s\n", alias);
        }
        memmove(name, arrow + 2, strlen(arrow + 2) + 1);
    }
    trim(name);
    upcase(name);
}

static int var_index(const char *name)
{
    char tmp[MAX_NAME + 1];
    int i;

    strncpy(tmp, name, MAX_NAME);
    tmp[MAX_NAME] = '\0';
    trim(tmp);
    upcase(tmp);
    for (i = 0; i < g_var_count; i++) {
        if (same_word(g_vars[i].name, tmp)) {
            return i;
        }
    }
    return -1;
}

static const char *var_value(const char *name)
{
    int ix = var_index(name);
    if (ix < 0) {
        return "";
    }
    return g_vars[ix].value;
}

static int set_var(const char *name, const char *value)
{
    int ix;
    char vname[MAX_NAME + 1];

    strncpy(vname, name, MAX_NAME);
    vname[MAX_NAME] = '\0';
    trim(vname);
    if (vname[0] == '&') {
        memmove(vname, vname + 1, strlen(vname));
    }
    upcase(vname);
    if (vname[0] == '\0') {
        say("? memory variable name missing\n");
        return 0;
    }
    ix = var_index(vname);
    if (ix < 0) {
        if (g_var_count >= MAX_VARS) {
            say("? too many memory variables\n");
            return 0;
        }
        ix = g_var_count++;
        strncpy(g_vars[ix].name, vname, MAX_NAME);
        g_vars[ix].name[MAX_NAME] = '\0';
    }
    strncpy(g_vars[ix].value, value, MAX_VALUE);
    g_vars[ix].value[MAX_VALUE] = '\0';
    trim(g_vars[ix].value);
    return 1;
}

static void expand_vars(char *line)
{
    char out[MAX_LINE];
    int i = 0;
    int j = 0;

    while (line[i] != '\0' && j < MAX_LINE - 1) {
        if (line[i] == '&') {
            char name[MAX_NAME + 1];
            const char *val;
            int n = 0;
            i++;
            while ((isalnum((unsigned char)line[i]) || line[i] == '_') &&
                   n < MAX_NAME) {
                name[n++] = line[i++];
            }
            name[n] = '\0';
            val = var_value(name);
            while (*val != '\0' && j < MAX_LINE - 1) {
                out[j++] = *val++;
            }
        } else {
            out[j++] = line[i++];
        }
    }
    out[j] = '\0';
    strncpy(line, out, MAX_LINE);
    line[MAX_LINE - 1] = '\0';
}

static void eval_value(const char *expr, char *out, int max)
{
    char buf[MAX_LINE];
    char *op = NULL;
    char *p;

    strncpy(buf, expr, sizeof(buf));
    buf[sizeof(buf) - 1] = '\0';
    trim(buf);
    expand_vars(buf);
    for (p = buf + 1; *p != '\0'; p++) {
        if (*p == '+' || *p == '-') {
            op = p;
            break;
        }
    }
    if (op != NULL) {
        char left[MAX_VALUE + 1];
        char right[MAX_VALUE + 1];
        long a;
        long b;
        int n = (int)(op - buf);
        if (n > MAX_VALUE) {
            n = MAX_VALUE;
        }
        strncpy(left, buf, n);
        left[n] = '\0';
        strncpy(right, op + 1, sizeof(right));
        right[sizeof(right) - 1] = '\0';
        trim(left);
        trim(right);
        if (is_numeric_text(left) && is_numeric_text(right)) {
            a = atol(left);
            b = atol(right);
            if (*op == '+') {
                snprintf(out, max, "%ld", a + b);
            } else {
                snprintf(out, max, "%ld", a - b);
            }
            return;
        }
    }
    strncpy(out, buf, max);
    out[max - 1] = '\0';
    trim(out);
}

static int has_outer_parens(const char *s)
{
    int depth = 0;
    int i;
    int n = (int)strlen(s);

    if (n < 2 || s[0] != '(' || s[n - 1] != ')') {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (s[i] == '(') {
            depth++;
        } else if (s[i] == ')') {
            depth--;
            if (depth == 0 && i < n - 1) {
                return 0;
            }
        }
        if (depth < 0) {
            return 0;
        }
    }
    return depth == 0;
}

static void strip_outer_parens(char *s)
{
    while (has_outer_parens(s)) {
        int n = (int)strlen(s);
        memmove(s, s + 1, n - 2);
        s[n - 2] = '\0';
        trim(s);
    }
}

static char *find_logic_op(char *s, const char *op)
{
    int depth = 0;
    int oplen = (int)strlen(op);
    int i;

    for (i = 0; s[i] != '\0'; i++) {
        if (s[i] == '(') {
            depth++;
        } else if (s[i] == ')') {
            depth--;
        } else if (depth == 0 &&
                   (i == 0 || isspace((unsigned char)s[i - 1])) &&
                   (s[i + oplen] == '\0' ||
                    isspace((unsigned char)s[i + oplen]))) {
            int j;
            int match = 1;
            for (j = 0; j < oplen; j++) {
                if (toupper((unsigned char)s[i + j]) !=
                    toupper((unsigned char)op[j])) {
                    match = 0;
                    break;
                }
            }
            if (match) {
                return s + i;
            }
        }
    }
    return NULL;
}

static int eval_atom_condition(const char *expr)
{
    char buf[MAX_LINE];
    char left[MAX_VALUE + 1];
    char right[MAX_VALUE + 1];
    char op[3];
    char *value = NULL;
    int i;

    strncpy(buf, expr, sizeof(buf));
    buf[sizeof(buf) - 1] = '\0';
    trim(buf);
    expand_vars(buf);
    if (starts_word(buf, "IF")) {
        memmove(buf, buf + 2, strlen(buf + 2) + 1);
        trim(buf);
    }
    if (starts_word(buf, "WHILE")) {
        memmove(buf, buf + 5, strlen(buf + 5) + 1);
        trim(buf);
    }
    op[0] = '\0';
    for (i = 0; buf[i] != '\0'; i++) {
        if ((buf[i] == '<' || buf[i] == '>' || buf[i] == '!') &&
            buf[i + 1] == '=') {
            op[0] = buf[i];
            op[1] = '=';
            op[2] = '\0';
            buf[i] = '\0';
            value = buf + i + 2;
            break;
        }
        if (buf[i] == '<' && buf[i + 1] == '>') {
            strcpy(op, "<>");
            buf[i] = '\0';
            value = buf + i + 2;
            break;
        }
        if (buf[i] == '=' || buf[i] == '<' || buf[i] == '>') {
            op[0] = buf[i];
            op[1] = '\0';
            buf[i] = '\0';
            value = buf + i + 1;
            break;
        }
    }
    if (value == NULL) {
        trim(buf);
        if (buf[0] == '\0') {
            return 0;
        }
        if (is_numeric_text(buf)) {
            return atol(buf) != 0;
        }
        return 1;
    }
    strncpy(left, buf, sizeof(left));
    left[sizeof(left) - 1] = '\0';
    strncpy(right, value, sizeof(right));
    right[sizeof(right) - 1] = '\0';
    trim(left);
    trim(right);
    if (is_numeric_text(left) && is_numeric_text(right)) {
        long a = atol(left);
        long b = atol(right);
        if (strcmp(op, "=") == 0) return a == b;
        if (strcmp(op, "<>") == 0 || strcmp(op, "!=") == 0) return a != b;
        if (strcmp(op, ">") == 0) return a > b;
        if (strcmp(op, "<") == 0) return a < b;
        if (strcmp(op, ">=") == 0) return a >= b;
        if (strcmp(op, "<=") == 0) return a <= b;
        return 0;
    }
    if (strcmp(op, "=") == 0) return same_word(left, right);
    if (strcmp(op, "<>") == 0 || strcmp(op, "!=") == 0) {
        return !same_word(left, right);
    }
    i = strcmp(left, right);
    if (strcmp(op, ">") == 0) return i > 0;
    if (strcmp(op, "<") == 0) return i < 0;
    if (strcmp(op, ">=") == 0) return i >= 0;
    if (strcmp(op, "<=") == 0) return i <= 0;
    return 0;
}

static int eval_condition(const char *expr)
{
    char buf[MAX_LINE];
    char *op;

    strncpy(buf, expr, sizeof(buf));
    buf[sizeof(buf) - 1] = '\0';
    trim(buf);
    expand_vars(buf);
    if (starts_word(buf, "IF")) {
        memmove(buf, buf + 2, strlen(buf + 2) + 1);
        trim(buf);
    }
    if (starts_word(buf, "WHILE")) {
        memmove(buf, buf + 5, strlen(buf + 5) + 1);
        trim(buf);
    }
    strip_outer_parens(buf);
    op = find_logic_op(buf, "OR");
    if (op != NULL) {
        char right[MAX_LINE];
        *op = '\0';
        strncpy(right, op + 2, sizeof(right));
        right[sizeof(right) - 1] = '\0';
        return eval_condition(buf) || eval_condition(right);
    }
    op = find_logic_op(buf, "AND");
    if (op != NULL) {
        char right[MAX_LINE];
        *op = '\0';
        strncpy(right, op + 3, sizeof(right));
        right[sizeof(right) - 1] = '\0';
        return eval_condition(buf) && eval_condition(right);
    }
    if (starts_word(buf, "NOT")) {
        memmove(buf, buf + 3, strlen(buf + 3) + 1);
        trim(buf);
        return !eval_condition(buf);
    }
    return eval_atom_condition(buf);
}

static void normalize_command_line(char *line)
{
    static const char *cmds[] = {
        "HELP", "CREATE", "USE", "APPEND", "LIST", "BROWSE",
        "DISPLAY", "REPLACE", "DELETE", "PACK", "FIND", "LOCATE",
        "COUNT", "QUIT", "EXIT", "GO", "GOTO", "SKIP", "RECALL", "TABLES",
        "COPY", "LOCATE", "CONTINUE", "SUM", "AVERAGE", "ZAP",
        "INDEX", "INDEXES", "REINDEX", "SELECT", "SET", "SEEK", "RELATION", "STORE",
        "DO", "IF", "ELSE", "ENDIF", "WHILE", "ENDDO", NULL
    };
    int i;
    int j;

    trim(line);
    while (line[0] == '.') {
        memmove(line, line + 1, strlen(line));
        trim(line);
    }
    if (line[0] == '\0' || line[0] == '?' ||
        isalpha((unsigned char)line[0])) {
        return;
    }

    for (i = 0; line[i]; i++) {
        if (!isalpha((unsigned char)line[i]) && line[i] != '.') {
            continue;
        }
        for (j = 0; cmds[j] != NULL; j++) {
            const char *p = line + i;
            if (*p == '.') {
                p++;
            }
            if (starts_word(p, cmds[j])) {
                memmove(line, p, strlen(p) + 1);
                trim(line);
                return;
            }
        }
    }
}

static void say(const char *fmt, ...)
{
    char tmp[MAX_LINE];
    va_list ap;
    int n;
#if defined(__MVS__) && defined(DBASE_TSO)
    int i;
#endif

    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    tmp[sizeof(tmp) - 1] = '\0';

#if defined(__MVS__) && defined(DBASE_TSO)
    for (i = 0; tmp[i]; i++) {
        if (tmp[i] == '\n') {
            if (g_out_len > 0) {
                g_out[g_out_len] = '\0';
                dbtput(g_out, g_out_len);
                g_out_len = 0;
            }
        } else {
            if (g_out_len >= (int)sizeof(g_out) - 1) {
                g_out[g_out_len] = '\0';
                dbtput(g_out, g_out_len);
                g_out_len = 0;
            }
            g_out[g_out_len++] = tmp[i];
        }
    }
#else
    fputs(tmp, stdout);
#endif
}

static void flush_out(void)
{
#if defined(__MVS__) && defined(DBASE_TSO)
    if (g_out_len > 0) {
        g_out[g_out_len] = '\0';
        dbtput(g_out, g_out_len);
        g_out_len = 0;
    }
#else
    fflush(stdout);
#endif
}

static int read_line(char *buf, int max)
{
#if defined(__MVS__) && defined(DBASE_TSO)
    int rc;
    memset(buf, 0, max);
    rc = dbtget(buf, max);
    if (rc < 0) {
        return 0;
    }
    buf[max - 1] = '\0';
    rtrim(buf);
    normalize_command_line(buf);
    return 1;
#else
    if (fgets(buf, max, stdin) == NULL) {
        return 0;
    }
    rtrim(buf);
    normalize_command_line(buf);
    return 1;
#endif
}

static void make_key(char *out, const char *kind, const char *name, int seq)
{
    char tmp[KEY_LEN + 1];
    char mapped[2];
    mapped[0] = kind[0];
    mapped[1] = '\0';
    if (kind[0] == 'T') {
        mapped[0] = 'A';
    } else if (kind[0] == 'C') {
        mapped[0] = 'B';
    }
    memset(out, ' ', KEY_LEN);
    if (seq >= 0) {
        sprintf(tmp, "%s|%-16.16s|%06d", mapped, name, seq);
    } else {
        sprintf(tmp, "%s|%-16.16s", mapped, name);
    }
    memcpy(out, tmp, strlen(tmp));
}

static void make_name_key(char *out, const char *kind, const char *table,
                          const char *name)
{
    char tmp[KEY_LEN + 1];
    memset(out, ' ', KEY_LEN);
    sprintf(tmp, "%s|%-16.16s|%-16.16s", kind, table, name);
    memcpy(out, tmp, strlen(tmp));
}

static void make_index_key(char *out, const struct IndexDef *idx,
                           const char *value, int seq)
{
    char tmp[KEY_LEN + 1];
    char val[21];
    strncpy(val, value, 20);
    val[20] = '\0';
    upcase(val);
    memset(out, ' ', KEY_LEN);
    sprintf(tmp, "K|%-16.16s|%-16.16s|%-20.20s|%06d",
            idx->table, idx->name, val, seq);
    memcpy(out, tmp, strlen(tmp));
}

static void data_put(char *dst, const char *src)
{
    memset(dst, ' ', DATA_LEN);
    strncpy(dst, src, DATA_LEN);
}

static void data_get(char *dst, const char *src, int max)
{
    int n;
    if (max < 1) {
        return;
    }
    n = max - 1;
    if (n > DATA_LEN) {
        n = DATA_LEN;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
    rtrim(dst);
}

static int kv_open(void)
{
#ifdef __MVS__
    if (g_kv != NULL) {
        return 1;
    }
    return __vsopen(DB_DD, VSTYPE_KSDS, VSACCESS_DYNAM, VSMODE_UPD, &g_kv) == 0;
#else
    return 1;
#endif
}

static void kv_close(void)
{
#ifdef __MVS__
    if (g_kv != NULL) {
        __vsclos(g_kv);
        g_kv = NULL;
    }
#endif
}

static int kv_get(const char *key, char *data, int max)
{
    struct Rec rec;
#ifdef __MVS__
    int rc;
    if (!kv_open()) {
        return 0;
    }
    memset(&rec, ' ', sizeof(rec));
    rc = __vsread(g_kv, &rec, sizeof(rec), (void *)key, KEY_LEN);
    if (rc < 0) {
        __vsclr(g_kv);
        return 0;
    }
    data_get(data, rec.data, max);
    return 1;
#else
    int i;
    for (i = 0; i < g_host_kv_count; i++) {
        if (memcmp(g_host_kv[i].key, key, KEY_LEN) == 0) {
            data_get(data, g_host_kv[i].data, max);
            return 1;
        }
    }
    return 0;
#endif
}

static int kv_put(const char *key, const char *data)
{
    struct Rec rec;
#ifdef __MVS__
    char old[DATA_LEN + 1];
#else
    int i;
#endif
    memset(&rec, ' ', sizeof(rec));
    memcpy(rec.key, key, KEY_LEN);
    data_put(rec.data, data);
#ifdef __MVS__
    if (!kv_open()) {
        return 0;
    }
    if (kv_get(key, old, sizeof(old))) {
        memset(&rec, ' ', sizeof(rec));
        __vsread(g_kv, &rec, sizeof(rec), (void *)key, KEY_LEN);
        g_store_rc = __vsdel(g_kv, &rec, sizeof(rec));
        if (g_store_rc != 0) {
            kv_close();
            return 0;
        }
        kv_close();
        if (!kv_open()) {
            return 0;
        }
        memset(&rec, ' ', sizeof(rec));
        memcpy(rec.key, key, KEY_LEN);
        data_put(rec.data, data);
        g_store_rc = __vswrit(g_kv, &rec, sizeof(rec), (void *)key, KEY_LEN);
        kv_close();
        return g_store_rc == 0;
    }
    g_store_rc = __vswrit(g_kv, &rec, sizeof(rec), (void *)key, KEY_LEN);
    kv_close();
    return g_store_rc == 0;
#else
    for (i = 0; i < g_host_kv_count; i++) {
        if (memcmp(g_host_kv[i].key, key, KEY_LEN) == 0) {
            g_host_kv[i] = rec;
            return 1;
        }
    }
    if (g_host_kv_count >= (int)(sizeof(g_host_kv) / sizeof(g_host_kv[0]))) {
        return 0;
    }
    g_host_kv[g_host_kv_count++] = rec;
    return 1;
#endif
}

static int kv_delete(const char *key)
{
#ifdef __MVS__
    struct Rec rec;
    int rc;
    if (!kv_open()) {
        return 0;
    }
    memset(&rec, ' ', sizeof(rec));
    rc = __vsread(g_kv, &rec, sizeof(rec), (void *)key, KEY_LEN);
    if (rc < 0) {
        __vsclr(g_kv);
        return 1;
    }
    g_store_rc = __vsdel(g_kv, &rec, sizeof(rec));
    kv_close();
    return g_store_rc == 0;
#else
    int i;
    int j;
    for (i = 0; i < g_host_kv_count; i++) {
        if (memcmp(g_host_kv[i].key, key, KEY_LEN) == 0) {
            for (j = i; j < g_host_kv_count - 1; j++) {
                g_host_kv[j] = g_host_kv[j + 1];
            }
            g_host_kv_count--;
            return 1;
        }
    }
    return 1;
#endif
}

static int kv_scan(const char *prefix, int prefix_len,
                   int (*cb)(const char *key, const char *data, void *arg),
                   void *arg)
{
#ifdef __MVS__
    struct Rec rec;
    char last_key[KEY_LEN];
    int rc;
    int have_last = 0;
    if (!kv_open()) {
        return 0;
    }
    memset(&rec, ' ', sizeof(rec));
    rc = __vsstge(g_kv, &rec, sizeof(rec), (void *)prefix, prefix_len);
    if (rc != 0) {
        __vsclr(g_kv);
        return 1;
    }
    if (key_has_prefix(rec.key, prefix, prefix_len)) {
        char data[DATA_LEN + 1];
        data_get(data, rec.data, sizeof(data));
        if (!cb(rec.key, data, arg)) {
            return 1;
        }
        memcpy(last_key, rec.key, KEY_LEN);
        have_last = 1;
    }
    while ((rc = __vsread(g_kv, &rec, sizeof(rec), NULL, 0)) >= 0) {
        char data[DATA_LEN + 1];
        if (!key_has_prefix(rec.key, prefix, prefix_len)) {
            break;
        }
        if (have_last && memcmp(last_key, rec.key, KEY_LEN) == 0) {
            continue;
        }
        data_get(data, rec.data, sizeof(data));
        if (!cb(rec.key, data, arg)) {
            break;
        }
        memcpy(last_key, rec.key, KEY_LEN);
        have_last = 1;
    }
    return rc == -2 ? 0 : 1;
#else
    int i;
    for (i = 0; i < g_host_kv_count; i++) {
        char data[DATA_LEN + 1];
        if (key_has_prefix(g_host_kv[i].key, prefix, prefix_len)) {
            data_get(data, g_host_kv[i].data, sizeof(data));
            if (!cb(g_host_kv[i].key, data, arg)) {
                break;
            }
        }
    }
    return 1;
#endif
}

static int parse_table_def(const char *text, struct Table *t)
{
    char buf[DATA_LEN + 1];
    char *p;
    char *tok;
    int i = 0;

    strncpy(buf, text, sizeof(buf));
    buf[sizeof(buf) - 1] = '\0';
    p = strtok(buf, "|");
    if (p == NULL) {
        return 0;
    }
    strncpy(t->name, p, MAX_NAME);
    t->name[MAX_NAME] = '\0';
    p = strtok(NULL, "|");
    if (p == NULL) {
        return 0;
    }
    t->field_count = atoi(p);
    if (t->field_count < 1 || t->field_count > MAX_FIELDS) {
        return 0;
    }
    while ((tok = strtok(NULL, "|")) != NULL && i < t->field_count) {
        char *colon = strchr(tok, ':');
        if (colon == NULL) {
            return 0;
        }
        *colon = '\0';
        {
            char *colon2;
            colon2 = strchr(colon + 1, ':');
            strncpy(t->fields[i].name, tok, MAX_NAME);
            t->fields[i].name[MAX_NAME] = '\0';
            if (colon2 != NULL) {
                *colon2 = '\0';
                t->fields[i].type = (char)toupper((unsigned char)*(colon + 1));
                t->fields[i].len = atoi(colon2 + 1);
            } else {
                t->fields[i].type = TYPE_CHAR;
                t->fields[i].len = atoi(colon + 1);
            }
        }
        if (t->fields[i].len < 1 || t->fields[i].len > MAX_VALUE) {
            return 0;
        }
        i++;
    }
    return i == t->field_count;
}

static void serialize_table_def(const struct Table *t, char *out, int max)
{
    char part[64];
    int i;
    sprintf(out, "%s|%d", t->name, t->field_count);
    for (i = 0; i < t->field_count; i++) {
        sprintf(part, "|%s:%c:%d", t->fields[i].name,
                t->fields[i].type, t->fields[i].len);
        strncat(out, part, max - (int)strlen(out) - 1);
    }
}

static int load_table(const char *name)
{
    char key[KEY_LEN];
    char data[DATA_LEN + 1];
    char uname[MAX_NAME + 1];

    strncpy(uname, name, MAX_NAME);
    uname[MAX_NAME] = '\0';
    trim(uname);
    upcase(uname);
    make_key(key, "T", uname, -1);
    if (!kv_get(key, data, sizeof(data))) {
        return 0;
    }
    if (!parse_table_def(data, &g_table)) {
        return 0;
    }
    g_have_table = 1;
    g_recno = 1;
    g_have_index = 0;
    return 1;
}

static int load_table_def(const char *name, struct Table *t)
{
    char key[KEY_LEN];
    char data[DATA_LEN + 1];
    char uname[MAX_NAME + 1];

    strncpy(uname, name, MAX_NAME);
    uname[MAX_NAME] = '\0';
    trim(uname);
    upcase(uname);
    make_key(key, "T", uname, -1);
    if (!kv_get(key, data, sizeof(data))) {
        return 0;
    }
    return parse_table_def(data, t);
}

static int is_numeric_text(const char *s)
{
    int i = 0;
    int digit = 0;
    if (s[0] == '-' || s[0] == '+') {
        i++;
    }
    for (; s[i]; i++) {
        if (s[i] == '.') {
            continue;
        }
        if (!isdigit((unsigned char)s[i])) {
            return 0;
        }
        digit = 1;
    }
    return digit;
}

static int is_date_text(const char *s)
{
    int i;
    if (strlen(s) == 8) {
        for (i = 0; i < 8; i++) {
            if (!isdigit((unsigned char)s[i])) {
                return 0;
            }
        }
        return 1;
    }
    if (strlen(s) == 10 && s[2] == '.' && s[5] == '.') {
        for (i = 0; i < 10; i++) {
            if (i == 2 || i == 5) {
                continue;
            }
            if (!isdigit((unsigned char)s[i])) {
                return 0;
            }
        }
        return 1;
    }
    return 0;
}

static int validate_value(int ix, const char *value)
{
    char c;
    if ((int)strlen(value) > g_table.fields[ix].len) {
        say("? value too long for %s\n", g_table.fields[ix].name);
        return 0;
    }
    switch (g_table.fields[ix].type) {
    case TYPE_NUM:
        if (value[0] != '\0' && !is_numeric_text(value)) {
            say("? %s expects numeric value\n", g_table.fields[ix].name);
            return 0;
        }
        break;
    case TYPE_DATE:
        if (value[0] != '\0' && !is_date_text(value)) {
            say("? %s expects date YYYYMMDD or DD.MM.YYYY\n",
                g_table.fields[ix].name);
            return 0;
        }
        break;
    case TYPE_LOGICAL:
        c = (char)toupper((unsigned char)value[0]);
        if (value[0] != '\0' &&
            !(c == 'Y' || c == 'N' || c == 'T' || c == 'F')) {
            say("? %s expects Y/N or T/F\n", g_table.fields[ix].name);
            return 0;
        }
        break;
    default:
        break;
    }
    return 1;
}

static int get_count(const char *table)
{
    char key[KEY_LEN];
    char row[DATA_LEN + 1];
    int count = 0;
    int i;
    for (i = 1; i <= MAX_ROWS; i++) {
        make_key(key, "R", table, i);
        if (!kv_get(key, row, sizeof(row))) {
            break;
        }
        count = i;
    }
    return count;
}

static int field_index(const char *name)
{
    char ref[MAX_NAME * 2 + 4];
    int i;
    strncpy(ref, name, sizeof(ref));
    ref[sizeof(ref) - 1] = '\0';
    normalize_field_ref(ref);
    for (i = 0; i < g_table.field_count; i++) {
        if (same_word(g_table.fields[i].name, ref)) {
            return i;
        }
    }
    return -1;
}

static void split_values(char *row, char vals[MAX_FIELDS][MAX_VALUE + 1]);
static void show_relation_for_parent(const char *parent_row);
static int require_table(void);
static void print_header(void);
static void print_row(int seq, const char *row);
static int read_row(int seq, char *row, int max);
static int row_matches_for(const char *row, const char *expr);
static void dispatch(char *line);

static void row_values(const char *row, char vals[MAX_FIELDS][MAX_VALUE + 1])
{
    char body[DATA_LEN + 1];
    strncpy(body, row + 1, sizeof(body));
    body[sizeof(body) - 1] = '\0';
    split_values(body, vals);
}

static int row_visible(const char *row, int all, const char *forp)
{
    if (!all && g_deleted_on && row[0] == '*') {
        return 0;
    }
    if (g_filter[0] != '\0' && !row_matches_for(row, g_filter)) {
        return 0;
    }
    if (forp != NULL && forp[0] != '\0' && !row_matches_for(row, forp)) {
        return 0;
    }
    return 1;
}

static void split_values_for(const struct Table *t, char *row,
                             char vals[MAX_FIELDS][MAX_VALUE + 1])
{
    char *p;
    int i;
    for (i = 0; i < MAX_FIELDS; i++) {
        vals[i][0] = '\0';
    }
    p = strtok(row, "|");
    i = 0;
    while (p != NULL && i < t->field_count) {
        strncpy(vals[i], p, MAX_VALUE);
        vals[i][MAX_VALUE] = '\0';
        p = strtok(NULL, "|");
        i++;
    }
}

static int field_index_for(const struct Table *t, const char *name)
{
    int i;
    for (i = 0; i < t->field_count; i++) {
        if (same_word(t->fields[i].name, name)) {
            return i;
        }
    }
    return -1;
}

static int compare_values(int field, const char *a, const char *op,
                          const char *b)
{
    int cmp;
    if (g_table.fields[field].type == TYPE_NUM) {
        double da = atof(a);
        double db = atof(b);
        if (a[0] == '\0' || b[0] == '\0') {
            if (strcmp(op, "=") == 0) {
                return a[0] == '\0' && b[0] == '\0';
            }
            if (strcmp(op, "<>") == 0 || strcmp(op, "!=") == 0) {
                return !(a[0] == '\0' && b[0] == '\0');
            }
            return 0;
        }
        if (strcmp(op, "=") == 0) {
            return da == db;
        }
        if (strcmp(op, "<>") == 0 || strcmp(op, "!=") == 0) {
            return da != db;
        }
        if (strcmp(op, ">") == 0) {
            return da > db;
        }
        if (strcmp(op, "<") == 0) {
            return da < db;
        }
        if (strcmp(op, ">=") == 0) {
            return da >= db;
        }
        if (strcmp(op, "<=") == 0) {
            return da <= db;
        }
        return 0;
    }

    cmp = strcmp(a, b);
    if (strcmp(op, "=") == 0) {
        return same_word(a, b);
    }
    if (strcmp(op, "<>") == 0 || strcmp(op, "!=") == 0) {
        return !same_word(a, b);
    }
    if (strcmp(op, ">") == 0) {
        return cmp > 0;
    }
    if (strcmp(op, "<") == 0) {
        return cmp < 0;
    }
    if (strcmp(op, ">=") == 0) {
        return cmp >= 0;
    }
    if (strcmp(op, "<=") == 0) {
        return cmp <= 0;
    }
    return 0;
}

static int row_matches_atom(const char *row, const char *expr)
{
    char buf[MAX_LINE];
    char vals[MAX_FIELDS][MAX_VALUE + 1];
    char *field;
    char *value;
    char op[3];
    int ix;
    int i;

    if (expr == NULL || expr[0] == '\0') {
        return 1;
    }
    strncpy(buf, expr, sizeof(buf));
    buf[sizeof(buf) - 1] = '\0';
    trim(buf);
    if (starts_word(buf, "FOR")) {
        memmove(buf, buf + 3, strlen(buf + 3) + 1);
        trim(buf);
    }
    value = NULL;
    op[0] = '\0';
    for (i = 0; buf[i] != '\0'; i++) {
        if ((buf[i] == '<' || buf[i] == '>' || buf[i] == '!') &&
            buf[i + 1] == '=') {
            op[0] = buf[i];
            op[1] = '=';
            op[2] = '\0';
            buf[i] = '\0';
            value = buf + i + 2;
            break;
        }
        if (buf[i] == '<' && buf[i + 1] == '>') {
            strcpy(op, "<>");
            buf[i] = '\0';
            value = buf + i + 2;
            break;
        }
        if (buf[i] == '=' || buf[i] == '<' || buf[i] == '>') {
            op[0] = buf[i];
            op[1] = '\0';
            buf[i] = '\0';
            value = buf + i + 1;
            break;
        }
    }
    if (value == NULL) {
        say("? only FOR field op value is supported\n");
        return 0;
    }
    field = buf;
    trim(field);
    trim(value);
    upcase(field);
    ix = field_index(field);
    if (ix < 0) {
        say("? unknown field in FOR: %s\n", field);
        return 0;
    }
    row_values(row, vals);
    return compare_values(ix, vals[ix], op, value);
}

static int row_matches_for(const char *row, const char *expr)
{
    char buf[MAX_LINE];
    char *op;

    if (expr == NULL || expr[0] == '\0') {
        return 1;
    }
    strncpy(buf, expr, sizeof(buf));
    buf[sizeof(buf) - 1] = '\0';
    trim(buf);
    expand_vars(buf);
    if (starts_word(buf, "FOR")) {
        memmove(buf, buf + 3, strlen(buf + 3) + 1);
        trim(buf);
    }
    strip_outer_parens(buf);
    op = find_logic_op(buf, "OR");
    if (op != NULL) {
        char right[MAX_LINE];
        *op = '\0';
        strncpy(right, op + 2, sizeof(right));
        right[sizeof(right) - 1] = '\0';
        return row_matches_for(row, buf) || row_matches_for(row, right);
    }
    op = find_logic_op(buf, "AND");
    if (op != NULL) {
        char right[MAX_LINE];
        *op = '\0';
        strncpy(right, op + 3, sizeof(right));
        right[sizeof(right) - 1] = '\0';
        return row_matches_for(row, buf) && row_matches_for(row, right);
    }
    if (starts_word(buf, "NOT")) {
        memmove(buf, buf + 3, strlen(buf + 3) + 1);
        trim(buf);
        return !row_matches_for(row, buf);
    }
    return row_matches_atom(row, buf);
}

static void split_values(char *row, char vals[MAX_FIELDS][MAX_VALUE + 1])
{
    char *p;
    int i;
    for (i = 0; i < MAX_FIELDS; i++) {
        vals[i][0] = '\0';
    }
    p = strtok(row, "|");
    i = 0;
    while (p != NULL && i < g_table.field_count) {
        strncpy(vals[i], p, MAX_VALUE);
        vals[i][MAX_VALUE] = '\0';
        p = strtok(NULL, "|");
        i++;
    }
}

static void join_values(char vals[MAX_FIELDS][MAX_VALUE + 1],
                        char *out, int max, int deleted)
{
    int i;
    out[0] = deleted ? '*' : ' ';
    out[1] = '\0';
    for (i = 0; i < g_table.field_count; i++) {
        if (i > 0) {
            strncat(out, "|", max - (int)strlen(out) - 1);
        }
        strncat(out, vals[i], max - (int)strlen(out) - 1);
    }
}

static char *find_for_clause(char *args)
{
    char upper[MAX_LINE];
    char *p;

    strncpy(upper, args, sizeof(upper));
    upper[sizeof(upper) - 1] = '\0';
    upcase(upper);
    p = strstr(upper, " FOR ");
    if (p != NULL) {
        return args + (p - upper) + 1;
    }
    if (starts_word(upper, "FOR")) {
        return args;
    }
    return NULL;
}

static void make_index_prefix(char *out, const struct IndexDef *idx,
                              const char *value)
{
    char tmp[KEY_LEN + 1];
    char val[21];
    strncpy(val, value, 20);
    val[20] = '\0';
    upcase(val);
    memset(out, ' ', KEY_LEN);
    sprintf(tmp, "K|%-16.16s|%-16.16s|%-20.20s|",
            idx->table, idx->name, val);
    memcpy(out, tmp, strlen(tmp));
}

static void serialize_index_def(const struct IndexDef *idx, char *out, int max)
{
    snprintf(out, max, "%s|%s|%s", idx->table, idx->name, idx->field);
}

static int parse_index_def(const char *text, struct IndexDef *idx)
{
    char buf[DATA_LEN + 1];
    char *p;

    strncpy(buf, text, sizeof(buf));
    buf[sizeof(buf) - 1] = '\0';
    p = strtok(buf, "|");
    if (p == NULL) {
        return 0;
    }
    strncpy(idx->table, p, MAX_NAME);
    idx->table[MAX_NAME] = '\0';
    p = strtok(NULL, "|");
    if (p == NULL) {
        return 0;
    }
    strncpy(idx->name, p, MAX_NAME);
    idx->name[MAX_NAME] = '\0';
    p = strtok(NULL, "|");
    if (p == NULL) {
        return 0;
    }
    strncpy(idx->field, p, MAX_NAME);
    idx->field[MAX_NAME] = '\0';
    upcase(idx->table);
    upcase(idx->name);
    upcase(idx->field);
    return 1;
}

static int load_index_def(const char *name, struct IndexDef *idx)
{
    char key[KEY_LEN];
    char data[DATA_LEN + 1];
    char iname[MAX_NAME + 1];

    if (!require_table()) {
        return 0;
    }
    strncpy(iname, name, MAX_NAME);
    iname[MAX_NAME] = '\0';
    trim(iname);
    upcase(iname);
    make_name_key(key, "I", g_table.name, iname);
    if (!kv_get(key, data, sizeof(data))) {
        return 0;
    }
    return parse_index_def(data, idx);
}

static int write_index_entry(const struct IndexDef *idx, int seq,
                             const char *row)
{
    char vals[MAX_FIELDS][MAX_VALUE + 1];
    char key[KEY_LEN];
    char data[32];
    int ix;

    ix = field_index(idx->field);
    if (ix < 0 || row[0] == '*') {
        return 1;
    }
    row_values(row, vals);
    if (vals[ix][0] == '\0') {
        return 1;
    }
    make_index_key(key, idx, vals[ix], seq);
    sprintf(data, "%d", seq);
    return kv_put(key, data);
}

struct KeyCollectCtx {
    char (*keys)[KEY_LEN];
    int count;
    int max;
};

static int collect_key_cb(const char *key, const char *data, void *arg)
{
    struct KeyCollectCtx *ctx = (struct KeyCollectCtx *)arg;
    (void)data;
    if (ctx->count >= ctx->max) {
        return 0;
    }
    memcpy(ctx->keys[ctx->count], key, KEY_LEN);
    ctx->count++;
    return 1;
}

static int delete_prefix_keys(const char *prefix, int prefix_len)
{
    struct KeyCollectCtx ctx;
    int i;

    ctx.max = MAX_ROWS + 32;
    ctx.count = 0;
    ctx.keys = (char (*)[KEY_LEN])malloc(ctx.max * KEY_LEN);
    if (ctx.keys == NULL) {
        say("? no memory for index cleanup\n");
        return 0;
    }
    if (!kv_scan(prefix, prefix_len, collect_key_cb, &ctx)) {
        free(ctx.keys);
        return 0;
    }
    for (i = 0; i < ctx.count; i++) {
        if (!kv_delete(ctx.keys[i])) {
            free(ctx.keys);
            return 0;
        }
    }
    free(ctx.keys);
    return 1;
}

static int delete_index_entries(const struct IndexDef *idx)
{
    char prefix[KEY_LEN];
    char tmp[KEY_LEN + 1];

    memset(prefix, ' ', sizeof(prefix));
    sprintf(tmp, "K|%-16.16s|%-16.16s|", idx->table, idx->name);
    memcpy(prefix, tmp, strlen(tmp));
    return delete_prefix_keys(prefix, (int)strlen(tmp));
}

static int rebuild_index(const struct IndexDef *idx, int *entries)
{
    int count;
    int i;
    int built = 0;
    int ix;
    char row[DATA_LEN + 1];

    ix = field_index(idx->field);
    if (ix < 0) {
        say("? index field missing: %s\n", idx->field);
        return 0;
    }
    if (!delete_index_entries(idx)) {
        say("? index cleanup failed rc=%d\n", g_store_rc);
        return 0;
    }
    count = get_count(idx->table);
    for (i = 1; i <= count; i++) {
        if (read_row(i, row, sizeof(row)) && row[0] != '*') {
            char vals[MAX_FIELDS][MAX_VALUE + 1];
            row_values(row, vals);
            if (vals[ix][0] == '\0') {
                continue;
            }
            if (!write_index_entry(idx, i, row)) {
                say("? index write failed rc=%d\n", g_store_rc);
                return 0;
            }
            built++;
        }
    }
    if (entries != NULL) {
        *entries = built;
    }
    return 1;
}

struct ReindexCtx {
    int count;
    int entries;
    int ok;
    struct IndexDef indexes[MAX_INDEXES];
};

static int reindex_cb(const char *key, const char *data, void *arg)
{
    struct ReindexCtx *ctx = (struct ReindexCtx *)arg;
    (void)key;

    if (ctx->count >= MAX_INDEXES) {
        return 0;
    }
    if (!parse_index_def(data, &ctx->indexes[ctx->count])) {
        return 1;
    }
    ctx->count++;
    return 1;
}

static int rebuild_table_indexes(int verbose)
{
    char prefix[KEY_LEN];
    char tmp[KEY_LEN + 1];
    struct ReindexCtx ctx;

    if (!g_have_table) {
        return 1;
    }
    memset(prefix, ' ', sizeof(prefix));
    sprintf(tmp, "I|%-16.16s|", g_table.name);
    memcpy(prefix, tmp, strlen(tmp));
    ctx.count = 0;
    ctx.entries = 0;
    ctx.ok = 1;
    if (!kv_scan(prefix, (int)strlen(tmp), reindex_cb, &ctx)) {
        return 0;
    }
    {
        int i;
        for (i = 0; i < ctx.count; i++) {
            int entries = 0;
            if (!rebuild_index(&ctx.indexes[i], &entries)) {
                ctx.ok = 0;
                break;
            }
            ctx.entries += entries;
        }
    }
    if (verbose) {
        say("Reindexed %d index(es), %d entry(s)\n", ctx.count,
            ctx.entries);
    }
    return ctx.ok;
}

static void update_active_index(int seq, const char *row)
{
    (void)seq;
    (void)row;
    if (!rebuild_table_indexes(0)) {
        say("? index maintenance failed\n");
    }
}

static int read_row(int seq, char *row, int max)
{
    char key[KEY_LEN];
    make_key(key, "R", g_table.name, seq);
    return kv_get(key, row, max);
}

static int read_named_row(const char *table, int seq, char *row, int max)
{
    char key[KEY_LEN];
    make_key(key, "R", table, seq);
    return kv_get(key, row, max);
}

static int write_row(int seq, const char *row)
{
    char key[KEY_LEN];
    make_key(key, "R", g_table.name, seq);
    return kv_put(key, row);
}

static void cmd_help(void)
{
    say("Commands:\n");
    say("  CREATE name field type len (field type len ...)\n");
    say("  Types: C char, N numeric, D date, L logical\n");
    say("  TABLES\n");
    say("  SELECT n|alias, USE name (ALIAS alias)\n");
    say("  APPEND field=value (field=value ...)\n");
    say("  APPEND BLANK\n");
    say("  APPEND FROM ddname\n");
    say("  COPY TO ddname (ALL)\n");
    say("  LIST (ALL) (field-list) (FOR field=value)\n");
    say("  DISPLAY (ALL) (field-list), DISPLAY STRUCTURE, DISPLAY TABLES\n");
    say("  REPLACE recno field=value (field=value ...)\n");
    say("  REPLACE field WITH value (FOR field op value)\n");
    say("  DELETE (recno), DELETE ALL, DELETE FOR field op value\n");
    say("  RECALL (recno), RECALL ALL, RECALL FOR field op value\n");
    say("  PACK\n");
    say("  FIND text, LOCATE FOR field op value, CONTINUE\n");
    say("  SUM field (FOR field op value)\n");
    say("  AVERAGE field (FOR field op value)\n");
    say("  INDEX ON field TO name, INDEXES, REINDEX, SET INDEX TO name\n");
    say("  SEEK value\n");
    say("  SET RELATION TO field INTO table ON field\n");
    say("  SET RELATION OFF, RELATION\n");
    say("  SET FILTER TO expression, SET FILTER OFF\n");
    say("  SET DELETED ON|OFF, DISPLAY STATUS\n");
    say("  STORE value TO var, STORE var=value, ? expression\n");
    say("  DISPLAY MEMORY, LIST MEMORY\n");
    say("  DO ddname or dataset(member) with IF/ELSE/ENDIF and DO WHILE/ENDDO\n");
    say("  ZAP\n");
    say("  GO TOP|BOTTOM|recno, GOTO recno, SKIP (n)\n");
    say("  COUNT\n");
    say("  QUIT\n");
}

struct TablesCtx {
    int count;
};

static int tables_cb(const char *key, const char *data, void *arg)
{
    struct TablesCtx *ctx = (struct TablesCtx *)arg;
    struct Table t;
    (void)key;
    if (parse_table_def(data, &t)) {
        say("  %-16s %2d fields\n", t.name, t.field_count);
        ctx->count++;
    }
    return 1;
}

static void cmd_tables(void)
{
    char prefix[KEY_LEN];
    struct TablesCtx ctx;

    memset(prefix, ' ', sizeof(prefix));
    memcpy(prefix, "A|", 2);
    ctx.count = 0;
    say("Tables:\n");
    if (!kv_scan(prefix, 2, tables_cb, &ctx)) {
        say("? cannot scan VSAM store\n");
        return;
    }
    if (ctx.count == 0) {
        say("  none\n");
    }
}

struct IndexesCtx {
    int count;
};

static int indexes_cb(const char *key, const char *data, void *arg)
{
    struct IndexesCtx *ctx = (struct IndexesCtx *)arg;
    struct IndexDef idx;
    (void)key;
    if (parse_index_def(data, &idx)) {
        say("  %-16s ON %s\n", idx.name, idx.field);
        ctx->count++;
    }
    return 1;
}

static void cmd_indexes(void)
{
    char prefix[KEY_LEN];
    char tmp[KEY_LEN + 1];
    struct IndexesCtx ctx;

    if (!require_table()) {
        return;
    }
    memset(prefix, ' ', sizeof(prefix));
    sprintf(tmp, "I|%-16.16s|", g_table.name);
    memcpy(prefix, tmp, strlen(tmp));
    ctx.count = 0;
    say("Indexes for %s:\n", g_table.name);
    if (!kv_scan(prefix, (int)strlen(tmp), indexes_cb, &ctx)) {
        say("? cannot scan indexes\n");
        return;
    }
    if (ctx.count == 0) {
        say("  none\n");
    }
}

static void cmd_reindex(void)
{
    if (!require_table()) {
        return;
    }
    if (!rebuild_table_indexes(1)) {
        say("? reindex failed\n");
    }
}

static void cmd_index(char *args)
{
    struct IndexDef idx;
    char key[KEY_LEN];
    char data[DATA_LEN + 1];
    char field[MAX_NAME + 1];
    char name[MAX_NAME + 1];
    char *to;
    char upper[MAX_LINE];
    int ix;
    int entries = 0;

    if (!require_table()) {
        return;
    }
    trim(args);
    if (starts_word(args, "ON")) {
        memmove(args, args + 2, strlen(args + 2) + 1);
        trim(args);
    }
    strncpy(upper, args, sizeof(upper));
    upper[sizeof(upper) - 1] = '\0';
    upcase(upper);
    to = strstr(upper, " TO ");
    if (to == NULL) {
        say("? try INDEX ON field TO name\n");
        return;
    }
    args[to - upper] = '\0';
    strncpy(field, args, MAX_NAME);
    field[MAX_NAME] = '\0';
    trim(field);
    upcase(field);
    strncpy(name, args + (to - upper) + 4, MAX_NAME);
    name[MAX_NAME] = '\0';
    trim(name);
    upcase(name);
    ix = field_index(field);
    if (ix < 0 || name[0] == '\0') {
        say("? bad index definition\n");
        return;
    }
    memset(&idx, 0, sizeof(idx));
    strncpy(idx.table, g_table.name, MAX_NAME);
    strncpy(idx.name, name, MAX_NAME);
    strncpy(idx.field, field, MAX_NAME);
    serialize_index_def(&idx, data, sizeof(data));
    make_name_key(key, "I", idx.table, idx.name);
    if (!kv_put(key, data)) {
        say("? index metadata write failed rc=%d\n", g_store_rc);
        return;
    }
    if (!rebuild_index(&idx, &entries)) {
        return;
    }
    g_index = idx;
    g_have_index = 1;
    say("Index %s on %s built with %d entries\n", name, field, entries);
}

struct SeekCtx {
    struct IndexDef idx;
    char value[MAX_VALUE + 1];
    int found;
};

static int seek_cb(const char *key, const char *data, void *arg)
{
    struct SeekCtx *ctx = (struct SeekCtx *)arg;
    char row[DATA_LEN + 1];
    char vals[MAX_FIELDS][MAX_VALUE + 1];
    int seq = atoi(data);
    int ix;
    (void)key;

    if (seq < 1 || !read_row(seq, row, sizeof(row)) || row[0] == '*') {
        return 1;
    }
    ix = field_index(ctx->idx.field);
    if (ix < 0) {
        return 0;
    }
    row_values(row, vals);
    if (!same_word(vals[ix], ctx->value)) {
        return 1;
    }
    g_recno = seq;
    ctx->found = 1;
    print_header();
    print_row(seq, row);
    show_relation_for_parent(row);
    return 0;
}

static void cmd_set_index(char *args)
{
    char name[MAX_NAME + 1];

    trim(args);
    if (starts_word(args, "TO")) {
        memmove(args, args + 2, strlen(args + 2) + 1);
        trim(args);
    }
    if (same_word(args, "OFF") || args[0] == '\0') {
        g_have_index = 0;
        say("Index off\n");
        return;
    }
    strncpy(name, args, MAX_NAME);
    name[MAX_NAME] = '\0';
    trim(name);
    upcase(name);
    if (!load_index_def(name, &g_index)) {
        say("? index not found: %s\n", name);
        return;
    }
    g_have_index = 1;
    say("Index %s active on %s\n", g_index.name, g_index.field);
}

static void cmd_set_filter(char *args)
{
    trim(args);
    if (starts_word(args, "TO")) {
        memmove(args, args + 2, strlen(args + 2) + 1);
        trim(args);
    }
    if (args[0] == '\0' || same_word(args, "OFF")) {
        g_filter[0] = '\0';
        save_area();
        say("Filter off\n");
        return;
    }
    strncpy(g_filter, args, sizeof(g_filter));
    g_filter[sizeof(g_filter) - 1] = '\0';
    trim(g_filter);
    save_area();
    say("Filter set to %s\n", g_filter);
}

static void cmd_set_deleted(char *args)
{
    trim(args);
    if (same_word(args, "ON")) {
        g_deleted_on = 1;
    } else if (same_word(args, "OFF")) {
        g_deleted_on = 0;
    } else {
        say("? try SET DELETED ON or SET DELETED OFF\n");
        return;
    }
    save_area();
    say("Deleted %s\n", g_deleted_on ? "on" : "off");
}

static void cmd_status(void)
{
    int i;
    say("Work areas:\n");
    for (i = 0; i < MAX_WORK_AREAS; i++) {
        struct WorkArea *a = &g_areas[i];
        say("%c %2d ", i == g_area ? '*' : ' ', i + 1);
        if (a->have_table) {
            say("%-16s alias %-16s recno %d",
                a->table.name, a->alias[0] ? a->alias : a->table.name,
                a->recno);
            if (a->filter[0] != '\0') {
                say(" filter %s", a->filter);
            }
            say(" deleted %s", a->deleted_on ? "ON" : "OFF");
        } else {
            say("empty");
        }
        say("\n");
    }
}

static void cmd_seek(char *args)
{
    char prefix[KEY_LEN];
    struct SeekCtx ctx;
    int prefix_len;

    if (!require_table()) {
        return;
    }
    if (!g_have_index) {
        say("? no active index; use SET INDEX TO name\n");
        return;
    }
    trim(args);
    if (args[0] == '\0') {
        say("? seek value missing\n");
        return;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.idx = g_index;
    strncpy(ctx.value, args, MAX_VALUE);
    ctx.value[MAX_VALUE] = '\0';
    trim(ctx.value);
    make_index_prefix(prefix, &g_index, ctx.value);
    prefix_len = 57;
    if (!kv_scan(prefix, prefix_len, seek_cb, &ctx)) {
        say("? seek failed\n");
        return;
    }
    if (!ctx.found) {
        say("? not found\n");
    }
}

static void cmd_set_relation(char *args)
{
    char parent[MAX_NAME + 1];
    char child[MAX_NAME + 1];
    char child_field[MAX_NAME + 1];
    char upper[MAX_LINE];
    char *into;
    char *on;
    struct Table child_def;

    if (!require_table()) {
        return;
    }
    trim(args);
    if (same_word(args, "OFF")) {
        memset(&g_relation, 0, sizeof(g_relation));
        say("Relation off\n");
        return;
    }
    if (starts_word(args, "TO")) {
        memmove(args, args + 2, strlen(args + 2) + 1);
        trim(args);
    }
    strncpy(upper, args, sizeof(upper));
    upper[sizeof(upper) - 1] = '\0';
    upcase(upper);
    into = strstr(upper, " INTO ");
    on = strstr(upper, " ON ");
    if (into == NULL || on == NULL || on < into) {
        say("? try SET RELATION TO field INTO table ON field\n");
        return;
    }
    args[into - upper] = '\0';
    args[on - upper] = '\0';
    strncpy(parent, args, MAX_NAME);
    parent[MAX_NAME] = '\0';
    strncpy(child, args + (into - upper) + 6, MAX_NAME);
    child[MAX_NAME] = '\0';
    strncpy(child_field, args + (on - upper) + 4, MAX_NAME);
    child_field[MAX_NAME] = '\0';
    trim(parent);
    trim(child);
    trim(child_field);
    upcase(parent);
    upcase(child);
    upcase(child_field);
    if (field_index(parent) < 0) {
        say("? parent field not found: %s\n", parent);
        return;
    }
    if (!load_table_def(child, &child_def)) {
        say("? child table not found: %s\n", child);
        return;
    }
    if (field_index_for(&child_def, child_field) < 0) {
        say("? child field not found: %s\n", child_field);
        return;
    }
    memset(&g_relation, 0, sizeof(g_relation));
    g_relation.active = 1;
    strncpy(g_relation.parent_table, g_table.name, MAX_NAME);
    strncpy(g_relation.parent_field, parent, MAX_NAME);
    strncpy(g_relation.child_table, child, MAX_NAME);
    strncpy(g_relation.child_field, child_field, MAX_NAME);
    say("Relation %s.%s -> %s.%s active\n", g_relation.parent_table,
        g_relation.parent_field, g_relation.child_table,
        g_relation.child_field);
}

static void cmd_relation(void)
{
    if (!g_relation.active) {
        say("No active relation\n");
        return;
    }
    say("Relation %s.%s -> %s.%s\n", g_relation.parent_table,
        g_relation.parent_field, g_relation.child_table,
        g_relation.child_field);
}

static void cmd_create(char *args)
{
    struct Table t;
    char *tokens[64];
    char *tok;
    char key[KEY_LEN];
    char data[DATA_LEN + 1];
    int ntok = 0;
    int pos = 0;

    memset(&t, 0, sizeof(t));
    tok = strtok(args, " ");
    while (tok != NULL && ntok < (int)(sizeof(tokens) / sizeof(tokens[0]))) {
        tokens[ntok++] = tok;
        tok = strtok(NULL, " ");
    }
    if (ntok < 1) {
        say("? table name missing\n");
        return;
    }
    strncpy(t.name, tokens[pos++], MAX_NAME);
    t.name[MAX_NAME] = '\0';
    upcase(t.name);
    while (pos < ntok) {
        char fname[MAX_NAME + 1];
        char ftype = TYPE_CHAR;
        int flen;
        if (t.field_count >= MAX_FIELDS) {
            say("? too many fields\n");
            return;
        }
        strncpy(fname, tokens[pos++], MAX_NAME);
        fname[MAX_NAME] = '\0';
        upcase(fname);
        if (pos >= ntok) {
            say("? length missing for %s\n", fname);
            return;
        }
        if (strlen(tokens[pos]) == 1 &&
            strchr("CcNnDdLl", tokens[pos][0]) != NULL) {
            ftype = (char)toupper((unsigned char)tokens[pos][0]);
            pos++;
            if (ftype == TYPE_LOGICAL) {
                flen = 1;
                if (pos < ntok && isdigit((unsigned char)tokens[pos][0])) {
                    pos++;
                }
            } else if (ftype == TYPE_DATE && (pos >= ntok ||
                       !isdigit((unsigned char)tokens[pos][0]))) {
                flen = 8;
            } else {
                if (pos >= ntok) {
                    say("? length missing for %s\n", fname);
                    return;
                }
                flen = atoi(tokens[pos++]);
            }
        } else {
            flen = atoi(tokens[pos++]);
        }
        strncpy(t.fields[t.field_count].name, fname, MAX_NAME);
        t.fields[t.field_count].type = ftype;
        t.fields[t.field_count].len = flen;
        if (t.fields[t.field_count].len < 1 ||
            t.fields[t.field_count].len > MAX_VALUE) {
            say("? invalid length for %s\n", fname);
            return;
        }
        t.field_count++;
    }
    if (t.field_count < 1) {
        say("? define at least one field\n");
        return;
    }
    serialize_table_def(&t, data, sizeof(data));
    make_key(key, "T", t.name, -1);
    if (!kv_put(key, data)) {
        say("? store write failed; is DBASEV allocated? rc=%d\n", g_store_rc);
        return;
    }
    g_table = t;
    g_have_table = 1;
    g_recno = 0;
    strncpy(g_areas[g_area].alias, t.name, MAX_NAME);
    g_areas[g_area].alias[MAX_NAME] = '\0';
    save_area();
    say("Table %s created with %d fields\n", t.name, t.field_count);
}

static void cmd_use(char *args)
{
    char work[MAX_LINE];
    char upper[MAX_LINE];
    char name[MAX_NAME + 1];
    char alias[MAX_NAME + 1];
    char *p;
    char *aliasp;

    strncpy(work, args, sizeof(work));
    work[sizeof(work) - 1] = '\0';
    trim(work);
    alias[0] = '\0';
    strncpy(upper, work, sizeof(upper));
    upper[sizeof(upper) - 1] = '\0';
    upcase(upper);
    aliasp = strstr(upper, " ALIAS ");
    if (aliasp != NULL) {
        work[aliasp - upper] = '\0';
        strncpy(alias, work + (aliasp - upper) + 7, MAX_NAME);
        alias[MAX_NAME] = '\0';
        trim(alias);
        upcase(alias);
    }
    p = strchr(work, ' ');
    if (p != NULL) {
        *p = '\0';
    }
    strncpy(name, work, MAX_NAME);
    name[MAX_NAME] = '\0';
    trim(name);
    trim(args);
    if (load_table(name)) {
        if (alias[0] == '\0') {
            strncpy(alias, g_table.name, MAX_NAME);
            alias[MAX_NAME] = '\0';
        }
        strncpy(g_areas[g_area].alias, alias, MAX_NAME);
        g_areas[g_area].alias[MAX_NAME] = '\0';
        save_area();
        say("Using %s in work area %d alias %s\n", g_table.name,
            g_area + 1, current_alias());
    } else {
        say("? table not found: %s\n", name);
    }
}

static void cmd_select(char *args)
{
    char target[MAX_NAME + 1];
    int i;
    int n;

    trim(args);
    if (args[0] == '\0') {
        say("Work area %d", g_area + 1);
        if (g_have_table) {
            say(" %s alias %s", g_table.name, current_alias());
        }
        say("\n");
        return;
    }
    strncpy(target, args, MAX_NAME);
    target[MAX_NAME] = '\0';
    trim(target);
    upcase(target);
    if (isdigit((unsigned char)target[0])) {
        n = atoi(target);
        if (n < 1 || n > MAX_WORK_AREAS) {
            say("? work area must be 1 to %d\n", MAX_WORK_AREAS);
            return;
        }
        save_area();
        g_area = n - 1;
        load_area();
    } else {
        for (i = 0; i < MAX_WORK_AREAS; i++) {
            if (g_areas[i].alias[0] != '\0' &&
                same_word(g_areas[i].alias, target)) {
                save_area();
                g_area = i;
                load_area();
                break;
            }
        }
        if (i >= MAX_WORK_AREAS) {
            say("? alias not found: %s\n", target);
            return;
        }
    }
    say("Selected work area %d", g_area + 1);
    if (g_have_table) {
        say(" %s alias %s", g_table.name, current_alias());
    }
    say("\n");
}

static int require_table(void)
{
    if (!g_have_table) {
        say("? no table selected; use CREATE or USE\n");
        return 0;
    }
    return 1;
}

static int apply_assignments(char *args, char vals[MAX_FIELDS][MAX_VALUE + 1])
{
    char *tok;
    while ((tok = strtok(args, " ")) != NULL) {
        char *eq = strchr(tok, '=');
        int ix;
        args = NULL;
        if (eq == NULL) {
            say("? expected field=value: %s\n", tok);
            return 0;
        }
        *eq = '\0';
        upcase(tok);
        ix = field_index(tok);
        if (ix < 0) {
            say("? unknown field: %s\n", tok);
            return 0;
        }
        if (!validate_value(ix, eq + 1)) {
            return 0;
        }
        strncpy(vals[ix], eq + 1, g_table.fields[ix].len);
        vals[ix][g_table.fields[ix].len] = '\0';
    }
    return 1;
}

static void cmd_append(char *args)
{
    char vals[MAX_FIELDS][MAX_VALUE + 1];
    char row[DATA_LEN + 1];
    int i;
    int count;

    if (!require_table()) {
        return;
    }
    for (i = 0; i < MAX_FIELDS; i++) {
        vals[i][0] = '\0';
    }
    trim(args);
    if (same_word(args, "BLANK")) {
        count = get_count(g_table.name);
        if (count >= MAX_ROWS) {
            say("? table full\n");
            return;
        }
        join_values(vals, row, sizeof(row), 0);
        if (!write_row(count + 1, row)) {
            say("? append failed rc=%d\n", g_store_rc);
            return;
        }
        g_recno = count + 1;
        update_active_index(g_recno, row);
        say("Blank record %d added\n", count + 1);
        return;
    }
    if (!apply_assignments(args, vals)) {
        return;
    }
    count = get_count(g_table.name);
    if (count >= MAX_ROWS) {
        say("? table full\n");
        return;
    }
    join_values(vals, row, sizeof(row), 0);
    if (!write_row(count + 1, row)) {
        say("? append failed rc=%d\n", g_store_rc);
        return;
    }
    g_recno = count + 1;
    update_active_index(g_recno, row);
    say("Record %d added\n", count + 1);
}

static void print_header(void)
{
    int i;
    say("RECNO ");
    for (i = 0; i < g_table.field_count; i++) {
        say("%-*s ", g_table.fields[i].len, g_table.fields[i].name);
    }
    say("\n");
}

static void print_row(int seq, const char *row)
{
    char tmp[DATA_LEN + 1];
    char vals[MAX_FIELDS][MAX_VALUE + 1];
    int i;
    strncpy(tmp, row + 1, sizeof(tmp));
    tmp[sizeof(tmp) - 1] = '\0';
    split_values(tmp, vals);
    say("%5d ", seq);
    for (i = 0; i < g_table.field_count; i++) {
        say("%-*s ", g_table.fields[i].len, vals[i]);
    }
    say("\n");
}

static int parse_field_list(char *text, int fields[MAX_FIELDS], int *count)
{
    char *tok;
    *count = 0;
    trim(text);
    if (text[0] == '\0') {
        return 1;
    }
    for (tok = strtok(text, " ,"); tok != NULL; tok = strtok(NULL, " ,")) {
        int ix;
        if (*count >= MAX_FIELDS) {
            say("? too many fields in list\n");
            return 0;
        }
        ix = field_index(tok);
        if (ix < 0) {
            say("? unknown field: %s\n", tok);
            return 0;
        }
        fields[*count] = ix;
        (*count)++;
    }
    return 1;
}

static void print_header_fields(const int fields[MAX_FIELDS], int field_count)
{
    int i;
    if (field_count == 0) {
        print_header();
        return;
    }
    say("RECNO ");
    for (i = 0; i < field_count; i++) {
        int ix = fields[i];
        say("%-*s ", g_table.fields[ix].len, g_table.fields[ix].name);
    }
    say("\n");
}

static void print_row_fields(int seq, const char *row,
                             const int fields[MAX_FIELDS], int field_count)
{
    char tmp[DATA_LEN + 1];
    char vals[MAX_FIELDS][MAX_VALUE + 1];
    int i;
    if (field_count == 0) {
        print_row(seq, row);
        return;
    }
    strncpy(tmp, row + 1, sizeof(tmp));
    tmp[sizeof(tmp) - 1] = '\0';
    split_values(tmp, vals);
    say("%5d ", seq);
    for (i = 0; i < field_count; i++) {
        int ix = fields[i];
        say("%-*s ", g_table.fields[ix].len, vals[ix]);
    }
    say("\n");
}

static void print_row_for(const struct Table *t, int seq, const char *row)
{
    char tmp[DATA_LEN + 1];
    char vals[MAX_FIELDS][MAX_VALUE + 1];
    int i;
    strncpy(tmp, row + 1, sizeof(tmp));
    tmp[sizeof(tmp) - 1] = '\0';
    split_values_for(t, tmp, vals);
    say("%5d ", seq);
    for (i = 0; i < t->field_count; i++) {
        say("%-*s ", t->fields[i].len, vals[i]);
    }
    say("\n");
}

static void print_header_for(const struct Table *t)
{
    int i;
    say("RECNO ");
    for (i = 0; i < t->field_count; i++) {
        say("%-*s ", t->fields[i].len, t->fields[i].name);
    }
    say("\n");
}

static void show_relation_for_parent(const char *parent_row)
{
    struct Table child;
    char parent_vals[MAX_FIELDS][MAX_VALUE + 1];
    char child_vals[MAX_FIELDS][MAX_VALUE + 1];
    char row[DATA_LEN + 1];
    char body[DATA_LEN + 1];
    int parent_ix;
    int child_ix;
    int i;
    int count;

    if (!g_relation.active || !same_word(g_relation.parent_table,
                                         g_table.name)) {
        return;
    }
    if (!load_table_def(g_relation.child_table, &child)) {
        say("? related table not found: %s\n", g_relation.child_table);
        return;
    }
    parent_ix = field_index(g_relation.parent_field);
    child_ix = field_index_for(&child, g_relation.child_field);
    if (parent_ix < 0 || child_ix < 0) {
        say("? relation field missing\n");
        return;
    }
    row_values(parent_row, parent_vals);
    count = get_count(child.name);
    for (i = 1; i <= count; i++) {
        if (!read_named_row(child.name, i, row, sizeof(row)) ||
            row[0] == '*') {
            continue;
        }
        strncpy(body, row + 1, sizeof(body));
        body[sizeof(body) - 1] = '\0';
        split_values_for(&child, body, child_vals);
        if (same_word(parent_vals[parent_ix], child_vals[child_ix])) {
            say("Related %s:\n", child.name);
            print_header_for(&child);
            print_row_for(&child, i, row);
            return;
        }
    }
    say("Related %s: not found\n", child.name);
}

static void cmd_list(char *args)
{
    int i;
    int count;
    int all = 0;
    char *forp;
    char field_text[MAX_LINE];
    int fields[MAX_FIELDS];
    int field_count = 0;
    char args_upper[MAX_LINE];
    char row[DATA_LEN + 1];

    if (!require_table()) {
        return;
    }
    trim(args);
    strncpy(args_upper, args, sizeof(args_upper));
    args_upper[sizeof(args_upper) - 1] = '\0';
    upcase(args_upper);
    if (starts_word(args_upper, "ALL")) {
        all = 1;
        memmove(args, args + 3, strlen(args + 3) + 1);
        trim(args);
    }
    strncpy(args_upper, args, sizeof(args_upper));
    args_upper[sizeof(args_upper) - 1] = '\0';
    upcase(args_upper);
    forp = strstr(args_upper, " FOR ");
    if (forp != NULL) {
        forp = args + (forp - args_upper) + 1;
    } else if (starts_word(args_upper, "FOR")) {
        forp = args;
    } else {
        forp = NULL;
    }
    if (forp != NULL) {
        char *cut = forp;
        while (cut > args && isspace((unsigned char)cut[-1])) {
            cut--;
        }
        *cut = '\0';
    }
    strncpy(field_text, args, sizeof(field_text));
    field_text[sizeof(field_text) - 1] = '\0';
    trim(field_text);
    if (!parse_field_list(field_text, fields, &field_count)) {
        return;
    }
    count = get_count(g_table.name);
    print_header_fields(fields, field_count);
    for (i = 1; i <= count; i++) {
        if (read_row(i, row, sizeof(row)) && row_visible(row, all, forp)) {
            print_row_fields(i, row, fields, field_count);
        }
    }
}

static void show_current(void)
{
    char row[DATA_LEN + 1];
    int count;
    int fields[MAX_FIELDS];

    if (!require_table()) {
        return;
    }
    count = get_count(g_table.name);
    if (g_recno < 1 || g_recno > count ||
        !read_row(g_recno, row, sizeof(row))) {
        say("? record pointer is out of range\n");
        return;
    }
    if (!row_visible(row, 0, NULL)) {
        say("? current record is hidden by SET DELETED or SET FILTER\n");
        return;
    }
    print_header_fields(fields, 0);
    print_row_fields(g_recno, row, fields, 0);
    show_relation_for_parent(row);
}

static void cmd_display_records(char *args)
{
    char args_upper[MAX_LINE];
    char field_text[MAX_LINE];
    char row[DATA_LEN + 1];
    char *forp;
    int fields[MAX_FIELDS];
    int field_count = 0;
    int all = 0;
    int count;
    int i;

    if (!require_table()) {
        return;
    }
    trim(args);
    strncpy(args_upper, args, sizeof(args_upper));
    args_upper[sizeof(args_upper) - 1] = '\0';
    upcase(args_upper);
    if (starts_word(args_upper, "ALL")) {
        all = 1;
        memmove(args, args + 3, strlen(args + 3) + 1);
        trim(args);
    }
    strncpy(args_upper, args, sizeof(args_upper));
    args_upper[sizeof(args_upper) - 1] = '\0';
    upcase(args_upper);
    forp = strstr(args_upper, " FOR ");
    if (forp != NULL) {
        forp = args + (forp - args_upper) + 1;
    } else if (starts_word(args_upper, "FOR")) {
        forp = args;
    } else {
        forp = NULL;
    }
    if (forp != NULL) {
        char *cut = forp;
        while (cut > args && isspace((unsigned char)cut[-1])) {
            cut--;
        }
        *cut = '\0';
    }
    strncpy(field_text, args, sizeof(field_text));
    field_text[sizeof(field_text) - 1] = '\0';
    trim(field_text);
    if (!parse_field_list(field_text, fields, &field_count)) {
        return;
    }
    if (all || forp != NULL) {
        count = get_count(g_table.name);
        print_header_fields(fields, field_count);
        for (i = 1; i <= count; i++) {
            if (read_row(i, row, sizeof(row)) && row_visible(row, all, forp)) {
                print_row_fields(i, row, fields, field_count);
            }
        }
        return;
    }
    count = get_count(g_table.name);
    if (g_recno < 1 || g_recno > count ||
        !read_row(g_recno, row, sizeof(row))) {
        say("? record pointer is out of range\n");
        return;
    }
    if (!row_visible(row, 0, NULL)) {
        say("? current record is hidden by SET DELETED or SET FILTER\n");
        return;
    }
    print_header_fields(fields, field_count);
    print_row_fields(g_recno, row, fields, field_count);
    show_relation_for_parent(row);
}

static void cmd_go(char *args)
{
    int count;
    if (!require_table()) {
        return;
    }
    trim(args);
    count = get_count(g_table.name);
    if (same_word(args, "TOP")) {
        g_recno = count > 0 ? 1 : 0;
    } else if (same_word(args, "BOTTOM")) {
        g_recno = count;
    } else {
        g_recno = atoi(args);
    }
    if (g_recno < 1 || g_recno > count) {
        say("? record number out of range\n");
        return;
    }
    show_current();
}

static void cmd_skip(char *args)
{
    int n;
    int count;
    if (!require_table()) {
        return;
    }
    trim(args);
    n = args[0] ? atoi(args) : 1;
    count = get_count(g_table.name);
    if (g_recno == 0 && count > 0) {
        g_recno = 1;
    } else {
        g_recno += n;
    }
    if (g_recno < 1) {
        g_recno = 1;
    }
    if (g_recno > count) {
        g_recno = count;
    }
    show_current();
}

static void cmd_structure(void)
{
    int i;
    if (!require_table()) {
        return;
    }
    say("Table: %s\n", g_table.name);
    say("Fields:\n");
    for (i = 0; i < g_table.field_count; i++) {
        say("  %-16s %c %d\n", g_table.fields[i].name,
            g_table.fields[i].type, g_table.fields[i].len);
    }
}

static void cmd_replace(char *args)
{
    char *recno;
    char *assigns;
    char *forp;
    char *work;
    char assignbuf[MAX_LINE];
    char row[DATA_LEN + 1];
    char body[DATA_LEN + 1];
    char vals[MAX_FIELDS][MAX_VALUE + 1];
    int seq;
    int count;
    int changed = 0;
    int all = 0;

    if (!require_table()) {
        return;
    }
    trim(args);
    count = get_count(g_table.name);
    if (args[0] != '\0' && !isdigit((unsigned char)args[0])) {
        forp = find_for_clause(args);
        if (forp != NULL) {
            char *cut = forp;
            while (cut > args && isspace((unsigned char)cut[-1])) {
                cut--;
            }
            *cut = '\0';
        }
        all = starts_word(args, "ALL");
        if (all) {
            memmove(args, args + 3, strlen(args + 3) + 1);
            trim(args);
        }
        work = args;
        {
            char upper[MAX_LINE];
            char *withp;
            strncpy(upper, work, sizeof(upper));
            upper[sizeof(upper) - 1] = '\0';
            upcase(upper);
            withp = strstr(upper, " WITH ");
            if (withp == NULL) {
                say("? expected field WITH value\n");
                return;
            }
            work[withp - upper] = '\0';
            assigns = work + (withp - upper) + 6;
            trim(work);
            trim(assigns);
            snprintf(assignbuf, sizeof(assignbuf), "%s=%s", work, assigns);
        }
        for (seq = all || forp != NULL ? 1 : g_recno;
             seq <= count; seq++) {
            if (!read_row(seq, row, sizeof(row)) || row[0] == '*') {
                if (!all && forp == NULL) {
                    break;
                }
                continue;
            }
            if (!row_visible(row, 0, forp)) {
                if (!all && forp == NULL) {
                    break;
                }
                continue;
            }
            row_values(row, vals);
            {
                char one_assign[MAX_LINE];
                strncpy(one_assign, assignbuf, sizeof(one_assign));
                one_assign[sizeof(one_assign) - 1] = '\0';
                if (!apply_assignments(one_assign, vals)) {
                    return;
                }
            }
            join_values(vals, row, sizeof(row), 0);
            if (!write_row(seq, row)) {
                say("? replace failed\n");
                return;
            }
            changed++;
            g_recno = seq;
            update_active_index(seq, row);
            if (!all && forp == NULL) {
                break;
            }
        }
        say("%d record(s) replaced\n", changed);
        return;
    }

    recno = args;
    assigns = strchr(args, ' ');
    if (assigns != NULL) {
        *assigns = '\0';
        assigns++;
        trim(assigns);
    }
    if (recno == NULL) {
        say("? recno missing\n");
        return;
    }
    if (assigns == NULL || assigns[0] == '\0') {
        say("? replacement field=value missing\n");
        return;
    }
    seq = atoi(recno);
    if (seq < 1 || !read_row(seq, row, sizeof(row)) || row[0] == '*') {
        say("? record not found\n");
        return;
    }
    strncpy(body, row + 1, sizeof(body));
    body[sizeof(body) - 1] = '\0';
    split_values(body, vals);
    if (!apply_assignments(assigns, vals)) {
        return;
    }
    join_values(vals, row, sizeof(row), 0);
    if (!write_row(seq, row)) {
        say("? replace failed\n");
        return;
    }
    g_recno = seq;
    update_active_index(seq, row);
    say("Record %d replaced\n", seq);
}

static void cmd_delete(char *args)
{
    int seq;
    int count;
    int changed = 0;
    int all = 0;
    char *forp;
    char row[DATA_LEN + 1];
    if (!require_table()) {
        return;
    }
    trim(args);
    count = get_count(g_table.name);
    forp = find_for_clause(args);
    all = same_word(args, "ALL") || forp != NULL;
    if (all) {
        for (seq = 1; seq <= count; seq++) {
            if (read_row(seq, row, sizeof(row)) && row[0] != '*' &&
                row_visible(row, 1, forp)) {
                row[0] = '*';
                if (!write_row(seq, row)) {
                    say("? delete failed\n");
                    return;
                }
                changed++;
                g_recno = seq;
            }
        }
        update_active_index(0, NULL);
        say("%d record(s) deleted\n", changed);
        return;
    }
    seq = args[0] ? atoi(args) : g_recno;
    if (seq < 1 || seq > count || !read_row(seq, row, sizeof(row)) ||
        row[0] == '*') {
        say("? record not found\n");
        return;
    }
    row[0] = '*';
    if (!write_row(seq, row)) {
        say("? delete failed\n");
        return;
    }
    g_recno = seq;
    update_active_index(0, NULL);
    say("Record %d deleted\n", seq);
}

static void cmd_recall(char *args)
{
    int seq;
    int count;
    int changed = 0;
    int all = 0;
    char *forp;
    char row[DATA_LEN + 1];
    if (!require_table()) {
        return;
    }
    trim(args);
    count = get_count(g_table.name);
    forp = find_for_clause(args);
    all = same_word(args, "ALL") || forp != NULL;
    if (all) {
        for (seq = 1; seq <= count; seq++) {
            if (read_row(seq, row, sizeof(row)) && row[0] == '*' &&
                (g_filter[0] == '\0' || row_matches_for(row, g_filter)) &&
                row_matches_for(row, forp)) {
                row[0] = ' ';
                if (!write_row(seq, row)) {
                    say("? recall failed\n");
                    return;
                }
                changed++;
                g_recno = seq;
            }
        }
        update_active_index(0, NULL);
        say("%d record(s) recalled\n", changed);
        return;
    }
    seq = args[0] ? atoi(args) : g_recno;
    if (seq < 1 || seq > count || !read_row(seq, row, sizeof(row)) ||
        row[0] != '*') {
        say("? deleted record not found\n");
        return;
    }
    row[0] = ' ';
    if (!write_row(seq, row)) {
        say("? recall failed\n");
        return;
    }
    g_recno = seq;
    update_active_index(0, NULL);
    say("Record %d recalled\n", seq);
}

static void cmd_count(void)
{
    int i;
    int count;
    int active = 0;
    char row[DATA_LEN + 1];
    if (!require_table()) {
        return;
    }
    count = get_count(g_table.name);
    for (i = 1; i <= count; i++) {
        if (read_row(i, row, sizeof(row)) && row_visible(row, 0, NULL)) {
            active++;
        }
    }
    say("%d active records (%d physical)\n", active, count);
}

static void cmd_pack(void)
{
    int src;
    int dst = 0;
    int count;
    char row[DATA_LEN + 1];
    if (!require_table()) {
        return;
    }
    count = get_count(g_table.name);
    for (src = 1; src <= count; src++) {
        if (read_row(src, row, sizeof(row)) && row[0] != '*') {
            dst++;
            if (dst != src && !write_row(dst, row)) {
                say("? pack failed\n");
                return;
            }
        }
    }
    for (src = dst + 1; src <= count; src++) {
        char key[KEY_LEN];
        make_key(key, "R", g_table.name, src);
        if (!kv_delete(key)) {
            say("? pack cleanup failed rc=%d\n", g_store_rc);
            return;
        }
    }
    update_active_index(0, NULL);
    say("Packed %s: %d active records\n", g_table.name, dst);
}

static void cmd_find(char *args)
{
    int i;
    int count;
    int shown = 0;
    char row[DATA_LEN + 1];
    char hay[DATA_LEN + 1];
    char needle[MAX_VALUE + 1];
    if (!require_table()) {
        return;
    }
    trim(args);
    strncpy(needle, args, MAX_VALUE);
    needle[MAX_VALUE] = '\0';
    upcase(needle);
    count = get_count(g_table.name);
    print_header();
    for (i = 1; i <= count; i++) {
        if (read_row(i, row, sizeof(row)) && row_visible(row, 0, NULL)) {
            strncpy(hay, row, sizeof(hay));
            hay[sizeof(hay) - 1] = '\0';
            upcase(hay);
            if (strstr(hay, needle) != NULL) {
                print_row(i, row);
                shown++;
            }
        }
    }
    say("%d found\n", shown);
}

static int locate_from(int start, const char *expr)
{
    int i;
    int count;
    char row[DATA_LEN + 1];

    count = get_count(g_table.name);
    for (i = start; i <= count; i++) {
        if (read_row(i, row, sizeof(row)) && row_visible(row, 0, expr)) {
            g_recno = i;
            print_header();
            print_row(i, row);
            show_relation_for_parent(row);
            return 1;
        }
    }
    say("? not found\n");
    return 0;
}

static void cmd_locate(char *args)
{
    char *forp;

    if (!require_table()) {
        return;
    }
    trim(args);
    forp = find_for_clause(args);
    if (forp == NULL) {
        say("? try LOCATE FOR field op value\n");
        return;
    }
    strncpy(g_locate_for, forp, sizeof(g_locate_for));
    g_locate_for[sizeof(g_locate_for) - 1] = '\0';
    g_locate_recno = 1;
    if (locate_from(1, g_locate_for)) {
        g_locate_recno = g_recno + 1;
    }
}

static void cmd_continue(void)
{
    if (!require_table()) {
        return;
    }
    if (g_locate_for[0] == '\0') {
        say("? no active LOCATE\n");
        return;
    }
    if (locate_from(g_locate_recno, g_locate_for)) {
        g_locate_recno = g_recno + 1;
    }
}

static void cmd_sum(char *args, int average)
{
    char field[MAX_NAME + 1];
    char *forp;
    char *p;
    int ix;
    int i;
    int count;
    int used = 0;
    double total = 0.0;
    char row[DATA_LEN + 1];
    char vals[MAX_FIELDS][MAX_VALUE + 1];

    if (!require_table()) {
        return;
    }
    trim(args);
    forp = find_for_clause(args);
    if (forp != NULL) {
        char *cut = forp;
        while (cut > args && isspace((unsigned char)cut[-1])) {
            cut--;
        }
        *cut = '\0';
    }
    p = strchr(args, ' ');
    if (p != NULL) {
        *p = '\0';
    }
    strncpy(field, args, MAX_NAME);
    field[MAX_NAME] = '\0';
    trim(field);
    upcase(field);
    ix = field_index(field);
    if (ix < 0) {
        say("? unknown field: %s\n", field);
        return;
    }
    if (g_table.fields[ix].type != TYPE_NUM) {
        say("? %s is not numeric\n", field);
        return;
    }
    count = get_count(g_table.name);
    for (i = 1; i <= count; i++) {
        if (read_row(i, row, sizeof(row)) && row_visible(row, 0, forp)) {
            row_values(row, vals);
            if (vals[ix][0] != '\0') {
                total += atof(vals[ix]);
                used++;
            }
        }
    }
    if (average) {
        if (used == 0) {
            say("Average %s = 0\n", field);
        } else {
            say("Average %s = %.2f\n", field, total / used);
        }
    } else {
        say("Sum %s = %.2f\n", field, total);
    }
}

static void cmd_zap(void)
{
    int i;
    int count;
    int deleted = 0;
    char key[KEY_LEN];

    if (!require_table()) {
        return;
    }
    count = get_count(g_table.name);
    for (i = 1; i <= count; i++) {
        make_key(key, "R", g_table.name, i);
        if (!kv_delete(key)) {
            say("? zap failed rc=%d\n", g_store_rc);
            return;
        }
        deleted++;
    }
    g_recno = 0;
    update_active_index(0, NULL);
    say("Zapped %d record(s) from %s\n", deleted, g_table.name);
}

static void cmd_append_from(char *args)
{
    FILE *f;
    char dd[MAX_NAME + 1];
    char line[DATA_LEN + 1];
    int added = 0;

    if (!require_table()) {
        return;
    }
    trim(args);
    if (starts_word(args, "FROM")) {
        memmove(args, args + 4, strlen(args + 4) + 1);
        trim(args);
    }
    strncpy(dd, args, MAX_NAME);
    dd[MAX_NAME] = '\0';
    trim(dd);
    upcase(dd);
    if (dd[0] == '\0') {
        say("? DD name missing\n");
        return;
    }
#ifdef __MVS__
    {
        char ddpath[MAX_NAME + 4];
        sprintf(ddpath, "DD:%s", dd);
        f = fopen(ddpath, "r");
    }
#else
    f = fopen(dd, "r");
#endif
    if (!f) {
        say("? cannot open %s for input\n", dd);
        return;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        char vals[MAX_FIELDS][MAX_VALUE + 1];
        char row[DATA_LEN + 1];
        char *p;
        int i = 0;
        int count;
        rtrim(line);
        p = strtok(line, "|");
        while (p != NULL && i < g_table.field_count) {
            trim(p);
            if (!validate_value(i, p)) {
                fclose(f);
                return;
            }
            strncpy(vals[i], p, g_table.fields[i].len);
            vals[i][g_table.fields[i].len] = '\0';
            p = strtok(NULL, "|");
            i++;
        }
        while (i < g_table.field_count) {
            vals[i][0] = '\0';
            i++;
        }
        count = get_count(g_table.name);
        if (count >= MAX_ROWS) {
            say("? table full\n");
            break;
        }
        join_values(vals, row, sizeof(row), 0);
        if (!write_row(count + 1, row)) {
            say("? append from failed rc=%d\n", g_store_rc);
            fclose(f);
            return;
        }
        g_recno = count + 1;
        update_active_index(g_recno, row);
        added++;
    }
    fclose(f);
    say("%d record(s) appended from %s\n", added, dd);
}

static void cmd_copy_to(char *args)
{
    FILE *f;
    char dd[MAX_NAME + 1];
    char *p;
    int all = 0;
    int i;
    int count;
    int copied = 0;
    char row[DATA_LEN + 1];

    if (!require_table()) {
        return;
    }
    trim(args);
    if (starts_word(args, "TO")) {
        memmove(args, args + 2, strlen(args + 2) + 1);
        trim(args);
    }
    p = strchr(args, ' ');
    if (p != NULL) {
        *p = '\0';
        p++;
        trim(p);
        all = same_word(p, "ALL");
    }
    strncpy(dd, args, MAX_NAME);
    dd[MAX_NAME] = '\0';
    trim(dd);
    upcase(dd);
    if (dd[0] == '\0') {
        say("? DD name missing\n");
        return;
    }
    f = fopen(dd, "w");
#ifdef __MVS__
    if (!f) {
        char ddpath[MAX_NAME + 4];
        sprintf(ddpath, "DD:%s", dd);
        f = fopen(ddpath, "w");
    }
#endif
    if (!f) {
        say("? cannot open %s for output\n", dd);
        return;
    }
    count = get_count(g_table.name);
    for (i = 1; i <= count; i++) {
        if (read_row(i, row, sizeof(row)) && row_visible(row, all, NULL)) {
            char body[DATA_LEN + 1];
            char vals[MAX_FIELDS][MAX_VALUE + 1];
            int j;
            strncpy(body, row + 1, sizeof(body));
            body[sizeof(body) - 1] = '\0';
            split_values(body, vals);
            for (j = 0; j < g_table.field_count; j++) {
                if (j > 0) {
                    fputc('|', f);
                }
                fputs(vals[j], f);
            }
            fputc('\n', f);
            copied++;
        }
    }
    fclose(f);
    say("%d record(s) copied to %s\n", copied, dd);
}

static int is_comment_line(const char *line)
{
    return line[0] == '*' || starts_word(line, "NOTE");
}

static int line_starts(const char *line, const char *word)
{
    return starts_word(line, word);
}

static int skip_to_if_else_or_end(char lines[MAX_SCRIPT_LINES][MAX_LINE],
                                  int count, int pc)
{
    int depth = 0;
    int i;
    for (i = pc + 1; i < count; i++) {
        char tmp[MAX_LINE];
        strncpy(tmp, lines[i], sizeof(tmp));
        tmp[sizeof(tmp) - 1] = '\0';
        trim(tmp);
        if (tmp[0] == '\0' || is_comment_line(tmp)) {
            continue;
        }
        if (line_starts(tmp, "IF")) {
            depth++;
        } else if (line_starts(tmp, "ENDIF")) {
            if (depth == 0) {
                return i;
            }
            depth--;
        } else if (line_starts(tmp, "ELSE") && depth == 0) {
            return i;
        }
    }
    return count;
}

static int skip_to_endif(char lines[MAX_SCRIPT_LINES][MAX_LINE],
                         int count, int pc)
{
    int depth = 0;
    int i;
    for (i = pc + 1; i < count; i++) {
        char tmp[MAX_LINE];
        strncpy(tmp, lines[i], sizeof(tmp));
        tmp[sizeof(tmp) - 1] = '\0';
        trim(tmp);
        if (tmp[0] == '\0' || is_comment_line(tmp)) {
            continue;
        }
        if (line_starts(tmp, "IF")) {
            depth++;
        } else if (line_starts(tmp, "ENDIF")) {
            if (depth == 0) {
                return i;
            }
            depth--;
        }
    }
    return count;
}

static int skip_to_enddo(char lines[MAX_SCRIPT_LINES][MAX_LINE],
                         int count, int pc)
{
    int depth = 0;
    int i;
    for (i = pc + 1; i < count; i++) {
        char tmp[MAX_LINE];
        strncpy(tmp, lines[i], sizeof(tmp));
        tmp[sizeof(tmp) - 1] = '\0';
        trim(tmp);
        if (tmp[0] == '\0' || is_comment_line(tmp)) {
            continue;
        }
        if (line_starts(tmp, "DO WHILE")) {
            depth++;
        } else if (line_starts(tmp, "ENDDO")) {
            if (depth == 0) {
                return i;
            }
            depth--;
        }
    }
    return count;
}

static void run_script_lines(char lines[MAX_SCRIPT_LINES][MAX_LINE], int count)
{
    int pc;
    int while_pc[MAX_WHILE_DEPTH];
    char while_expr[MAX_WHILE_DEPTH][MAX_LINE];
    int while_sp = 0;

    for (pc = 0; pc < count; pc++) {
        char line[MAX_LINE];
        strncpy(line, lines[pc], sizeof(line));
        line[sizeof(line) - 1] = '\0';
        trim(line);
        if (line[0] == '\0' || is_comment_line(line)) {
            continue;
        }
        if (line_starts(line, "IF")) {
            char *expr = line + 2;
            trim(expr);
            if (!eval_condition(expr)) {
                pc = skip_to_if_else_or_end(lines, count, pc);
            }
            continue;
        }
        if (line_starts(line, "ELSE")) {
            pc = skip_to_endif(lines, count, pc);
            continue;
        }
        if (line_starts(line, "ENDIF")) {
            continue;
        }
        if (line_starts(line, "DO WHILE")) {
            char *expr = line + 8;
            trim(expr);
            if (!eval_condition(expr)) {
                pc = skip_to_enddo(lines, count, pc);
                continue;
            }
            if (while_sp >= MAX_WHILE_DEPTH) {
                say("? too many nested DO WHILE blocks\n");
                return;
            }
            while_pc[while_sp] = pc;
            strncpy(while_expr[while_sp], expr, MAX_LINE);
            while_expr[while_sp][MAX_LINE - 1] = '\0';
            while_sp++;
            continue;
        }
        if (line_starts(line, "ENDDO")) {
            if (while_sp <= 0) {
                say("? ENDDO without DO WHILE\n");
                return;
            }
            if (eval_condition(while_expr[while_sp - 1])) {
                pc = while_pc[while_sp - 1];
            } else {
                while_sp--;
            }
            continue;
        }
        if (same_word(line, "QUIT") || same_word(line, "EXIT")) {
            break;
        }
        expand_vars(line);
        dispatch(line);
        save_area();
    }
}

static void cmd_do(char *args)
{
    char target[MAX_LINE];
    char (*lines)[MAX_LINE];
    FILE *f;
    int count = 0;

    trim(args);
    if (starts_word(args, "WHILE")) {
        say("? DO WHILE is only valid inside DO command files\n");
        return;
    }
    if (args[0] == '\0') {
        say("? command file DD name missing\n");
        return;
    }
    strncpy(target, args, sizeof(target));
    target[sizeof(target) - 1] = '\0';
    trim(target);
    if (g_do_depth >= MAX_DO_DEPTH) {
        say("? DO nesting too deep\n");
        return;
    }
#ifdef __MVS__
    if (strchr(target, '.') != NULL || strchr(target, '(') != NULL) {
        f = fopen(target, "r");
    } else {
        char ddpath[MAX_NAME + 4];
        char dd[MAX_NAME + 1];
        strncpy(dd, target, MAX_NAME);
        dd[MAX_NAME] = '\0';
        upcase(dd);
        sprintf(ddpath, "DD:%s", dd);
        f = fopen(ddpath, "r");
    }
#else
    f = fopen(target, "r");
#endif
    if (!f) {
        say("? cannot open command file %s\n", target);
        return;
    }
    lines = (char (*)[MAX_LINE])malloc(MAX_SCRIPT_LINES * MAX_LINE);
    if (lines == NULL) {
        fclose(f);
        say("? no memory for command file\n");
        return;
    }
    while (count < MAX_SCRIPT_LINES &&
           fgets(lines[count], MAX_LINE, f) != NULL) {
        rtrim(lines[count]);
        normalize_command_line(lines[count]);
        count++;
    }
    fclose(f);
    g_do_depth++;
    run_script_lines(lines, count);
    g_do_depth--;
    free(lines);
}

static void cmd_store(char *args)
{
    char work[MAX_LINE];
    char upper[MAX_LINE];
    char value[MAX_VALUE + 1];
    char *to;
    char *eq;

    strncpy(work, args, sizeof(work));
    work[sizeof(work) - 1] = '\0';
    trim(work);
    eq = strchr(work, '=');
    if (eq != NULL) {
        *eq = '\0';
        eval_value(eq + 1, value, sizeof(value));
        if (set_var(work, value)) {
            say("%s = %s\n", work, value);
        }
        return;
    }
    strncpy(upper, work, sizeof(upper));
    upper[sizeof(upper) - 1] = '\0';
    upcase(upper);
    to = strstr(upper, " TO ");
    if (to == NULL) {
        say("? try STORE value TO var\n");
        return;
    }
    work[to - upper] = '\0';
    eval_value(work, value, sizeof(value));
    if (set_var(work + (to - upper) + 4, value)) {
        say("%s = %s\n", work + (to - upper) + 4, value);
    }
}

static void cmd_display_memory(void)
{
    int i;
    say("Memory variables:\n");
    if (g_var_count == 0) {
        say("  none\n");
        return;
    }
    for (i = 0; i < g_var_count; i++) {
        say("  %-16s %s\n", g_vars[i].name, g_vars[i].value);
    }
}

static void cmd_question(char *args)
{
    char value[MAX_VALUE + 1];
    eval_value(args, value, sizeof(value));
    say("%s\n", value);
}

static void dispatch(char *line)
{
    char *cmd;
    char *args;

    trim(line);
    if (line[0] == '\0') {
        return;
    }
    cmd = strtok(line, " ");
    args = strtok(NULL, "");
    if (args == NULL) {
        args = "";
    }

    if (same_word(cmd, "HELP")) {
        cmd_help();
    } else if (strcmp(cmd, "?") == 0) {
        cmd_question(args);
    } else if (same_word(cmd, "TABLES")) {
        cmd_tables();
    } else if (same_word(cmd, "SELECT")) {
        cmd_select(args);
    } else if (same_word(cmd, "STORE")) {
        cmd_store(args);
    } else if (same_word(cmd, "DO")) {
        cmd_do(args);
    } else if (same_word(cmd, "INDEX")) {
        cmd_index(args);
    } else if (same_word(cmd, "INDEXES")) {
        cmd_indexes();
    } else if (same_word(cmd, "REINDEX")) {
        cmd_reindex();
    } else if (same_word(cmd, "SEEK")) {
        cmd_seek(args);
    } else if (same_word(cmd, "RELATION")) {
        cmd_relation();
    } else if (same_word(cmd, "SET")) {
        if (starts_word(args, "INDEX")) {
            memmove(args, args + 5, strlen(args + 5) + 1);
            trim(args);
            cmd_set_index(args);
        } else if (starts_word(args, "RELATION")) {
            memmove(args, args + 8, strlen(args + 8) + 1);
            trim(args);
            cmd_set_relation(args);
        } else if (starts_word(args, "FILTER")) {
            memmove(args, args + 6, strlen(args + 6) + 1);
            trim(args);
            cmd_set_filter(args);
        } else if (starts_word(args, "DELETED")) {
            memmove(args, args + 7, strlen(args + 7) + 1);
            trim(args);
            cmd_set_deleted(args);
        } else {
            say("? try SET INDEX, SET RELATION, SET FILTER or SET DELETED\n");
        }
    } else if (same_word(cmd, "CREATE")) {
        cmd_create(args);
    } else if (same_word(cmd, "USE")) {
        cmd_use(args);
    } else if (same_word(cmd, "APPEND")) {
        if (starts_word(args, "FROM")) {
            cmd_append_from(args);
        } else {
            cmd_append(args);
        }
    } else if (same_word(cmd, "COPY")) {
        cmd_copy_to(args);
    } else if (same_word(cmd, "LIST") || same_word(cmd, "BROWSE")) {
        if (same_word(args, "MEMORY")) {
            cmd_display_memory();
        } else {
            cmd_list(args);
        }
    } else if (same_word(cmd, "DISPLAY")) {
        if (args[0] == '\0') {
            show_current();
        } else if (same_word(args, "STRUCTURE")) {
            cmd_structure();
        } else if (same_word(args, "TABLES")) {
            cmd_tables();
        } else if (same_word(args, "MEMORY")) {
            cmd_display_memory();
        } else if (same_word(args, "STATUS")) {
            save_area();
            cmd_status();
        } else {
            cmd_display_records(args);
        }
    } else if (same_word(cmd, "REPLACE")) {
        cmd_replace(args);
    } else if (same_word(cmd, "DELETE")) {
        cmd_delete(args);
    } else if (same_word(cmd, "RECALL")) {
        cmd_recall(args);
    } else if (same_word(cmd, "PACK")) {
        cmd_pack();
    } else if (same_word(cmd, "ZAP")) {
        cmd_zap();
    } else if (same_word(cmd, "FIND") || same_word(cmd, "LOCATE")) {
        if (same_word(cmd, "LOCATE")) {
            cmd_locate(args);
        } else {
            cmd_find(args);
        }
    } else if (same_word(cmd, "CONTINUE")) {
        cmd_continue();
    } else if (same_word(cmd, "SUM")) {
        cmd_sum(args, 0);
    } else if (same_word(cmd, "AVERAGE")) {
        cmd_sum(args, 1);
    } else if (same_word(cmd, "COUNT")) {
        cmd_count();
    } else if (same_word(cmd, "GO") || same_word(cmd, "GOTO")) {
        cmd_go(args);
    } else if (same_word(cmd, "SKIP")) {
        cmd_skip(args);
    } else if (same_word(cmd, "QUIT") || same_word(cmd, "EXIT")) {
        /* handled by main */
    } else if (same_word(cmd, "IF") || same_word(cmd, "ELSE") ||
               same_word(cmd, "ENDIF") || same_word(cmd, "ENDDO") ||
               same_word(cmd, "WHILE")) {
        say("? control flow is only available inside DO command files\n");
    } else {
        say("? unknown command: %s\n", cmd);
    }
}

int main(int argc, char **argv)
{
    char line[MAX_LINE];
    (void)argc;
    (void)argv;

    say("DBASE/TSO 0.1 for MVS 3.8j - type HELP\n");
    init_areas();
    if (!kv_open()) {
        say("? cannot open DD %s. Allocate the VSAM store first.\n", DB_DD);
    }
#if !defined(DBASE_TSO)
    {
        char (*lines)[MAX_LINE];
        int count = 0;

        lines = (char (*)[MAX_LINE])malloc(MAX_SCRIPT_LINES * MAX_LINE);
        if (lines == NULL) {
            say("? no memory for SYSIN command file\n");
            kv_close();
            return 1;
        }
        while (count < MAX_SCRIPT_LINES &&
               fgets(lines[count], MAX_LINE, stdin) != NULL) {
            rtrim(lines[count]);
            normalize_command_line(lines[count]);
            count++;
        }
        run_script_lines(lines, count);
        free(lines);
        kv_close();
        say("Bye\n");
        flush_out();
        return 0;
    }
#else
    for (;;) {
        say(". ");
        flush_out();
        if (!read_line(line, sizeof(line))) {
            break;
        }
        {
            char test[MAX_LINE];
            strncpy(test, line, sizeof(test));
            test[sizeof(test) - 1] = '\0';
            trim(test);
            if (same_word(test, "QUIT") || same_word(test, "EXIT")) {
                break;
            }
        }
        expand_vars(line);
        dispatch(line);
        save_area();
        flush_out();
    }
#endif
    kv_close();
    say("Bye\n");
    flush_out();
    return 0;
}
