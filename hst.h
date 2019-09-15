#pragma once


#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <netinet/in.h>


// Error reporting macro.
#if 1
    #define ERROR(fmt, ...) fprintf(stderr, \
            "ERROR: %s():%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)
#else
    #define ERROR(fmt, ...) ((void)0)
#endif


// Result codes returned by library functions.
#define HST_RES_OK      0
#define HST_RES_ERR     (-1)
#define HST_RES_TIMEOUT (-2)


// Known request headers ids.
typedef enum _hst_hdr_id_t {
    // ids should be sorted in alphabetical order
    HSTHDRID_ACCEPT,
    HSTHDRID_ACCEPT_ENCODING,
    HSTHDRID_ACCEPT_LANGUAGE,
    HSTHDRID_CONNECTION,
    HSTHDRID_HOST,
    HSTHDRID_USER_AGENT,

    // total count of ids
    COUNT_OF_HSTHDRID
} hst_hdr_id_t;


// Library configuration parameters.
typedef struct _hst_conf_t {
    int backlog;            // backlog parameter for listen()
    in_addr_t addr;         // addr to listen on
    in_port_t port;         // port to listen on
    size_t mem_total;       // amount of memory to be used by library
} hst_conf_t;


// Elements of request`s path parsed as list of names.
typedef struct _hst_path_elt_t hst_path_elt_t;
struct _hst_path_elt_t {
    hst_path_elt_t *next;   // NULL means last element of list
    const char *name;
};


// Elements of request`s query parsed as list of name-values pairs.
typedef struct _hst_query_elt_t hst_query_elt_t;
struct _hst_query_elt_t {
    hst_query_elt_t *next;
    const char *name;
    const char *value;
};


typedef struct {
    // request method used by client
    bool method_get;
    bool method_post;
    bool method_head;
    char padding1[5];

    // request-target from request line (path[?query])
    const char *request_target;

    // ptr to first path element
    hst_path_elt_t *path_elt_first;

    // ptr to first query element
    hst_query_elt_t *query_elt_first;
} hst_req_t;


int hst_init(hst_conf_t *conf);
void hst_deinit(void);

int hst_read(hst_req_t **req);

int hst_write_res(int code);
int hst_write_hdr(const char *name, const char *val);

int hst_write_body_all(void *ptr, int size);

int hst_write_body_begin(bool buffer);
int hst_write_body_data(void *ptr, int size);
int hst_write_body_print(const char *str);
int hst_write_body_printf(const char *format, ...);
int hst_write_body_end(void);
