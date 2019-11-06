#include <sys/types.h>
#include <sys/stat.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>


#define ERROR(fmt, ...) fprintf(stderr, \
        "ERROR: %s():%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)


#define MAX_FILE_COUNT      128


typedef enum _ftype_t {
    FTYPE_UNKNOWN,
    FTYPE_INVALID,
    FTYPE_HTML,
    FTYPE_CSS,
    FTYPE_JS
} ftype_t;


static struct _me {
    char dir[NAME_MAX];
    char inc_file_name[NAME_MAX];
    bool render;
    char padding1[1];

    struct timespec max_time;

    FILE *inc_file;

    char *html_files[MAX_FILE_COUNT];
    char *css_files[MAX_FILE_COUNT];
    char *js_files[MAX_FILE_COUNT];

    size_t html_files_count;
    size_t css_files_count;
    size_t js_files_count;
} me;


// forward declarations
void print_usage(void);
void update_max_time(const char *fname);
ftype_t get_file_type(const char *fname);
char *file_name_alloc(const char *fname);
void file_name_free(char *fname);
void render_file(const char *prefix, const char *fname);
void render_line(const char *line, FILE *file);


int main(int argc, char *argv[]) {
    int ret = 1;
    char fname[512];
    char *p;
    size_t i;

    // check parameters
    if (argc != 2) {
        print_usage();
        goto exit;
    } else {
        strncpy(me.dir, argv[1], NAME_MAX);
        size_t dlen = strlen(me.dir);
        if (me.dir[dlen-1] == '/')
            me.dir[dlen-1] = 0;
        struct stat s;
        stat(me.dir, &s);
        if (!S_ISDIR(s.st_mode)) {
            ERROR("Parameter must be a directory name: %s\r\n\r\n", me.dir);
            goto exit;
        }
    }

    // check target files
    snprintf(me.inc_file_name, NAME_MAX, "%s/", me.dir);
    strcat(me.inc_file_name, "webfiles.h");
    update_max_time(me.inc_file_name);
    me.render = false;

    // traverse directory and store valid file names
    bool invalid_fname = false;
    DIR *d = opendir(me.dir);
    if (!d) {
        ERROR("Can`t open current directory.");
        goto exit;
    }
    for (;;) {
        struct dirent *de = readdir(d);
        if (de == NULL) break;

        // path and file name concatenation
        sprintf(fname, "%s/", me.dir);
        strcat(fname, de->d_name);

        switch (get_file_type(fname)) {
        case FTYPE_HTML:
            if (me.html_files_count >= MAX_FILE_COUNT) goto efilecount;
            p = file_name_alloc(fname);
            if (p == NULL) goto exit;
            me.html_files[me.html_files_count++] = p;
            break;
        case FTYPE_CSS:
            if (me.css_files_count >= MAX_FILE_COUNT) goto efilecount;
            p = file_name_alloc(fname);
            if (p == NULL) goto exit;
            me.css_files[me.css_files_count++] = p;
            break;
        case FTYPE_JS:
            if (me.js_files_count >= MAX_FILE_COUNT) goto efilecount;
            p = file_name_alloc(fname);
            if (p == NULL) goto exit;
            me.js_files[me.js_files_count++] = p;
            break;
        case FTYPE_INVALID:
            invalid_fname = true;
            break;
        case FTYPE_UNKNOWN:
            break;
        }
    }
    closedir(d);
    if (invalid_fname)
        goto exit;

    // render files
    if (!me.render) {
        printf("Nothing to do.\n");
        ret = 0;
        goto exit;
    }

    // open output file
    me.inc_file = fopen(me.inc_file_name, "w");
    if (me.inc_file == NULL) {
        ERROR("fopen() error: %s\n", strerror(errno));
        goto exit;
    }

    // sort file names
    qsort(me.html_files, me.html_files_count,
		  sizeof(const char*), (__compar_fn_t)strcmp);
    qsort(me.css_files, me.css_files_count,
		  sizeof(const char*), (__compar_fn_t)strcmp);
    qsort(me.js_files, me.js_files_count,
		  sizeof(const char*), (__compar_fn_t)strcmp);

    // render files
	if (me.html_files_count) {
		for (i=0; i<me.html_files_count; i++)
			render_file("html", me.html_files[i]);
		printf("rendered %d html files\n", (int)me.html_files_count);
	}
	if (me.css_files_count) {
		for (i=0; i<me.css_files_count; i++)
			render_file("css", me.css_files[i]);
		printf("rendered %d css files\n", (int)me.css_files_count);
	}
	if (me.js_files_count) {
		for (i=0; i<me.js_files_count; i++)
			render_file("js", me.js_files[i]);
		printf("rendered %d js files\n", (int)me.js_files_count);
	}

    ret = 0;

exit:
    for (i=0; i<me.html_files_count; i++)
        file_name_free(me.html_files[i]);
    for (i=0; i<me.css_files_count; i++)
        file_name_free(me.css_files[i]);
    for (i=0; i<me.js_files_count; i++)
        file_name_free(me.js_files[i]);
    if (me.inc_file)
        fclose(me.inc_file);
    return ret;

efilecount:
    ERROR("Too much files.");
    goto exit;
}


void print_usage(void) {
    printf("\
    Hsttool is a companion utility for hst  library.  Is  searches  for\r\n\
*.htm *.css and *.js files in specified directory  and  generates  from\r\n\
them webfiles.h C source file to be used in project.\r\n\
    If target file is newer than any source file then it does nothing.\r\n\
\r\n\
Usage:\r\n\
    hsttool DIRECTORY\r\n\
\r\n");
}


void update_max_time(const char *fname) {
    struct stat s;
    int res = stat(fname, &s);
	if (res < 0) {
		if (errno != ENOENT) {
			ERROR("stat('%s') error: %d %s", fname, errno, strerror(errno));
		}
        return;
	}

	if (s.st_mtim.tv_sec < me.max_time.tv_sec) {
        return;
	}
	if (s.st_mtim.tv_sec > me.max_time.tv_sec) {
        me.render = true;
	} else if (s.st_mtim.tv_nsec > me.max_time.tv_nsec)
        me.render = true;

	if (me.render) {
		me.max_time.tv_sec = s.st_mtim.tv_sec;
		me.max_time.tv_nsec = s.st_mtim.tv_nsec;
	}
}


ftype_t get_file_type(const char *fname) {
    ftype_t ret = FTYPE_UNKNOWN;
    const char *ext = NULL;
    int i=0, ifn=0;

    // find last directory separator
    for (i=0; fname[i]; i++)
        if (fname[i] == '/')
            ifn = i + 1;

    // find file extension
    for (i=ifn; fname[i]; i++) {
        if (fname[i] == '.') {
            ext = fname + i + 1;
            break;
        }
    }
    if (ext == NULL)
        goto exit;

    // check for known extension types
    if (0 == strcmp(ext, "html")) {
        ret = FTYPE_HTML;
    } else if (0 == strcmp(ext, "htm")) {
        ret = FTYPE_HTML;
    } else if (0 == strcmp(ext, "css")) {
        ret = FTYPE_CSS;
    } else if (0 == strcmp(ext, "js")) {
        ret = FTYPE_JS;
    }

    // check file name
    if (ret != FTYPE_UNKNOWN) {
        bool valid = false;
        if (isalpha(fname[ifn])) {
            valid = true;
            for (i=ifn+1; fname[i] && fname[i]!='.'; i++) {
                if (!isalnum(fname[i]) && fname[i]!='_') {
                    valid = false;
                    break;
                }
            }
        }
        if (!valid) {
            ERROR("File name (before extension) must be a valid C identifier.");
            ret = FTYPE_INVALID;
        }
    }

    // get file modification time
    if (ret == FTYPE_HTML || ret == FTYPE_CSS || ret == FTYPE_JS)
        update_max_time(fname);

exit:
    return ret;
}


char *file_name_alloc(const char *fname) {
    size_t len = strlen(fname);
    char *ret = malloc(len+1);
    if (ret == NULL) {
        ERROR("Not enough memory.");
        goto exit;
    }
    strcpy(ret, fname);

exit:
    return ret;
}


void file_name_free(char *fname) {
    free(fname);
}


void render_file(const char *prefix, const char *fname) {
    size_t i=0, ifn=0, j=0;
    char cname[256];
    char *bufptr = NULL;
    size_t bufsize = 0;

    // find last directory separator
    for (i=0; fname[i]; i++)
        if (fname[i] == '/')
            ifn = i + 1;

    // compose C variable name
    for (i=0; i<sizeof(cname)-1 && prefix[i]; i++)
        cname[i] = prefix[i];
    cname[i++] = '_';
    for (j=ifn; i<sizeof(cname)-1 && fname[j] && fname[j]!='.'; i++,j++)
        cname[i] = fname[j];
    cname[i] = 0;

    FILE *file = fopen(fname, "r");
    if (file == NULL) {
        ERROR("Can`t open file: %s\n", fname);
        goto exit;
    }

    fprintf(me.inc_file, "\r\n\r\nconst char *%s = \"\\\r\n", cname);

    errno = 0;
    for (;;) {
        ssize_t llen = getline(&bufptr, &bufsize, file);
        if (llen < 0) {
            if (errno) {
                ERROR("Error in getline(): %s", strerror(errno));
                goto exit;
            }
            break;
        }
        render_line(bufptr, me.inc_file);
    }

    fprintf(me.inc_file, "\";\r\n");

exit:
    free(bufptr);
    fclose(file);
    return;
}


void render_line(const char *line, FILE *file) {
    size_t i, j, k;

    // special characters that needs escaping
    char ec[] = {'\t', '\r', '\n', '\"'};

    // special characters as letters
    char ec1[] = {'t', 'r', 'n', '\"'};

    // current special character for output
    char ec2[2] = {'\\', '\0'};

    // main loop
    for (i=0,j=0; ; j++) {
        // check for line end
        if (line[j] == 0) {
            fwrite(line+i, 1, j-i, file);   // write part of string
            fwrite("\\\r\n", 1, 3, file);   // write string ending
            break;
        }

        // check for special character
        bool special = false;
        for (k=0; k<sizeof(ec); k++) {
            if (line[j] == ec[k]) {
                ec2[1] = ec1[k];
                special = true;
                break;
            }
        }

        // if special character is found
        if (special) {
            fwrite(line+i, 1, j-i, file);   // write part of string
            fwrite(ec2, 1, 2, file);        // write special character
            i = j + 1;
            continue;
        }
    }

    return;
}
