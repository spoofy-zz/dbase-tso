#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdarg.h>

#if defined(__MVS__) && defined(DBASE_TSO)
extern int dbtget(char *buf, int max) asm("DBTGET");
extern int dbtput(char *buf, int len) asm("DBTPUT");
#endif

#define MAX_LINE 512
#define MAX_NAME 16
#define MAX_FIELDS 8
#define MAX_VALUE 32
#define MAX_ROWS 240
#define MAX_STORE 256
#define KEY_LEN 64
#define DATA_LEN 256
#define DB_FILE "DBASED"

struct Field {
    char name[MAX_NAME + 1];
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

static struct Table g_table;
static int g_have_table = 0;
static struct Rec g_store[MAX_STORE];
static int g_store_count = 0;
static int g_store_loaded = 0;

#if defined(__MVS__) && defined(DBASE_TSO)
static char g_out[MAX_LINE];
static int g_out_len = 0;
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

static void normalize_command_line(char *line)
{
    static const char *cmds[] = {
        "HELP", "CREATE", "USE", "APPEND", "LIST", "BROWSE",
        "DISPLAY", "REPLACE", "DELETE", "PACK", "FIND", "LOCATE",
        "COUNT", "QUIT", "EXIT", NULL
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
    if (seq >= 0) {
        sprintf(out, "%s|%.16s|%06d", kind, name, seq);
    } else {
        sprintf(out, "%s|%.16s", kind, name);
    }
}

static int load_store(void)
{
    FILE *f;
    char line[KEY_LEN + DATA_LEN + 8];

    if (g_store_loaded) {
        return 1;
    }
    g_store_loaded = 1;
    f = fopen(DB_FILE, "r");
    if (!f) {
        return 1;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        char *tab;
        rtrim(line);
        tab = strchr(line, '\t');
        if (tab == NULL || g_store_count >= MAX_STORE) {
            continue;
        }
        *tab = '\0';
        strncpy(g_store[g_store_count].key, line, KEY_LEN - 1);
        g_store[g_store_count].key[KEY_LEN - 1] = '\0';
        strncpy(g_store[g_store_count].data, tab + 1, DATA_LEN - 1);
        g_store[g_store_count].data[DATA_LEN - 1] = '\0';
        g_store_count++;
    }
    fclose(f);
    return 1;
}

static int save_store(void)
{
    FILE *f;
    int i;

    f = fopen(DB_FILE, "w");
    if (!f) {
        return 0;
    }
    for (i = 0; i < g_store_count; i++) {
        fprintf(f, "%s\t%s\n", g_store[i].key, g_store[i].data);
    }
    fclose(f);
    return 1;
}

static int kv_get(const char *key, char *data, int max)
{
    int i;
    if (!load_store()) {
        return 0;
    }
    for (i = 0; i < g_store_count; i++) {
        if (strcmp(g_store[i].key, key) == 0) {
            strncpy(data, g_store[i].data, max - 1);
            data[max - 1] = '\0';
            return 1;
        }
    }
    return 0;
}

static int kv_put(const char *key, const char *data)
{
    int i;
    if (!load_store()) {
        return 0;
    }
    for (i = 0; i < g_store_count; i++) {
        if (strcmp(g_store[i].key, key) == 0) {
            strncpy(g_store[i].data, data, DATA_LEN - 1);
            g_store[i].data[DATA_LEN - 1] = '\0';
            return save_store();
        }
    }
    if (g_store_count >= MAX_STORE) {
        return 0;
    }
    strncpy(g_store[g_store_count].key, key, KEY_LEN - 1);
    g_store[g_store_count].key[KEY_LEN - 1] = '\0';
    strncpy(g_store[g_store_count].data, data, DATA_LEN - 1);
    g_store[g_store_count].data[DATA_LEN - 1] = '\0';
    g_store_count++;
    return save_store();
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
        strncpy(t->fields[i].name, tok, MAX_NAME);
        t->fields[i].name[MAX_NAME] = '\0';
        t->fields[i].len = atoi(colon + 1);
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
        sprintf(part, "|%s:%d", t->fields[i].name, t->fields[i].len);
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
    return 1;
}

static int get_count(const char *table)
{
    char key[KEY_LEN];
    char data[64];
    make_key(key, "C", table, -1);
    if (!kv_get(key, data, sizeof(data))) {
        return 0;
    }
    return atoi(data);
}

static int set_count(const char *table, int count)
{
    char key[KEY_LEN];
    char data[32];
    make_key(key, "C", table, -1);
    sprintf(data, "%d", count);
    return kv_put(key, data);
}

static int field_index(const char *name)
{
    int i;
    for (i = 0; i < g_table.field_count; i++) {
        if (same_word(g_table.fields[i].name, name)) {
            return i;
        }
    }
    return -1;
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

static int read_row(int seq, char *row, int max)
{
    char key[KEY_LEN];
    make_key(key, "R", g_table.name, seq);
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
    say("  CREATE name field len [field len ...]\n");
    say("  USE name\n");
    say("  APPEND field=value [field=value ...]\n");
    say("  LIST [ALL]\n");
    say("  DISPLAY STRUCTURE\n");
    say("  REPLACE recno field=value [field=value ...]\n");
    say("  DELETE recno\n");
    say("  PACK\n");
    say("  FIND text\n");
    say("  COUNT\n");
    say("  QUIT\n");
}

static void cmd_create(char *args)
{
    struct Table t;
    char *tok;
    char key[KEY_LEN];
    char data[DATA_LEN + 1];

    memset(&t, 0, sizeof(t));
    tok = strtok(args, " ");
    if (tok == NULL) {
        say("? table name missing\n");
        return;
    }
    strncpy(t.name, tok, MAX_NAME);
    t.name[MAX_NAME] = '\0';
    upcase(t.name);
    while ((tok = strtok(NULL, " ")) != NULL) {
        char fname[MAX_NAME + 1];
        char *lenp;
        if (t.field_count >= MAX_FIELDS) {
            say("? too many fields\n");
            return;
        }
        strncpy(fname, tok, MAX_NAME);
        fname[MAX_NAME] = '\0';
        upcase(fname);
        lenp = strtok(NULL, " ");
        if (lenp == NULL) {
            say("? length missing for %s\n", fname);
            return;
        }
        strncpy(t.fields[t.field_count].name, fname, MAX_NAME);
        t.fields[t.field_count].len = atoi(lenp);
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
    if (!kv_put(key, data) || !set_count(t.name, 0)) {
        say("? store write failed; is DBASED allocated?\n");
        return;
    }
    g_table = t;
    g_have_table = 1;
    say("Table %s created with %d fields\n", t.name, t.field_count);
}

static void cmd_use(char *args)
{
    trim(args);
    if (load_table(args)) {
        say("Using %s\n", g_table.name);
    } else {
        say("? table not found: %s\n", args);
    }
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
    if (!apply_assignments(args, vals)) {
        return;
    }
    count = get_count(g_table.name);
    if (count >= MAX_ROWS) {
        say("? table full\n");
        return;
    }
    join_values(vals, row, sizeof(row), 0);
    if (!write_row(count + 1, row) || !set_count(g_table.name, count + 1)) {
        say("? append failed\n");
        return;
    }
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

static void cmd_list(char *args)
{
    int i;
    int count;
    int all = 0;
    char row[DATA_LEN + 1];

    if (!require_table()) {
        return;
    }
    trim(args);
    all = same_word(args, "ALL");
    count = get_count(g_table.name);
    print_header();
    for (i = 1; i <= count; i++) {
        if (read_row(i, row, sizeof(row)) && (all || row[0] != '*')) {
            print_row(i, row);
        }
    }
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
        say("  %-16s %d\n", g_table.fields[i].name, g_table.fields[i].len);
    }
}

static void cmd_replace(char *args)
{
    char *recno;
    char row[DATA_LEN + 1];
    char body[DATA_LEN + 1];
    char vals[MAX_FIELDS][MAX_VALUE + 1];
    int seq;

    if (!require_table()) {
        return;
    }
    recno = strtok(args, " ");
    if (recno == NULL) {
        say("? recno missing\n");
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
    if (!apply_assignments(NULL, vals)) {
        return;
    }
    join_values(vals, row, sizeof(row), 0);
    if (!write_row(seq, row)) {
        say("? replace failed\n");
        return;
    }
    say("Record %d replaced\n", seq);
}

static void cmd_delete(char *args)
{
    int seq;
    char row[DATA_LEN + 1];
    if (!require_table()) {
        return;
    }
    trim(args);
    seq = atoi(args);
    if (seq < 1 || !read_row(seq, row, sizeof(row)) || row[0] == '*') {
        say("? record not found\n");
        return;
    }
    row[0] = '*';
    if (!write_row(seq, row)) {
        say("? delete failed\n");
        return;
    }
    say("Record %d deleted\n", seq);
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
        if (read_row(i, row, sizeof(row)) && row[0] != '*') {
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
    set_count(g_table.name, dst);
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
        if (read_row(i, row, sizeof(row)) && row[0] != '*') {
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

    if (same_word(cmd, "HELP") || strcmp(cmd, "?") == 0) {
        cmd_help();
    } else if (same_word(cmd, "CREATE")) {
        cmd_create(args);
    } else if (same_word(cmd, "USE")) {
        cmd_use(args);
    } else if (same_word(cmd, "APPEND")) {
        cmd_append(args);
    } else if (same_word(cmd, "LIST") || same_word(cmd, "BROWSE")) {
        cmd_list(args);
    } else if (same_word(cmd, "DISPLAY")) {
        if (same_word(args, "STRUCTURE")) {
            cmd_structure();
        } else {
            say("? try DISPLAY STRUCTURE\n");
        }
    } else if (same_word(cmd, "REPLACE")) {
        cmd_replace(args);
    } else if (same_word(cmd, "DELETE")) {
        cmd_delete(args);
    } else if (same_word(cmd, "PACK")) {
        cmd_pack();
    } else if (same_word(cmd, "FIND") || same_word(cmd, "LOCATE")) {
        cmd_find(args);
    } else if (same_word(cmd, "COUNT")) {
        cmd_count();
    } else if (same_word(cmd, "QUIT") || same_word(cmd, "EXIT")) {
        /* handled by main */
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
    load_store();
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
        dispatch(line);
        flush_out();
    }
    say("Bye\n");
    flush_out();
    return 0;
}
