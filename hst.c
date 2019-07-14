#include "hst.h"
#include <stdlib.h>
#include <stdio.h>


// default values for 'hst_conf_t' fields
#define DFLT_CONF_PORT			80
#define DFLT_CONF_USE_MEM		64*1024
#define DFLT_CONF_BACKLOG		16


// library states
#define STATE_NOT_INIT			0
#define STATE_WAITING_REQUEST	1
#define STATE_WRITING_REPLY		2


static struct {
	char errmsg[256];	// buffer for error description
	hst_conf_t conf;	// library configuration

	int state;			// library state
	char padding[4];

	char *mem_base;		// memory base ptr
	size_t mem_used;	// number of allocated bytes

} me;


/***************************************************************************\
 * private methods
\***************************************************************************/

static inline void _hst_errmsg_reset(void) {
	me.errmsg[0] = 0;
}


static inline void _hst_errmsg(const char *msg) {
	snprintf(me.errmsg, sizeof(me.errmsg), msg);
}


static inline void *_hst_alloc(size_t size) {
	void *ret = NULL;

	// align up
	size += -size & (sizeof(size_t)-1);

	if (me.mem_used + size > me.conf.mem_total) {
		_hst_errmsg("Not enough memory.");
		goto exit;
	}

	ret = me.mem_base + me.mem_used;
	me.mem_used += size;

exit:
	return ret;
}


/***************************************************************************\
 * public methods
\***************************************************************************/


int hst_init(hst_conf_t *conf) {
	int ret = HST_RES_ERR;

	_hst_errmsg_reset();
	if (me.state != STATE_NOT_INIT) {
		_hst_errmsg("Already initialized.");
		goto exit;
	}

	if (conf) me.conf = *conf;
	if (!me.conf.port) me.conf.port = DFLT_CONF_PORT;
	if (me.conf.mem_total < 1024) me.conf.mem_total = DFLT_CONF_USE_MEM;

	me.mem_base = malloc(me.conf.mem_total);
	if (me.mem_base == NULL) {
		_hst_errmsg("Not enough memory.");
		goto exit;
	}

	ret = HST_RES_OK;

exit:
	return ret;
}


void hst_deinit(void) {
	_hst_errmsg_reset();
	if (me.state == STATE_NOT_INIT)
		return;

	return;
}


const char *hst_error(void) {
	return me.errmsg;
}


int hst_get(hst_req_t **req) {
	int ret = HST_RES_ERR;

	return ret;
}


int hst_res(int code) {
	int ret = HST_RES_ERR;

	return ret;
}


int hst_hdr(const char *name, const char *val) {
	int ret = HST_RES_ERR;

	return ret;
}


int hst_body_all(void *ptr, int size) {
	int ret = HST_RES_ERR;

	return ret;
}


int hst_body_begin(bool buffer) {
	int ret = HST_RES_ERR;

	return ret;
}


int hst_body_data(void *ptr, int size) {
	int ret = HST_RES_ERR;

	return ret;
}


int hst_body_print(const char *str) {
	int ret = HST_RES_ERR;

	return ret;
}


int hst_body_printf(const char *format, ...) {
	int ret = HST_RES_ERR;

	return ret;
}


int hst_body_end(void) {
	int ret = HST_RES_ERR;

	return ret;
}
