#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

static int logs;
static char last[128];

static void on_log(void *ctx, int code, const char *msg)
{
    (*(int *)ctx)++;
    logs = code;
    snprintf(last, sizeof last, "%s", msg);
}

int main(void)
{
    int calls = 0;
    int c1 = sqlite3_config(SQLITE_CONFIG_LOG, on_log, &calls);
    int c2 = sqlite3_config(SQLITE_CONFIG_MEMSTATUS, 0);
    int c3 = sqlite3_config(SQLITE_CONFIG_URI, 1);
    int c4 = sqlite3_config(SQLITE_CONFIG_MMAP_SIZE, (sqlite3_int64)1 << 20, (sqlite3_int64)1 << 24);
    int c5 = sqlite3_config(SQLITE_CONFIG_SERIALIZED);
    sqlite3_initialize();
    sqlite3 *db = NULL;
    int o = sqlite3_open(":memory:", &db);
    int fk = -1, trig = -1;
    int d1 = sqlite3_db_config(db, SQLITE_DBCONFIG_ENABLE_FKEY, 1, &fk);
    int d2 = sqlite3_db_config(db, SQLITE_DBCONFIG_ENABLE_TRIGGER, 0, &trig);
    int d3 = sqlite3_db_config(db, SQLITE_DBCONFIG_MAINDBNAME, "primary");
    sqlite3_stmt *st = NULL;
    int bad = sqlite3_prepare_v2(db, "select * from nowhere", -1, &st, NULL);
    printf("sqlite config %d %d %d %d %d open=%d db %d %d %d fk=%d trig=%d bad=%d logged=%d code=%d msg=%.20s\n", c1, c2,
           c3, c4, c5, o, d1, d2, d3, fk, trig, bad, calls > 0, logs, last);
    sqlite3_close(db);
    return 0;
}
