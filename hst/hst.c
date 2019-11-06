#include "hst.h"
#include <sys/ioctl.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


#pragma GCC diagnostic ignored "-Wformat-nonliteral"


typedef unsigned int uint;


// Error reporting macro. Disable or replace it if needed.
#if 1
    #define ERROR(fmt, ...) fprintf(stderr, \
            "ERROR: %s():%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)
#else
    #define ERROR(fmt, ...) ((void)0)
#endif


// Result codes for internal use.
#define HST_RES_TIMEOUT     (-11)
#define HST_RES_INTERNAL    (-12)
#define HST_RES_BADREQUEST  (-13)
#define HST_RES_DISCONNECT  (-14)


/****************************************************************************
* Memory allocator.
****************************************************************************/

//!
// Memory allocator object.
static struct _mem {
    char *start;        // start of memory allocated by malloc()
    int total;          // total amount of memory
    int current;        // current amount of memory allocated by this allocator
} mem;


static int mem_init(int size) {
    if (mem.start) {
        ERROR("Memory already initialised.");
        return HST_RES_ERR;
    }
    mem.start = malloc((size_t)size);
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


static void *mem_alloc(int size) {
    void *ret = NULL;
    // align up
    int current = mem.current + (-mem.current & (int)(sizeof(void*)-1));
    if (current + size > mem.total) {
        ERROR("Not enough memory. Requested %u bytes.", (uint)size);
    } else {
        ret = mem.start + current;
        mem.current = current + size;
    }
    return ret;
}


//static void *mem_alloc_all(int *size) {
//    // align up
//    mem.current += (int)(-mem.current & (int)(sizeof(void*)-1));
//    *size = mem.current > mem.total ? 0 : mem.total - mem.current;
//    return mem.start + mem.current;
//}


static int mem_grow(int size) {
    if (mem.current + size > mem.total)
        return HST_RES_ERR;
    mem.current += size;
    return HST_RES_OK;
}


static inline int mem_checkpoint_get(void) {
    return mem.current;
}


static inline void mem_checkpoint_restore(int checkpoint) {
    mem.current = checkpoint;
}


/****************************************************************************
* Buffer.
****************************************************************************/

//!
typedef struct _buf_t {
    char *buf;      // ptr to buffer
    int tot;        // total size of buffer
    int len;        // length of data in buffer
    int sta;        // start of not yet handled data
    char padding[4];
} buf_t;


// Allocate memory for buffer.
int buf_alloc(buf_t *buf, int size) {
    char *b = mem_alloc(size);
    if (b == NULL) return HST_RES_ERR;

    buf->buf = b;
    buf->tot = size;
    buf->len = 0;
    buf->sta = 0;
    return HST_RES_OK;
}


// Grow memory, previously allocated for buffer.
// Works only if this buffer allocation was a last memory allocation.
int buf_grow(buf_t *buf, int size) {
    if (HST_RES_OK != mem_grow(size))
        return HST_RES_INTERNAL;

    buf->tot += size;
    return HST_RES_OK;
}


int buf_add(buf_t *buf, const void *ptr, int size) {
    int free = buf->tot - buf->len;
    if (size > free) {
        int res = buf_grow(buf, size-free);
        if (res != HST_RES_OK) return res;
    }
    memcpy(buf->buf+buf->len, ptr, (uint)size);
    buf->len += size;
    return HST_RES_OK;
}


int buf_shift(buf_t *buf) {
    if (buf->sta == 0)
        return HST_RES_ERR;

    int len = buf->len - buf->sta;
    memcpy(buf->buf, buf->buf+buf->sta, (uint)len);
    buf->sta = 0;
    buf->len = len;
    return HST_RES_OK;
}


int buf_printf(buf_t *buf, const char *fmt, ...) {
    va_list v;
    va_start(v, fmt);
    int len = buf->tot - buf->len;
    int res = vsnprintf(buf->buf+buf->len, (uint)len, fmt, v);
    va_end(v);
    if (res < 0) {
        ERROR("%s.", strerror(errno));
        return HST_RES_ERR;
    } else if (res >= len) {
        ERROR("String too big.");
        return HST_RES_ERR;
    }
    buf->len += res;
    return HST_RES_OK;
}


/****************************************************************************
* Token.
****************************************************************************/

//!
typedef struct _tok_t {
    const char *ptr;    // token start
    int len;            // token length
    char padding[4];
} tok_t;


// Case-sensitive compare of token and zero-terminated string.
int tok_cmp_strz(tok_t *tok, const char *psz) {
    int i, ret;
    for (i=0, ret=0; ret==0; i++) {
        if (!psz[i]) {ret = tok->len - i; break;}
        if (i >= tok->len) {ret = -1; break;}
        ret = (tok->ptr[i] - psz[i]);
    }
    return ret;
}


// Case-insensitive compare of token and zero-terminated string.
int tok_cmpi_strz(tok_t *tok, const char *psz) {
    int i, ret;
    for (i=0, ret=0; ret==0; i++) {
        if (!psz[i]) {ret = tok->len - i; break;}
        if (i >= tok->len) {ret = -1; break;}
        ret = ((tok->ptr[i] | 0x20) - (psz[i] | 0x20));
    }
    return ret;
}


/****************************************************************************
* Html template system.
****************************************************************************/

//!
// Mapping between function name used in html template
// and corresponding c function.
typedef struct _hst_tpl_fdesc_t {
    struct _hst_tpl_fdesc_t *next;  // next element in list
    const char *name;               // template function name used in html
    hst_tpl_func_t func;            // ptr to corresponding c function
} hst_tpl_fdesc_t;


// Template element.
typedef struct _hst_tpl_elt_t {
    // next element in list
    struct _hst_tpl_elt_t *next;

    // if 0, then next union holds ptr to template function description
    // else next union holds ptr to html text and size is it`s size
    int size;
    char padding[4];

    union {
        const char *text;           // ptr to html text
        hst_tpl_fdesc_t *fd;        // ptr to template function description
    };
} hst_tpl_elt_t;


/****************************************************************************
* Hst.
****************************************************************************/

// Default values for 'hst_conf_t' fields.
#define DFLT_CONF_PORT          80
#define DFLT_CONF_BACKLOG       32
#define DFLT_CONF_MEM_TOTAL     (32*1024)


/* Constants.
 *
 * HBUF_SIZE
 *      Size of buffer for http request/reply headers.
 *      note: rfc7230: "It is RECOMMENDED that all HTTP senders and recipients
 *      support, at a minimum, request-line lengths of 8000 octets."
 * HREAD_SIZE
 *      Amount of data added to headers buffer per one read operation.
 *      It is relatively small to reduce size of body that goes to headers
 *      buffer if there is a body present in request.
 * CHUNK_SIZE
 *      Maximum chunk size for chunked transfer of reply body.
 */
#define HBUF_SIZE               (8*1024)
#define HREAD_SIZE              256
#define CHUNK_SIZE              (4*1024)


/* HST states.
 *
 * STATE_NOT_INIT
 *      Not initialised. It is set before first call to hst_init()
 *      or after a call to hst_deinit().
 * STATE_CFG
 *      Configuration. It is set after a call to hst_init().
 * STATE_READ
 *      Ready to read new request.
 * STATE_WR_RES
 *      Ready to write result code.
 * STATE_WR_HDR
 *      Ready to write reply headers.
 * STATE_WR_BODY
 *      Writing reply body. If there is enough memory, then 'Content-Length'
 *      header will be generated and no chunked transfer will be used.
 * STATE_WR_BODY_CHUNKED
 *      Reply body does not fit in hst memory. Switched to chunked transfer.
 * STATE_WR_ERROR
 *      Error happened during write operation.
 */
typedef enum _hst_state {
    STATE_NOT_INIT,
    STATE_CFG,
    STATE_READ,
    STATE_WR_RES,
    STATE_WR_HDR,
    STATE_WR_BODY,
    STATE_WR_BODY_CHUNKED,
    STATE_WR_ERROR
} hst_state_t;


static struct _hst {
    hst_state_t state;  // module state
    int checkpoint;     // memory checkpoint
    int ss;             // server socket descriptor
    int sc;             // client socket descriptor

    hst_req_t *req;     // current request

    buf_t hbuf;         // buffer for request/reply headers
    buf_t bbuf;         // buffer for request/reply body

    int body_len;       // body length
    int body_chunked;   // chunked transfer-encoding flag

    hst_tpl_fdesc_t *fdesc_first;  // ptr to first
} me;


// Create zero terminated string in hst memory.
static char *_hst_create_strz(const char *p, int len) {
    char *ret = mem_alloc(len+1);
    if (ret) {
        memcpy(ret, p, (size_t)len);
        ret[len] = 0;
    }
    return ret;
}


// Search for template function name and add one if not found.
// return:
//      if (returned value == NULL)
//          error occured; *found is not modified
//      else if (*found == true)
//          returned value points to found element
//      else
//          returned value points to added element
static hst_tpl_fdesc_t *_hst_tpl_function_add(const char *name, bool *found) {
    // search
    hst_tpl_fdesc_t **p = &me.fdesc_first;  // previous
    hst_tpl_fdesc_t *c = me.fdesc_first;    // current
    for (; c; p=&c->next, c=c->next) {
        if (0 == strcmp(name, c->name)) {
            *found = true;
            return c;
        }
    }

    // create new list element
    hst_tpl_fdesc_t *n = mem_alloc(sizeof(*n));
    if (n == NULL) {  // error occured
        return NULL;
    }
    n->name = name;
    n->func = NULL;
    n->next = NULL;
    *p = n;
    *found = false;
    return n;
}


// Check if client socket is ready for read or write.
static int _hst_is_socket_ready(bool read) {
    fd_set readfds, writefds, *r=NULL, *w=NULL;
    if (read) {
        r = &readfds;
        FD_ZERO(r);
        FD_SET(me.sc, r);
    } else {
        w = &writefds;
        FD_ZERO(w);
        FD_SET(me.sc, w);
    }

    struct timeval tv;
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    int res = select(me.sc+1, r, w, NULL, &tv);
    if (res < 0) {
        ERROR("%s.", strerror(errno));
        return HST_RES_ERR;
    }
    if (res == 0) {
        ERROR("Write timed out.");
        return HST_RES_TIMEOUT;
    }

    bool error = false;
    if (read) {
        if (!FD_ISSET(me.sc, r)) error = true;
    } else {
        if (!FD_ISSET(me.sc, w)) error = true;
    }
    if (error) {
        ERROR("Unknown error.");
        return HST_RES_ERR;
    }

    return HST_RES_OK;
}


/* Add data from socket to buffer.
 * May add fewer bytes than requested.
 *
 * Input:
 *      buf - ptr to buffer
 *      num - number of bytes to add
 * Return:
 *      > 0 - number of bytes added
 *      HST_RES_ERR - critical error
 *      HST_RES_INTERNAL - no memory
 *      HST_RES_BADREQUEST - bad request detected
 *      HST_RES_DISCONNECT - client disconnected
 */
static int _hst_buf_add(buf_t *buf, int num) {
    int res, ret = HST_RES_ERR;

    // check size
    if (num > buf->tot - buf->len) {
        buf_shift(buf);
        if (num > buf->tot - buf->len) {
            num = buf->tot - buf->len;
            if (num <= 0) {
                ERROR("No space in buffer.");
                ret = HST_RES_INTERNAL;
                goto exit;
            }
        }
    }

    // wait for data
    res = _hst_is_socket_ready(true);
    if (res == HST_RES_TIMEOUT) {
        // timeout considered equal to connection close
        ret = HST_RES_DISCONNECT;
        goto exit;
    } else if (res != HST_RES_OK)
        goto exit;

    // read from socket
    ssize_t s = recv(me.sc, buf->buf+buf->len, (size_t)num, 0);
    if (s < 0) {
        ERROR("%s.", strerror(errno));
        goto exit;
    } else if (s == 0) {  // connection closed
        ret = HST_RES_DISCONNECT;
        goto exit;
    }
    buf->len+= s;
    ret = num;

exit:
    return ret;
}


/* Ensure that buffer contains a line terminated with CRLF.
 * If needed, read data to buffer from client socket.
 *
 * Input:
 *      buf - buffer
 * Return:
 *      > 0 - line length (including CRLF)
 *      HST_RES_ERR - critical error
 *      HST_RES_BADREQUEST - line expected but connection is closed
 */
static int _hst_line_get(buf_t *buf) {
    int len = 0;
    for (;;) {
        for (; buf->sta + len < buf->len - 1; len++) {
            if (buf->buf[buf->sta+len] == '\r'
                     && buf->buf[buf->sta+len+1] == '\n') {
                return len+2;
            }
        }
        if (buf->len == buf->tot) {
            ERROR("Line does not fit in buffer.");
            return HST_RES_INTERNAL;
        }
       int res = _hst_buf_add(buf, HREAD_SIZE);
       if (res == 0) {  // connection is closed
           return HST_RES_BADREQUEST;
       } else if (res < 0)
           break;
    }
    return HST_RES_ERR;
}


// Parse http headers.
static int _hst_parse_headers(void) {
    int i, linelen, ret = HST_RES_ERR;
    buf_t *buf = &me.hbuf;
    char *p;

    tok_t tok1, tok2;
    int ts, te;         // token start index, token end index

    // get first line - it is a request line
    linelen = _hst_line_get(&me.hbuf);
    if (linelen <= 0) goto exit;

    // get method token
    for (ts=te=buf->sta; ; te++) {
        if (buf->buf[te] == '\r') goto exit;
        if (buf->buf[te] == ' ') break;
    }
    tok1.ptr = buf->buf + ts;
    tok1.len = te - ts;

    // parse method type
    if (0 == tok_cmp_strz(&tok1, "GET")) {
        me.req->method_get = true;
    } else if (0 == tok_cmp_strz(&tok1, "POST")) {
        me.req->method_post = true;
    } else if (0 == tok_cmp_strz(&tok1, "HEAD")) {
        me.req->method_head = true;
    } else {
        goto exit;
    }

    // get request-target
    te++;
    for (ts=te; ; te++) {
        if (buf->buf[te] == '\r') goto exit;
        if (buf->buf[te] == ' ') break;
    }
    tok1.ptr = buf->buf + ts;
    tok1.len = te - ts;
    p = _hst_create_strz(tok1.ptr, tok1.len);
    if (!p) goto exit;
    me.req->request_target = p;

    // parse path part of request-target
    hst_path_elt_t **last_path_elt = &me.req->path_elt_first;
    if (*p && *p != '/')
        goto exit;
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

    // parse headers
    bool hdr_content_len = false;
    bool hdr_transfer_enc = false;
    hst_hdr_t **last_hdr = &me.req->hdr_first;
    for (;;) {
        // discard previous line
        me.hbuf.sta += linelen;

        // get new line
        linelen = _hst_line_get(&me.hbuf);
        if (linelen <= 0) goto exit;

        // empty line is a sign of headers section end
        if (linelen < 3) {
            me.hbuf.sta += linelen;
            break;
        }

        // get header name
        for (ts=te=buf->sta; ; te++) {
            if (buf->buf[te] == '\r') goto exit;
            if (buf->buf[te] == ':') break;
        }
        tok1.ptr = buf->buf + ts;
        tok1.len = te - ts;
        if (tok1.len == 0) goto exit;

        // get header value
        for (ts=te+1; ; ts++)  // skip white space
            if (buf->buf[ts] != ' ' && buf->buf[ts] != '\t')
                break;
        for (te=ts; ; te++)  // value token
            if (buf->buf[te] == '\r')
                break;
        tok2.ptr = buf->buf + ts;
        tok2.len = te - ts;
        if (tok2.len == 0) goto exit;

        // create header
        hst_hdr_t *hdr = mem_alloc(sizeof(*hdr));
        if (hdr == NULL) goto exit;
        hdr->next = NULL;
        hdr->name = _hst_create_strz(tok1.ptr, tok1.len);
        if (hdr->name == NULL) goto exit;
        hdr->value = _hst_create_strz(tok2.ptr, tok2.len);
        if (hdr->value == NULL) goto exit;

        // add header to request
        *last_hdr = hdr;
        last_hdr = &hdr->next;

        // handle 'Content-Length' header
        if (0 == strcasecmp(hdr->name, "Content-Length")) {
            hdr_content_len = true;
            me.body_len = atoi(hdr->value);
        }

        // handle 'Transfer-Encoding' header
        if (0 == strcasecmp(hdr->name, "Transfer-Encoding")) {
            hdr_transfer_enc = true;
            if (strstr(hdr->value, "chunked"))
                me.body_chunked = 1;
        }
    }

    // If both 'Content-Length' and 'Transfer-Encoding' are set,
    // then this is an error (rfc7230 3.3.3).
    if (hdr_content_len && hdr_transfer_enc) {
        ERROR("Length and chunked.");
        ret = HST_RES_BADREQUEST;
        goto exit;
    }

    ret = HST_RES_OK;

exit:
    return ret;
}


// Write data to client socket.
static int _hst_write(const void *ptr, int size) {
    int ret = HST_RES_ERR;

    while (size) {
        ret = _hst_is_socket_ready(false);
        if (ret != HST_RES_OK) goto exit;

        ret = (int)send(me.sc, ptr, (size_t)size, 0);
        if (ret < 0) {
            ERROR("%s.", strerror(errno));
            goto exit;
        }
        size -= ret;
    }
    ret = HST_RES_OK;

exit:
    return ret;
}


// Write chunk to client socket.
static int _hst_write_chunk(const void *ptr, int size) {
    char buf[32];
    int res = snprintf(buf, sizeof(buf), "%d\r\n", size);
    if (res < 0) {
        ERROR("%s.", strerror(errno));
        return HST_RES_ERR;
    }
    res = _hst_write(buf, res);
    if (res != HST_RES_OK) return HST_RES_ERR;
    res = _hst_write(ptr, size);
    if (res != HST_RES_OK) return HST_RES_ERR;
    return HST_RES_OK;
}


static int _hst_write_body_init(void) {
    if (me.state == STATE_WR_HDR) {
        // allocate buffer for reply body
        int res = buf_alloc(&me.bbuf, CHUNK_SIZE);
        if (res != HST_RES_OK) goto exit;
        me.state = STATE_WR_BODY;
    }

    if (me.state == STATE_WR_BODY ||
            me.state == STATE_WR_BODY_CHUNKED)
        return HST_RES_OK;

exit:
    return HST_RES_ERR;
}


static int _hst_write_body_begin_chunked(void) {
    int ret;

    // add transfer-encoding header and end-of-headers sign
    ret = buf_printf(&me.hbuf, "Transfer-Encoding: chunked\r\n\r\n");
    if (ret != HST_RES_OK) goto exit;

    // send headers
    ret = _hst_write(me.hbuf.buf, me.hbuf.len);
    if (ret != HST_RES_OK) goto exit;

    // write out body as chunks
    for (; me.bbuf.len-me.bbuf.sta > CHUNK_SIZE; me.bbuf.sta+=CHUNK_SIZE) {
        ret = _hst_write_chunk(me.bbuf.buf+me.bbuf.sta, CHUNK_SIZE);
        if (ret != HST_RES_OK) goto exit;
    }
    buf_shift(&me.bbuf);
    me.state = STATE_WR_BODY_CHUNKED;

exit:
    return ret;
}


/* Set internal state to STATE_WR_ERROR and send 500 reply.
 * In case of errors, do as much as possible without error reporting.
 */
static void _hst_write_error(void) {
    if (me.state == STATE_WR_ERROR)
        return;

    if (me.sc != -1 && me.state != STATE_WR_BODY_CHUNKED) {
        int res = _hst_is_socket_ready(false);
        if (res == HST_RES_OK) {
            const char *p = "HTTP/1.1 500 Internal server error\r\n\r\n";
            send(me.sc, p, 38, 0);
        }
    }

    shutdown(me.sc, SHUT_RDWR);
    close(me.sc);
    me.sc = -1;
    me.state = STATE_WR_ERROR;
}


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

    // allocate headers buffer
    res = buf_alloc(&me.hbuf, HBUF_SIZE);
    if (res != HST_RES_OK) goto exit;

    // allocate request object
    me.req = mem_alloc(sizeof(*me.req));
    if (me.req == NULL) goto exit;

    me.ss = -1;
    me.sc = -1;

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

    if (me.sc != -1)
        close(me.sc);
    if (me.ss != -1)
        close(me.ss);
    mem_deinit();
    memset(&me, 0, sizeof(me));  // implicitly set me.state to STATE_NOT_INIT
}


int hst_tpl_function(const char *name, hst_tpl_func_t func) {
    if (me.state != STATE_CFG) {
        ERROR("Wrong state %d.", me.state);
        return HST_RES_ERR;
    }

    bool found = false;
    hst_tpl_fdesc_t *fd = _hst_tpl_function_add(name, &found);
    if (fd == NULL) return HST_RES_ERR;
    if (found && fd->func) {
        ERROR("Template function is already declared.");
        return HST_RES_ERR;
    }
    fd->func = func;
    return HST_RES_OK;
}


hst_tpl_t *hst_tpl_compile(const char *psz) {
    char name[256];
    int i;

    if (me.state != STATE_CFG) {
        ERROR("Wrong state %d.", me.state);
        goto exit;
    }

    hst_tpl_elt_t *first = NULL;    // first
    hst_tpl_elt_t *curr = NULL;     // current
    hst_tpl_elt_t **prev = &first;  // previous

    for (;;) {
        char *p = strstr(psz, "<!--hst ");
        if (p == NULL) break;

        // create template element of type "html text"
        curr = mem_alloc(sizeof(*curr));
        if (curr == NULL) goto exit;
        curr->next = NULL;
        curr->text = psz;
        curr->size = (int)(p - psz);
        *prev = curr;
        prev = &curr->next;

        // get template function name
        for (p+=8; *p==' '; p++)
            continue;
        for (i=0; *p && *p!=' ' && *p!='-' && i+1<(int)sizeof(name); )
            name[i++] = *p++;
        for (; *p && *p!='>'; p++)
            continue;
        if (*p == 0) goto exit;
        name[i] = 0;
        psz = p + 1;

        // create template element of type "template function"
        bool found = false;
        hst_tpl_fdesc_t *fd = _hst_tpl_function_add(name, &found);
        if (fd == NULL) goto exit;
        if (!found) {  // if added, then must allocate space for name
            fd->name = _hst_create_strz(name, i);
            if (fd->name == NULL) goto exit;
        }
        curr = mem_alloc(sizeof(*curr));
        if (curr == NULL) goto exit;
        curr->next = NULL;
        curr->size = 0;
        curr->fd = fd;
        *prev = curr;
        prev = &curr->next;
    }

    if ( (i=(int)strlen(psz)) ) {
        curr = mem_alloc(sizeof(*curr));
        if (curr == NULL) goto exit;
        curr->next = NULL;
        curr->text = psz;
        curr->size = i;
        *prev = curr;
    }

    return first;

exit:
    return NULL;
}


int hst_read(hst_req_t **req) {
    int res, ret = HST_RES_ERR;

    if (me.state == STATE_CFG) {
        me.checkpoint = mem_checkpoint_get();
        me.state = STATE_READ;
    } else if (me.state != STATE_READ) {
        ERROR("Wrong state %d.", me.state);
        goto exit;
    }

    if (me.sc != -1) {
        ERROR("Client socket should be closed.");
        goto exit;
    }

    // init data
    memset(me.req, 0, sizeof(*me.req));
    me.hbuf.len = 0;
    me.hbuf.sta = 0;
    me.bbuf.buf = NULL;
    me.bbuf.tot = 0;
    me.bbuf.len = 0;
    me.bbuf.sta = 0;
    me.body_len = 0;
    me.body_chunked = 0;
    mem_checkpoint_restore(me.checkpoint);

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
        goto exit;
    }
    if (res == 0) {
        ret = HST_RES_CONT;
        goto exit;
    }
    if (!FD_ISSET(me.ss, &readfds)) {
        ERROR("Unknown error.");
        goto exit;
    }

    // get client socket and make it nonblocking
    me.sc = accept(me.ss, NULL, 0);
    if (me.sc == -1) {
        ERROR("%s.", strerror(errno));
        goto exit;
    }
    int yes = 1;
    res = ioctl(me.sc, FIONBIO, (char *)&yes);
    if (res == -1) {
        ERROR("%s.", strerror(errno));
        goto exit;
    }

    // parse headers
    ret = _hst_parse_headers();
    if (ret != HST_RES_OK) goto exit;

    // read body
    if (me.body_len) {  // size is known
        // allocate body buffer
        int num = me.body_len;
        res = buf_alloc(&me.bbuf, num);
        if (res != HST_RES_OK) goto einternal;
        // copy from header buffer
        int n = me.hbuf.len - me.hbuf.sta;
        if (n > 0) {
            memcpy(me.bbuf.buf, me.hbuf.buf+me.hbuf.sta, (uint)n);
            me.bbuf.len += n;
        }
        // read from socket
        for (num-=n; num; num-=res) {
            res = _hst_buf_add(&me.bbuf, num);
            if (res < 0) {
                ret = res;
                goto exit;
            } else if (res == 0) {
                ret = HST_RES_BADREQUEST;
                goto exit;
            }
        }
    } else if (me.body_chunked) {  // chunked transfer is used
        // allocate body buffer
        res = buf_alloc(&me.bbuf, HREAD_SIZE);
        if (res != HST_RES_OK) goto einternal;
        // loop through chunks
        for (;;) {
            res = _hst_line_get(&me.hbuf);
            if (res < 0) {
                goto einternal;
            } else if (res == 0) {
                ret = HST_RES_BADREQUEST;
                goto exit;
            }
            // number of bytes in chunk
            errno = 0;
            long num = strtol(me.hbuf.buf+me.hbuf.sta, NULL, 10);
            if (errno || num < 0) {
                ret = HST_RES_BADREQUEST;
                goto exit;
            }
            // end of chunked transfer
            if (num == 0) break;
            // discard line
            for (;;)
                if (me.hbuf.buf[me.hbuf.sta++] == '\r')
                    break;
            me.hbuf.sta++;
            // copy from header buffer
            res = buf_grow(&me.bbuf, (int)num);
            if (res != HST_RES_OK) goto einternal;
            int n = me.hbuf.len - me.hbuf.sta;
            if (n > 0) {
                memcpy(me.bbuf.buf, me.hbuf.buf+me.hbuf.sta, (uint)n);
                me.bbuf.len += n;
                num -= n;
            }
            // read from socket
            while (num) {
                res = _hst_buf_add(&me.bbuf, (int)num);
                if (res <= 0) goto exit;
                num -= res;
            }
        }
    }
    if (me.bbuf.buf) {
        int zero = 0;
        ret = buf_add(&me.bbuf, &zero, 1);
        if (ret != HST_RES_OK) goto exit;
        me.req->body = me.bbuf.buf;
        me.req->body_len = me.bbuf.len - 1;  // zero not included
    }

    me.state = STATE_WR_RES;
    *req = me.req;
    ret = HST_RES_OK;

exit:
    if (ret == HST_RES_INTERNAL) {
        hst_write_res(500, "Internal server error.");
        hst_write_end();
        ret = HST_RES_CONT;
    } else if (ret == HST_RES_BADREQUEST) {
        hst_write_res(400, "Bad request");
        hst_write_end();
        ret = HST_RES_CONT;
    }
    return ret;

einternal:
    ret = HST_RES_INTERNAL;
    goto exit;
}


void hst_write_res(int code, const char *text) {
    if (me.state != STATE_WR_RES) {
        ERROR("Wrong state %d.", me.state);
        goto error;
    }

    // clear headers buffer and print result code
    me.hbuf.sta = 0;
    me.hbuf.len = 0;
    int res = buf_printf(&me.hbuf, "HTTP/1.1 %d %s\r\n", code, text);
    if (res != HST_RES_OK) goto error;

    me.state = STATE_WR_HDR;
    return;

error:
    _hst_write_error();
    return;
}


void hst_write_hdr(const char *name, const char *val) {
    if (me.state != STATE_WR_HDR) {
        ERROR("Wrong state %d.", me.state);
        goto error;
    }

    int res = buf_printf(&me.hbuf, "%s: %s\r\n", name, val);
    if (res != HST_RES_OK) goto error;

    return;

error:
    _hst_write_error();
    return;
}


int hst_write_tpl(const hst_tpl_t *tpl) {
    int res = _hst_write_body_init();
    if (res != HST_RES_OK) goto error;

    if ( (const char *)tpl < (const char *)mem.start ||
         (const char *)tpl > (const char *)mem.start + me.checkpoint ) {
        ERROR("Wrong parameter.");
        goto error;
    }

    for (; tpl; tpl=tpl->next) {
        if (tpl->size) {  // html text
            hst_write_body_data(tpl->text, tpl->size);
        } else {  // template function
            hst_tpl_func_t func = tpl->fd->func;
            if (func)
                func();
            else {
                hst_write_body_printf("<span>undefined template function: "
                                      "'%s'</span>", tpl->fd->name);
            }
        }
    }

    return hst_write_end();

error:
    _hst_write_error();
    return HST_RES_ERR;
}


void hst_write_body_data(const void *ptr, int size) {
    int res;

    res = _hst_write_body_init();
    if (res != HST_RES_OK) goto error;

    if (me.state == STATE_WR_BODY) {
        res = buf_add(&me.bbuf, ptr, size);
        if (res != HST_RES_OK) {
            res = _hst_write_body_begin_chunked();
            if (res != HST_RES_OK) goto error;
            goto chunked;
        }
        return;
    }

chunked:
    if (me.state == STATE_WR_BODY_CHUNKED) {
        while (size) {
            // write data to body
            int free = CHUNK_SIZE - me.bbuf.len;
            if (free > size) free = size;
            memcpy(me.bbuf.buf+me.bbuf.len, ptr, (uint)free);
            me.bbuf.len += free;
            ptr = (const char*)ptr + free;
            size -= free;

            // write chunk to client socket
            if (me.bbuf.len >= CHUNK_SIZE) {
                res = _hst_write_chunk(me.bbuf.buf, CHUNK_SIZE);
                if (res != HST_RES_OK) goto error;
                me.bbuf.sta = CHUNK_SIZE;
                buf_shift(&me.bbuf);
            }
        }
        return;
    }

error:
    _hst_write_error();
    return;
}


void hst_write_body_print(const char *strz) {
    hst_write_body_data(strz, (int)strlen(strz));
}


void hst_write_body_printf(const char *format, ...) {
    char buf[CHUNK_SIZE];

    va_list v;
    va_start(v, format);
    int res = vsnprintf(buf, sizeof(buf), format, v);
    va_end(v);
    if (res < 0) {
        ERROR("%s.", strerror(errno));
        goto error;
    } else if (res >= (int)sizeof(buf)) {
        ERROR("String too big.");
        goto error;
    }
    hst_write_body_data(buf, res);
    return;

error:
    _hst_write_error();
    return;
}


int hst_write_end(void) {
    int ret = HST_RES_ERR;

    if (me.state == STATE_WR_ERROR)
        goto exit;

    if (me.state == STATE_WR_HDR) {
        // add blank line to headers
        ret = buf_printf(&me.hbuf, "\r\n");
        if (ret != HST_RES_OK) goto exit;

        // send headers
        ret = _hst_write(me.hbuf.buf, me.hbuf.len);
        goto exit;
    }

    if (me.state == STATE_WR_BODY) {
        // add content-length header and blank line
        ret = buf_printf(&me.hbuf, "Content-Length: %d\r\n\r\n", me.bbuf.len);
        if (ret != HST_RES_OK) goto exit;

        // send headers
        ret = _hst_write(me.hbuf.buf, me.hbuf.len);
        if (ret != HST_RES_OK) goto exit;

        // send body
        ret = _hst_write(me.bbuf.buf, me.bbuf.len);
        goto exit;
    }

    if (me.state == STATE_WR_BODY_CHUNKED) {
        ret = _hst_write_chunk(me.bbuf.buf, me.bbuf.len);
        goto exit;
    }

    ERROR("Wrong state %d.", me.state);

exit:
    if (me.sc != -1) {
        shutdown(me.sc, SHUT_RDWR);
        close(me.sc);
        me.sc = -1;
    }
    me.state = STATE_READ;
    return ret;
}
