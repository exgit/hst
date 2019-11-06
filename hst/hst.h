#pragma once


#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <netinet/in.h>


/* Result codes returned by library functions.
 *
 * HST_RES_OK
 *      Operation completed successfully.
 * HST_RES_ERR
 *      Operation not completed and application should perform
 *      recovery actions or terminate.
 * HST_RES_CONT
 *      Operation not completed as expected, but application can continue.
 */
#define HST_RES_OK           (0)
#define HST_RES_ERR         (-1)
#define HST_RES_CONT        (-2)


// Library configuration parameters.
typedef struct _hst_conf_t {
    int backlog;            // backlog parameter for listen()
    in_addr_t addr;         // addr to listen on
    in_port_t port;         // port to listen on
    int mem_total;          // amount of memory to be used by library
} hst_conf_t;


// Http header.
typedef struct _hst_hdr_t hst_hdr_t;
struct _hst_hdr_t {
    hst_hdr_t *next;        // NULL means last element in list
    const char *name;
    const char *value;
};


// Elements of request`s path parsed as list of names.
typedef struct _hst_path_elt_t hst_path_elt_t;
struct _hst_path_elt_t {
    hst_path_elt_t *next;   // NULL means last element of list
    const char *name;
};


// Elements of request`s query parsed as list of name-values pairs.
typedef struct _hst_query_elt_t hst_query_elt_t;
struct _hst_query_elt_t {
    hst_query_elt_t *next;  // NULL means last element in list
    const char *name;
    const char *value;
};


// Template object.
typedef struct _hst_tpl_elt_t hst_tpl_t;


// Template function.
typedef void(*hst_tpl_func_t)(void);


// Request object.
typedef struct {
    // request method used by client
    bool method_get;
    bool method_post;
    bool method_head;
    char padding1[5];

    // request-target from request line (path[?query] part from URL)
    const char *request_target;

    // ptr to first http header
    hst_hdr_t *hdr_first;

    // ptr to first path element
    hst_path_elt_t *path_elt_first;

    // ptr to first query element
    hst_query_elt_t *query_elt_first;

    // request body
    const char *body;   // ptr to body as zero-terminated string
    int body_len;       // body length

    // result
    int res_code;
    char *res_text;
} hst_req_t;


int hst_init(hst_conf_t *conf);
void hst_deinit(void);

int hst_tpl_function(const char *name, hst_tpl_func_t func);
hst_tpl_t *hst_tpl_compile(const char *psz);

int hst_read(hst_req_t **req);

void hst_write_res(int code, const char *text);
void hst_write_hdr(const char *name, const char *val);
int hst_write_tpl(const hst_tpl_t *tpl);

void hst_write_body_data(const void *ptr, int size);
void hst_write_body_print(const char *strz);
void hst_write_body_printf(const char *format, ...);
int hst_write_end(void);
