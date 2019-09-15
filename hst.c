#include "hst.h"
#include <sys/ioctl.h>
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


typedef unsigned int uint;


/****************************************************************************
* memory allocator
****************************************************************************/

//!
static struct _mem {
    char *start;        // start of memory allocated by malloc()
    size_t total;       // total amount of memory
    size_t current;     // current amount of memory allocated by this allocator
} mem;
static int mem_init(size_t size) {
    if (mem.start) {
        ERROR("Memory already initialised.");
        return HST_RES_ERR;
    }
    mem.start = malloc(size);
    if (mem.start == NULL) {
        ERROR("Not enough memory.");
        return HST_RES_ERR;
    }
    mem.total = size;
    mem.current = 0;
    return HST_RES_OK;
}
static inline void mem_deinit(void) {
    free(mem.start);
    memset(&mem, 0, sizeof(mem));
}
static void *mem_alloc(size_t size) {
    void *ret = NULL;
    // align up
    size_t current = mem.current + (-mem.current & (sizeof(size_t)-1));
    if (current + size > mem.total) {
        ERROR("Not enough memory. Requested %u bytes.", (uint)size);
    } else {
        ret = mem.start + current;
        mem.current = current + size;
    }
    return ret;
}
/*static bool mem_grow(size_t size) {
    if (mem.current + size > mem.total) {
        ERROR("Not enough memory.");
        return false;
    }
    mem.current += size;
    return true;
}*/
static inline size_t mem_checkpoint_get(void) {
    return mem.current;
}
static inline void mem_checkpoint_restore(size_t checkpoint) {
    mem.current = checkpoint;
}


/****************************************************************************
* double linked list
****************************************************************************/

//!
typedef struct _list_item ListItem;
struct _list_item {
    ListItem *prev;
    ListItem *next;
};
/*static inline void ListInsBefore(ListItem *item, ListItem *new_item) {
    item->prev->next = new_item;
    new_item->prev = item->prev;
    new_item->next = item;
    item->prev = new_item;
}
static inline void ListInsAfter(ListItem *item, ListItem *new_item) {
    item->next->prev = new_item;
    new_item->prev = item;
    new_item->next = item->next;
    item->next = new_item;
}
static inline void ListRemove(ListItem *item) {
    item->prev->next = item->next;
    item->next->prev = item->prev;
}
static inline void ListInsFirst(ListItem *head, ListItem *new_item) {
    ListInsAfter(head, new_item);
}
static inline void ListInsLast(ListItem *head, ListItem *new_item) {
    ListInsBefore(head, new_item);
}*/


/****************************************************************************
* hst
****************************************************************************/

// default values for 'hst_conf_t' fields
#define DFLT_CONF_PORT			80
#define DFLT_CONF_BACKLOG		32
#define DFLT_CONF_MEM_TOTAL		16*1024


// constants
//
// note: rfc7230: "It is RECOMMENDED that all HTTP senders and recipients
// support, at a minimum, request-line lengths of 8000 octets."
#define RWBUF_SIZE              1024


/* module states
 * STATE_NOT_INIT
 *		Not initialised. Before first call to hst_init() or after hst_deinit().
 * STATE_ERR_INTERNAL
 *      Some internal error.
 * STATE_ERR_SYSTEM
 *      Error occured while calling system api function.
 * STATE_ERR_NO_MEM
 *      Not enough memory.
 * STATE_CONN_CLOSED
 *      Connection is closed by client unexpectedly.
 * STATE_ERR_BAD_REQUEST
 *      Some error in request found.
 * STATE_CFG
 *      Configuration phase. After call to hst_init().
 * STATE_IDLE
 *		Doing nothing. After sending a reply.
 * STATE_REPLY
 *		Waiting for a reply. After a call to hst_get().
 * STATE_REPLY_CHUNK
 *		Doing chunked reply. After a call to hst_body_begin().
 */
typedef enum _hst_state {
    STATE_NOT_INIT,
    STATE_ERR_INTERNAL,
    STATE_ERR_SYSTEM,
    STATE_ERR_NO_MEM,
    STATE_ERR_CONN_CLOSED,
    STATE_ERR_BAD_REQUEST,
    STATE_CFG,
    STATE_IDLE,
    STATE_REPLY,
    STATE_REPLY_CHUNK
} hst_state_t;


static struct _hst {
    char *buf;          // buffer for read-write operations
    hst_req_t *req;		// current request

    hst_state_t state;	// module state
    int ss;				// server socket descriptor
    int sc;				// client socket descriptor
    int blen;           // buffer length
    int bds;            // start of current portion of data in buffer
    int bde;            // end(+1) of current portion of data in buffer
    int bts;            // start of current token in buffer
    int bte;            // end(+1) of current token in buffer

    size_t checkpoint;  // memory checkpoint
} me;


static const char *request_headers[COUNT_OF_HSTHDRID] = {
    [HSTHDRID_ACCEPT]           = "Accept",
    [HSTHDRID_ACCEPT_ENCODING]  = "Accept-Encoding",
    [HSTHDRID_ACCEPT_LANGUAGE]  = "Accept-Language",
    [HSTHDRID_CONNECTION]       = "Connection",
    [HSTHDRID_HOST]             = "Host",
    [HSTHDRID_USER_AGENT]       = "User-Agent"
};


/****************************************************************************
* hst private methods
****************************************************************************/

//!
static void _hst_set_idle(void) {
    me.state = STATE_IDLE;
    memset(me.req, 0, sizeof(*me.req));
    mem_checkpoint_restore(me.checkpoint);
}


// case-sensitive compare of token and zero-terminated string
static int _hst_cmp_token_and_strz(const char *psz) {
    int i, ret;
    for (i=0, ret=0; ret==0; i++) {
        if (!psz[i]) {ret = me.bte-(me.bts+i); break;}
        if (me.bts+i >= me.bte) {ret = -1; break;}
        ret = (me.buf[me.bts+i] - psz[i]);
    }
    return ret;
}


// case-insensitive compare of token and zero-terminated string
static int _hst_cmpi_token_and_strz(const char *psz) {
    int i, ret;
    for (i=0, ret=0; ret==0; i++) {
        if (!psz[i]) {ret = me.bte-(me.bts+i); break;}
        if (me.bts+i >= me.bte) {ret = -1; break;}
        ret = ((me.buf[me.bts+i] | 0x20) - (psz[i] | 0x20));
    }
    return ret;
}


// add data from socket to buffer
static int _hst_buffer_add(void) {
    int res, ret = HST_RES_ERR;

    // if already handled data are present in buffer then discard it
    if (me.bds) {
        int len = me.bde - me.bds;
        memcpy(me.buf, me.buf+me.bds, (uint)len);
        me.bds = 0;
        me.bde = me.blen = len;
    }

    // wait for data
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(me.sc, &readfds);
    struct timeval tv;
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    res = select(me.sc+1, &readfds, NULL, NULL, &tv);
    if (res == -1) {
        ERROR("%s.", strerror(errno));
        me.state = STATE_ERR_SYSTEM;
        goto exit;
    }
    if (res == 0) {
        ret = HST_RES_TIMEOUT;
        goto exit;
    }
    if (!FD_ISSET(me.sc, &readfds)) {
        ERROR("Unknown error.");
        me.state = STATE_ERR_INTERNAL;
        goto exit;
    }

    // fill the rest of buffer from socket
    ssize_t s = recv(me.sc, me.buf+me.blen, (size_t)(RWBUF_SIZE-me.blen), 0);
    if (s < 0) {
        ERROR("%s.", strerror(errno));
        close(me.sc);
        me.sc = -1;
        me.state = STATE_ERR_SYSTEM;
        goto exit;
    } else if (s == 0) {
        ERROR("Connection unexpectedly closed by client.");
        shutdown(me.sc, SHUT_RDWR);
        close(me.sc);
        me.sc = -1;
        me.state = STATE_ERR_CONN_CLOSED;
        goto exit;
    }
    me.blen += s;
    ret = HST_RES_OK;

exit:
    return ret;
}


// search buffer for a new line terminated with CRLF
static int _hst_line_get(void) {
    int ret;

    // discard already handled data
    me.bds = me.bde;

    for (;;) {
        // scan until eol or end of buffer is reached
        while (me.bde < me.blen) {
            if (me.buf[me.bde++] == '\r') {
                if (me.bde >= me.blen) {
                    ret = _hst_buffer_add();
                    if (ret != HST_RES_OK) goto exit;
                }
                if (me.buf[me.bde] != '\n') {
                    me.state = STATE_ERR_BAD_REQUEST;
                    goto exit;
                }
                me.bde++;
                ret = HST_RES_OK;
                goto exit;
            }
        }
        if (me.blen == RWBUF_SIZE) {
            ERROR("Line does not fit in buffer.");
            goto exit;
        }
        ret = _hst_buffer_add();
        if (ret != HST_RES_OK) goto exit;
    }

exit:
    return ret;
}


static char *_hst_create_strz(char *p, int len) {
    char *ret = NULL;
    ret = mem_alloc((size_t)(len+1));
    if (!ret) {
        me.state = STATE_ERR_NO_MEM;
        goto exit;
    }
    memcpy(ret, p, (size_t)len);
    ret[len] = 0;
exit:
    return ret;
}


static int _hst_parse_request_line(void) {
    int i, ret = HST_RES_ERR;
    char *p;

    // get method token
    for (me.bts=me.bte=me.bds; ; me.bte++) {
        if (me.buf[me.bte] == '\r') goto exit;
        if (me.buf[me.bte] == ' ') break;
    }

    // parse method type
    if (0 == _hst_cmp_token_and_strz("GET")) {
        me.req->method_get = true;
    } else if (0 == _hst_cmp_token_and_strz("POST")) {
        me.req->method_post = true;
    } else if (0 == _hst_cmp_token_and_strz("HEAD")) {
        me.req->method_head = true;
    } else {
        goto exit;
    }

    // get request-target
    me.bte++;
    for (me.bts=me.bte; ; me.bte++) {
        if (me.buf[me.bte] == '\r') goto exit;
        if (me.buf[me.bte] == ' ') break;
    }
    p = _hst_create_strz(me.buf+me.bts, me.bte-me.bts);
    if (!p) goto exit;
    me.req->request_target = p;

    // parse path part of request-target
    hst_path_elt_t **last_path_elt = &me.req->path_elt_first;
    if (*p && *p != '/') {
        me.state = STATE_ERR_BAD_REQUEST;
        goto exit;
    }
    for (p++,i=0; ; ) {
        bool slash = (p[i] == '/');
        bool end = (!p[i] || p[i] == '?');
        if ((slash || end) && i) {
            hst_path_elt_t *e = mem_alloc(sizeof(*e));
            if (e == NULL) goto exit;
            memset(e, 0, sizeof(*e));
            e->name = _hst_create_strz(p, i);
            if (e->name == NULL) goto exit;
            *last_path_elt = e;
            last_path_elt = &e->next;
        }
        if (slash) {p += i+1; i=0; continue;}
        if (end) break;
        i++;
    }

    // todo: parse query part of request-target

    // todo: parse http version

    // discard request line and return OK
    me.bds = me.bde;
    ret = HST_RES_OK;

exit:
    return ret;
}


static int _hst_parse_header_line(void) {
    int i, res, ret = HST_RES_ERR;

    // get header name
    for (me.bts=me.bte=me.bds; ; me.bte++) {
        if (me.buf[me.bte] == '\r') goto exit;
        if (me.buf[me.bte] == ':') break;
    }

    // binary search
    hst_hdr_id_t hi = COUNT_OF_HSTHDRID;
    int first=0, last=COUNT_OF_HSTHDRID;
    while (first < last) {
        i = (first + last) / 2;
        res = _hst_cmpi_token_and_strz(request_headers[i]);
        if (res < 0) last = i;
        else if (res > 0) first = i+1;
        else {
            hi = (hst_hdr_id_t)i;
            break;
        }
    }

    // if found - handle it, if not - nothing to do
    if (hi < COUNT_OF_HSTHDRID) {
        // todo: implement
    } else {
        // debug print
        const char *p = _hst_create_strz(me.buf+me.bds, me.bde-me.bds);
        printf("DEBUG: unknown header: %s\n", p);
    }

    ret = HST_RES_OK;

exit:
    return ret;
}


/****************************************************************************
* hst public methods
****************************************************************************/

//!
int hst_init(hst_conf_t *conf) {
    int res, ret = HST_RES_ERR;

    if (me.state != STATE_NOT_INIT) {
        ERROR("Already initialized.");
        goto exit;
    }

    // set module configuration
    hst_conf_t c;
    if (conf) c = *conf;
    else memset(&c, 0, sizeof(c));
    if (!c.backlog) c.backlog = DFLT_CONF_BACKLOG;
    if (!c.port) c.port = DFLT_CONF_PORT;
    if (c.mem_total < DFLT_CONF_MEM_TOTAL) c.mem_total = DFLT_CONF_MEM_TOTAL;

    // init memory allocator
    res = mem_init(c.mem_total);
    if (res != HST_RES_OK) goto exit;

    // allocate rw buffer
    me.buf = mem_alloc(RWBUF_SIZE);
    if (me.buf == NULL) goto exit;
    memset(me.buf, 0, RWBUF_SIZE);

    // allocate request object
    me.req = mem_alloc(sizeof(*me.req));
    if (me.req == NULL) goto exit;
    memset(me.req, 0, sizeof(*me.req));

    me.ss = -1;
    me.sc = -1;
    me.blen = 0;
    me.bds = 0;
    me.bde = 0;

    me.ss = socket(AF_INET, SOCK_STREAM, 0);
    if (me.ss == -1) {
        ERROR("%s.", strerror(errno));
        goto exit;
    }
    int yes = 1;
    res = setsockopt(me.ss, SOL_SOCKET, SO_REUSEADDR, (char*)&yes, sizeof(yes));
    if (res == -1) {
        ERROR("%s.", strerror(errno));
        goto exit;
    }
    res = ioctl(me.ss, FIONBIO, (char *)&yes);
    if (res == -1) {
        ERROR("%s.", strerror(errno));
        goto exit;
    }
    struct sockaddr_in hstaddr;
    memset(&hstaddr, 0, sizeof(hstaddr));
    hstaddr.sin_family = AF_INET;
    hstaddr.sin_addr.s_addr = c.addr;
    hstaddr.sin_port = c.port;
    res = bind(me.ss, (struct sockaddr *)&hstaddr, sizeof(hstaddr));
    if (res == -1) {
        ERROR("%s.", strerror(errno));
        goto exit;
    }
    res = listen(me.ss, c.backlog);
    if (res == -1) {
        ERROR("%s.", strerror(errno));
        goto exit;
    }

    me.state = STATE_CFG;
    ret = HST_RES_OK;

exit:
    return ret;
}


void hst_deinit(void) {
    if (me.state == STATE_NOT_INIT)
        return;
    mem_deinit();
    memset(&me, 0, sizeof(me));
}


int hst_read(hst_req_t **req) {
    int res, ret = HST_RES_ERR;

    if (me.state == STATE_CFG) {
        me.state = STATE_IDLE;
        me.checkpoint = mem_checkpoint_get();
    }

    if (me.state != STATE_IDLE) {
        ERROR("Wrong state %d.", me.state);
        goto exit;
    }

    // wait until client connects or timeout expires
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(me.ss, &readfds);
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    res = select(me.ss+1, &readfds, NULL, NULL, &tv);
    if (res == -1) {
        ERROR("%s.", strerror(errno));
        me.state = STATE_ERR_SYSTEM;
        goto exit;
    }
    if (res == 0) {
        ret = HST_RES_TIMEOUT;
        goto exit;
    }
    if (!FD_ISSET(me.ss, &readfds)) {
        ERROR("Unknown error.");
        me.state = STATE_ERR_INTERNAL;
        goto exit;
    }

    // get client socket and make it nonblocking
    me.sc = accept(me.ss, NULL, 0);
    if (me.sc == -1) {
        ERROR("%s.", strerror(errno));
        me.state = STATE_ERR_SYSTEM;
        goto exit;
    }
    int yes = 1;
    res = ioctl(me.sc, FIONBIO, (char *)&yes);
    if (res == -1) {
        ERROR("%s.", strerror(errno));
        goto exit;
    }

    // parse request line
    ret = _hst_line_get();
    if (ret != HST_RES_OK) goto exit;
    ret = _hst_parse_request_line();
    if (ret != HST_RES_OK) goto exit;

    // parse headers
    int i;
    for (i=0; ; i++) {
        ret = _hst_line_get();
        if (ret != HST_RES_OK) goto exit;
        if (me.bde - me.bds < 3) break; // empty line
        ret = _hst_parse_header_line();
        if (ret != HST_RES_OK) goto exit;
    }
    me.bds = me.bde;

    me.state = STATE_REPLY;
    *req = me.req;
    ret = HST_RES_OK;

exit:
    if (ret != HST_RES_OK) _hst_set_idle();
    return ret;
}


int hst_write_res(int code) {
    int ret = HST_RES_ERR;

    // todo: implement method
    (void)code;

    return ret;
}


int hst_write_hdr(const char *name, const char *val) {
    int ret = HST_RES_ERR;

    // todo: implement method
    (void)name;
    (void)val;

    return ret;
}


int hst_write_body_all(void *ptr, int size) {
    int res, ret = HST_RES_ERR;

    // todo: implement method
    (void)ptr;
    (void)size;

    if (me.state != STATE_REPLY) {
        ERROR("Wrong state.");
        goto exit;
    }

    const char *reply = "HTTP/1.1 200 OK\n\n";
    res = (int)send(me.sc, reply, strlen(reply), 0);
    shutdown(me.sc, SHUT_RDWR);
    close(me.sc);
    me.sc = -1;
    _hst_set_idle();
    ret = HST_RES_OK;

exit:
    return ret;
}


int hst_write_body_begin(bool buffer) {
    int ret = HST_RES_ERR;

    (void)buffer;

    return ret;
}


int hst_write_body_data(void *ptr, int size) {
    int ret = HST_RES_ERR;

    (void)ptr;
    (void)size;

    return ret;
}


int hst_write_body_print(const char *str) {
    int ret = HST_RES_ERR;

    (void)str;

    return ret;
}


int hst_write_body_printf(const char *format, ...) {
    int ret = HST_RES_ERR;

    (void)format;

    return ret;
}


int hst_write_body_end(void) {
    int ret = HST_RES_ERR;

    return ret;
}
