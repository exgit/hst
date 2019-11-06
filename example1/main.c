#include "../hst/hst.h"
#include <stdio.h>
#include <string.h>
#include <time.h>


#define ERROR(fmt, ...) fprintf(stderr, \
        "ERROR: %s():%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)


// forward declarations
static void prepare_templates(void);
static void tfunc_req_number(void);
static void tfunc_uptime(void);
static void tfunc_show_headers(void);
static void request_get(void);
static void get_root(void);


// server object
static struct {
    int req_count;              // handled requests count
    char padding1[4];

    time_t time_start;          // server start time
} srv;


// request context
static struct {
    hst_req_t *req;
    hst_path_elt_t *path;
    bool req_handled;
    char padding1[7];
} ctx;


// compiled templates
static hst_tpl_t *tpl_main;


int main() {
    int res;

    srv.time_start = time(NULL);

    // init hst library
    hst_conf_t conf;
    memset(&conf, 0, sizeof(conf));
    conf.addr = INADDR_ANY;
    conf.port = htons(30000);
    conf.mem_total = 10000;
    res = hst_init(&conf);
    if (res != HST_RES_OK) {
        ERROR("Hst is not initialised.");
        goto exit;
    }
    printf("Hst initialised.\n");

    prepare_templates();

    // main event loop
    for (;;) {
        memset(&ctx, 0, sizeof(ctx));

        res = hst_read(&ctx.req);
        if (res == HST_RES_CONT) {
            continue;
        } else if (res == HST_RES_ERR) {
            printf("Hst error.\n");
            break;
        } else if (res != HST_RES_OK) {
            printf("Unknown error.\n");
            break;
        }

        ctx.path = ctx.req->path_elt_first;

        if (ctx.req->method_get) {
            if (ctx.path && 0 == strcmp(ctx.path->name, "exit")) {
                hst_write_res(200, "Ok");
                hst_write_hdr("Content-Type", "text/plain");
                hst_write_body_print("HST server shutdown.");
                hst_write_end();
                break;
            }
            request_get();
        } else if (ctx.req->method_post) {
            // todo: implement
        } else if (ctx.req->method_head) {
            // todo: implement
        }

        if (ctx.req_handled) {
            srv.req_count++;
        } else {
            hst_write_res(404, "Not found");
            hst_write_hdr("Content-Type", "text/plain");
            hst_write_body_print("Page not found.");
            hst_write_end();
        }
    }

    hst_deinit();
    printf("Hst deinitialised.\n");

exit:
    return 0;
}


static void request_get(void) {
    if (ctx.path == NULL) {
        get_root();
        return;
    }
}


static void prepare_templates(void) {
    int res;

    res = hst_tpl_function("req_number", tfunc_req_number);
    if (res != HST_RES_OK) goto efunc;
    res = hst_tpl_function("uptime", tfunc_uptime);
    if (res != HST_RES_OK) goto efunc;
    res = hst_tpl_function("show_headers", tfunc_show_headers);
    if (res != HST_RES_OK) goto efunc;

    tpl_main = hst_tpl_compile(
"<!DOCTYPE html>\r\n"
"<html>\r\n"
"<head>\r\n"
"  <title>HST</title>\r\n"
"</head>\r\n"
"<body>\r\n"
"  <h1>HST server demo.</h1>\r\n"
"  <p>This is a request number <!--hst req_number -->.</p>\r\n"
"  <p>Server uptime is <!--hst uptime --> seconds.</p>\r\n"
"  <h3>Request headers:</h3>\r\n"
"  <!--hst show_headers -->\r\n"
"</body>\r\n"
"<html>\r\n");
    if (tpl_main == NULL) goto etpl;

    return;

efunc:
    ERROR("Function register error.");
    return;
etpl:
    ERROR("Template compile error.");
    return;
}


static void tfunc_req_number(void) {
    hst_write_body_printf("%d", srv.req_count+1);
}


static void tfunc_uptime(void) {
    time_t time_now = time(NULL);
    time_t time_passed = time_now - srv.time_start;
    struct tm tm_time = *gmtime(&time_passed);
    char buf[100];
    strftime(buf, sizeof(buf), "%T", &tm_time);
    hst_write_body_printf("%s", buf);
}



static void tfunc_show_headers(void) {
    hst_hdr_t *hdr = ctx.req->hdr_first;
    for (; hdr; hdr = hdr->next) {
        hst_write_body_printf("<p>%s: %s</p>\r\n", hdr->name, hdr->value);
    }
}


static void get_root(void) {
    hst_write_res(200, "Ok");
    hst_write_tpl(tpl_main);
    ctx.req_handled = true;
}
