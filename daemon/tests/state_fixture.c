#include "gitstored/protocol.h"
#include "gitstored/state.h"
#include <string.h>
int main(int argc, char **argv) {
    if (argc != 3 || strcmp(argv[1], "seed"))
        return 2;
    gstd_json_initialize();
    sqlite3 *db = NULL;
    if (gstd_state_open(argv[2], &db))
        return 1;
    int status =
        sqlite3_exec(db,
                     "INSERT INTO profiles VALUES('core','1','synthetic','synthetic','fixture');"
                     "INSERT INTO deployments "
                     "VALUES('d1','core','1','running',2,1,'pending','normal','held','2026-11-"
                     "03T00:00:00Z',NULL);"
                     "INSERT INTO deployments "
                     "VALUES('d2','core','1','stopped',1,0,'pending','normal','released','2026-11-"
                     "03T00:00:00Z',NULL);"
                     "INSERT INTO operations "
                     "VALUES('op1',1,'deployment.start','d1',NULL,'"
                     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa','{}','2026-"
                     "11-03T00:00:00Z','2026-11-03T00:00:00Z','running','normal',NULL,NULL);",
                     NULL, NULL, NULL);
    sqlite3_close(db);
    return status == SQLITE_OK ? 0 : 1;
}
