#include "hst.h"
#include <stdio.h>


int main() {
    int res;

    res = hst_init(&(hst_conf_t){
        .addr = INADDR_ANY,
        .port = htons(30000),
        .mem_total = 10000,
        .backlog = 1
    });
    if (res != HST_RES_OK) {
        ERROR("Hst is not initialised.");
        goto exit;
    }

    printf("Hst initialised.\n");

    int rcount = 0;
    hst_req_t *req = NULL;
    for (;;) {
        res = hst_read(&req);
        if (res == HST_RES_ERR) {
            printf("Got error.\n");
            break;
        } else if (res == HST_RES_TIMEOUT) {
            continue;
        }

        rcount++;
        printf("Got request %d.\n", rcount);

        if (req->method_get) {
            //printf("Method is GET.\n");
        } else if (req->method_post) {
            //printf("Method is POST.\n");
        } else if (req->method_head) {
            //printf("Method is HEAD.\n");
        }

        //printf("Path is:\n");
        hst_path_elt_t *e;
        for (e = req->path_elt_first; e; e = e->next) {
            //printf("\t%s\n", e->name);
        }

        hst_write_body_all(NULL, 0);
    }

    hst_deinit();
    printf("Hst deinitialised.\n");

exit:
    return res;
}
