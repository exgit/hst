#pragma once


#include <stdbool.h>


#define HST_RES_OK		0
#define HST_RES_ERR		(-1)


typedef struct {
	const char *host;		// host name
	int use_mem;			// maximum amount of memory to be used by library
	int backlog;			// backlog parameter for listen()
} hst_conf_t;


typedef struct {
	const char *name;
	const char *value;
} hst_hdr_t;


typedef struct {
	// request type
	bool rqt_get;
	bool rqt_post;
	bool rqt_head;
	char padding1[5];

	const char *reqstr;			// request string
	hst_hdr_t *headers;			// headers array
	int        headers_num;		// number of headers
} hst_req_t;


int hst_init(hst_conf_t *conf);
void hst_deinit(void);
const char *hst_error(void);

int hst_get(hst_req_t **req);

int hst_res(int code);
int hst_hdr(const char *name, const char *val);

int hst_body_all(void *ptr, int size);

int hst_body_begin(bool buffer);
int hst_body_data(void *ptr, int size);
int hst_body_print(const char *str);
int hst_body_printf(const char *format, ...);
int hst_body_end(void);
