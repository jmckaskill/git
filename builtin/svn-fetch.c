#include "git-compat-util.h"
#include "parse-options.h"
#include "gettext.h"
#include "cache.h"
#include "cache-tree.h"
#include "refs.h"
#include "unpack-trees.h"
#include "commit.h"
#include "tag.h"
#include "revision.h"
#include "diff.h"
#include "diffcore.h"
#include "alloc.h"
#include "other.h"

#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

static const char *svnuser;
static const char *svnpass;
static char *trunk;
static char *branches;
static char *tags;
static const char *revisions;
static int verbose;
static int use_stdin;
static FILE* tmpf;
static int infd = -1;
static int outfd = -1;
static int interval = -1;
static const char* url;

static const char* const builtin_svn_fetch_usage[] = {
	"git svn-fetch [options] <repository>",
	NULL,
};

static struct option builtin_svn_fetch_options[] = {
	OPT_STRING(0, "user", &svnuser, "user", "svn username"),
	OPT_STRING(0, "pass", &svnpass, "pass", "svn password"),
	OPT_BOOLEAN('v', "verbose", &verbose, "verbose logging of all svn traffic"),
	OPT_STRING('r', "revision", &revisions, "N:M", "revisions to fetch in the form N or N:M"),
	OPT_BOOLEAN(0, "inetd", &use_stdin, "inetd mode using stdin/out"),
	OPT_STRING('t', "trunk", &trunk, "path", "path of trunk branch"),
	OPT_STRING('b', "branches", &branches, "path", "path of branches"),
	OPT_STRING('T', "tags", &tags, "path", "path of tags"),
	OPT_INTEGER(0, "interval", &interval, "poll interval in seconds (0 to only wake on a signal)"),
	OPT_END()
};

static const char* const builtin_svn_push_usage[] = {
	"git svn-push [options] repo ref <commit>..<commit>",
	"git svn-push [options] --pre-receive repo",
	NULL,
};

static struct option builtin_svn_push_options[] = {
	OPT_STRING(0, "user", &svnuser, "user", "svn username"),
	OPT_STRING(0, "pass", &svnpass, "pass", "svn password"),
	OPT_BOOLEAN('v', "verbose", &verbose, "verbose logging of all svn traffic"),
	OPT_STRING('t', "trunk", &trunk, "path", "path of trunk branch"),
	OPT_STRING('b', "branches", &branches, "path", "path of branches"),
	OPT_STRING('T', "tags", &tags, "path", "path of tags"),
	OPT_STRING(0, "pre-receive", &url, "url", "run as a pre-receive hook"),
	OPT_END()
};

#ifndef min
#define min(a,b) ((a) < (b) ? (a) : (b))
#endif

static char inbuf[4096];
static int inb, ine;

static int readc() {
	if (ine == inb) {
		inb = 0;
		ine = xread(infd, inbuf, sizeof(inbuf));
		if (ine <= 0) return EOF;
	}

	return inbuf[inb++];
}

static void unreadc() {
	inb--;
}

static int readsvn(void* u, void* p, int n) {
	/* big reads we may as well read directly into the target */
	if (ine == inb && n >= sizeof(inbuf) / 2) {
		return xread(infd, p, n);

	} else if (ine == inb) {
		inb = 0;
		ine = xread(infd, inbuf, sizeof(inbuf));
		if (ine <= 0) return ine;
	}

	n = min(n, ine - inb);
	memcpy(p, &inbuf[inb], n);
	inb += n;
	return n;
}

static int readf(void* u, void* p, int n) {
	return fread(p, 1, n, (FILE*) u);
}

static int writef(void* u, const void* p, int n) {
	return fwrite(p, 1, n, (FILE*) u);
}

typedef int (*reader)(void*, void*, int);
typedef int (*writer)(void*, const void*, int);

struct buf {
	char* p;
	int n;
};

static int readb(void* u, void* p, int n) {
	struct buf* b = u;
	int r = min(b->n, n);
	memcpy(p, b->p, r);
	b->n -= r;
	b->p += r;
	return r;
}

static int writeb(void* u, const void* p, int n) {
	struct buf* b = u;
	int r = min(b->n, n);
	memcpy(b->p, p, r);
	b->n -= r;
	b->p += r;
	return r;
}

static const char hex[] = "0123456789abcdef";

static int print_ascii(writer wf, void* wd, const void* p, int n) {
	int i;
	const unsigned char* v = p;

	for (i = 0; i < n; i++) {
		int ch = v[i];

		if (' ' <= ch && ch < 0x7F && ch != '\\') {
			if (wf(wd, &v[i], 1) != 1) {
				return -1;
			}
		} else if (ch == '\n') {
			if (wf(wd, "\\n", 2) != 2) {
				return -1;
			}
		} else if (ch == '\r') {
			if (wf(wd, "\\r", 2) != 2) {
				return -1;
			}
		} else if (ch == '\t') {
			if (wf(wd, "\\t", 2) != 2) {
				return -1;
			}
		} else if (ch == '\\') {
			if (wf(wd, "\\\\", 2) != 2) {
				return -1;
			}
		} else {
			char b[4];
			b[0] = '\\';
			b[1] = 'x';
			b[2] = hex[ch >> 4];
			b[3] = hex[ch & 0x0F];
			if (wf(wd, b, 4) != 4) {
				return -1;
			}
		}
	}

	return n;
}

static void print_hex(writer wf, void* wd, const void* p, int n) {
	int i;
	const unsigned char* v = p;
	for (i = 0; i < n; i++) {
		char b[2];
		b[0] = hex[v[i] >> 4];
		b[1] = hex[v[i] & 0x0F];
		wf(wd, b, 2);
	}
}

static void copyn(writer wf, void* wd, reader rf, void *rd, int64_t n) {
	while (n > 0) {
		char buf[BUFSIZ];
		int r = rf(rd, buf, min(sizeof(buf), n));
		if (r <= 0)
			die(_("unexpected end %s"), strerror(errno));

		if (wf && wf(wd, buf, r) != r)
			die(_("failed write %s"), strerror(errno));

		n -= r;
	}
}

static void readfull(void *p, reader rf, void* rd, int n) {
	while (n > 0) {
		int r = rf(rd, p, n);
		if (r <= 0)
			die(_("unexpected end %s"), strerror(errno));

		p = (char*) p + r;
		n -= r;
	}
}

static int64_t read_varint(reader rf, void* rd) {
	int64_t v = 0;
	unsigned char ch = 0x80;

	while (ch & 0x80) {
		if (v > (INT64_MAX >> 7) || rf(rd, &ch, 1) != 1)
			die(_("invalid svndiff"));

		v = (v << 7) | (ch & 0x7F);
	}

	return v;
}

#define MAX_VARINT_LEN 9

static unsigned char* encode_varint(unsigned char* p, int64_t n) {
	if (n < 0) die("int too large");
	if (n >= (INT64_C(1) << 56)) *(p++) = ((n >> 56) & 0x7F) | 0x80;
	if (n >= (INT64_C(1) << 49)) *(p++) = ((n >> 49) & 0x7F) | 0x80;
	if (n >= (INT64_C(1) << 42)) *(p++) = ((n >> 42) & 0x7F) | 0x80;
	if (n >= (INT64_C(1) << 35)) *(p++) = ((n >> 35) & 0x7F) | 0x80;
	if (n >= (INT64_C(1) << 28)) *(p++) = ((n >> 28) & 0x7F) | 0x80;
	if (n >= (INT64_C(1) << 21)) *(p++) = ((n >> 21) & 0x7F) | 0x80;
	if (n >= (INT64_C(1) << 14)) *(p++) = ((n >> 14) & 0x7F) | 0x80;
	if (n >= (INT64_C(1) << 7)) *(p++) = ((n >> 7) & 0x7F) | 0x80;
	*(p++) = n & 0x7F;
	return p;
}

__attribute__((format (printf,1,2)))
static void sendf(const char* fmt, ...);

static void sendf(const char* fmt, ...) {
	static struct strbuf out = STRBUF_INIT;
	char* nl;
	va_list ap;
	va_start(ap, fmt);
	strbuf_reset(&out);
	strbuf_vaddf(&out, fmt, ap);

	nl = out.buf;
	while (verbose && nl < out.buf + out.len) {
		char* p = nl;
		nl = memmem(p, out.buf + out.len - p, " )\n( ", 5);
		if (nl) {
			/* not last of multi-line */
			nl += 2;
		} else if (out.buf[out.len-1] == '\n') {
			/* last of multi-line */
			nl = out.buf + out.len - 1;
		} else {
			/* last of incomplete line */
			nl = out.buf + out.len;
		}

		if (nl - p >= 2 && !memcmp(p, "( ", 2)) {
			fputc('+', stderr);
		}

		print_ascii(&writef, stderr, p, nl - p);

		if (nl[0] == '\n') {
			fputc('\n', stderr);
			nl++;
		}
	}

	if (write_in_full(outfd, out.buf, out.len) != out.len) {
		die_errno("write");
	}
}

static void sendb(const void* data, size_t sz) {
	if (verbose) {
		print_ascii(&writef, stderr, data, sz);
	}
	if (write_in_full(outfd, data, sz) != sz) {
		die_errno("write");
	}
}

struct unzip {
	git_zstream z;
	reader rf;
	void* rd;
	int rn;
	unsigned char buf[4096];
};

static int readz(void* u, void* p, int n) {
	struct unzip* z = u;
	int r;

	z->z.avail_out = n;
	z->z.next_out = p;

	if (z->z.avail_in) {
		r = git_inflate(&z->z, 0);
		if (r) return -1;
		if (z->z.avail_out < n) return n - z->z.avail_out;
	}

	if (z->rn) {
		memmove(z->buf, z->z.next_in, z->z.avail_in);
		z->z.next_in = z->buf;
		r = z->rf(z->rd, z->buf, min(z->rn, sizeof(z->buf) - z->z.avail_in));
		if (r <= 0) return -1;
		z->rn -= r;
		z->z.avail_in += r;
	}

	r = git_inflate(&z->z, z->rn ? 0 : Z_FINISH);
	if (r) return -1;

	return n - z->z.avail_out;
}

#define COPY_FROM_SOURCE (0 << 6)
#define COPY_FROM_TARGET (1 << 6)
#define COPY_FROM_NEW    (2 << 6)

static int read_instruction(reader rf, void* rd, int64_t* off, int64_t* len) {
	unsigned char hdr;
	int ret;

	if (rf(rd, &hdr, 1) != 1)
		die(_("invalid svndiff"));

	*len = hdr & 0x3F;
	if (*len == 0) {
		*len = read_varint(rf, rd);
	}

	ret = hdr & 0xC0;
	if (ret == COPY_FROM_SOURCE || ret == COPY_FROM_TARGET) {
		*off = read_varint(rf, rd);
	} else {
		*off = 0;
	}

	return ret;
}

#define MAX_INS_LEN (1 + 2 * MAX_VARINT_LEN)

static unsigned char* encode_instruction(unsigned char* p, int ins, int64_t off, int64_t len) {
	if (len < 0x3F) {
		*(p++) = ins | len;
	} else {
		*(p++) = ins;
		p = encode_varint(p, len);
	}

	if (ins == COPY_FROM_SOURCE || ins == COPY_FROM_TARGET) {
		p = encode_varint(p, off);
	}

	return p;
}

static void apply_svndiff_win(FILE* tgt, const void* srcp, size_t srcn, reader df, void* dd, int ver) {
	struct unzip z;
	char insv[4096];
	char tgtv[4096];
	struct buf ins, b;
	int64_t srco = read_varint(df, dd);
	int64_t srcl = read_varint(df, dd);
	int64_t tgtl = read_varint(df, dd);
	int64_t insc = read_varint(df, dd);
	int64_t datac = read_varint(df, dd);
	int64_t insl = ver > 0 ? read_varint(df, dd) : insc;
	int64_t datal;
	int64_t w = 0;

	if (srco + srcl > srcn) goto err;
	if (insl > sizeof(insv)) goto err;

	if (insc < insl) {
		git_inflate_init(&z.z);
		z.rf = df;
		z.rd = dd;
		z.rn = insc;
		readfull(insv, &readz, &z, (int) insl);
		git_inflate_end(&z.z);
	} else {
		readfull(insv, df, dd, (int) insl);
	}

	ins.p = insv;
	ins.n = insl;

	datal = ver > 0 ? read_varint(df, dd) : datac;

	if (datac < datal) {
		git_inflate_init(&z.z);
		z.rf = df;
		z.rd = dd;
		z.rn = datac;
		df = &readz;
		dd = &z;
	}

	while (ins.n) {
		int64_t off, len;
		int tgtr;
		switch (read_instruction(&readb, &ins, &off, &len)) {
		case COPY_FROM_SOURCE:
			if (off + len > srcl) goto err;
			b.p = (char*) srcp + srco + off;
			b.n = len;
			copyn(&writef, tgt, &readb, &b, len);
			w += len;
			break;

		case COPY_FROM_TARGET:
			tgtr = min(w - off, len);
			if (tgtr <= 0 || tgtr > sizeof(tgtv)) goto err;
			fseek(tgt, -w + off, SEEK_END);
			readfull(tgtv, &readf, tgt, tgtr);
			len -= tgtr;

			/* The target len may roll off the end, in that
			 * case we just copy out what is in the buffer.
			 * This is used for repeats.
			 */
			while (len) {
				int n = min(len, tgtr);
				if (fwrite(tgtv, 1, n, tgt) != n)
					goto err;
				len -= n;
				w += n;
			}
			break;

		case COPY_FROM_NEW:
			if (len > datal) goto err;
			copyn(&writef, tgt, df, dd, len);
			w += len;
			datal -= len;
			break;

		default:
			goto err;
		}
	}

	if (dd == &z) {
		git_inflate_end(&z.z);
	}

	if (w != tgtl || datal) goto err;

	return;
err:
	die(_("invalid svndiff"));
}

static void apply_svndiff(FILE* tgt, const void* srcp, size_t srcn, reader df, void* dd, int (*eof)(void*)) {
	unsigned char hdr[4];
	readfull(hdr, df, dd, 4);
	if (memcmp(hdr, "SVN", 3))
		goto err;

	if (hdr[3] > 1)
		goto err;

	while (!eof(dd)) {
		apply_svndiff_win(tgt, srcp, srcn, df, dd, hdr[3]);
	}

	return;

err:
	die(_("invalid svndiff"));
}

/* returns -1 if it can't find a number */
static int64_t read_number64() {
	int64_t v;

	for (;;) {
		int ch = readc();
		if ('0' <= ch && ch <= '9') {
			v = ch - '0';
			break;
		} else if (ch != ' ' && ch != '\n') {
			unreadc();
			return -1;
		}
	}

	for (;;) {
		int ch = readc();
		if (ch < '0' || ch > '9') {
			unreadc();
			if (verbose) fprintf(stderr, " %d", (int) v);
			return v;
		}

		if (v > INT64_MAX/10) {
			die(_("number too big"));
		} else {
			v = 10*v + (ch - '0');
		}
	}
}

static int read_number() {
	int64_t v = read_number64();
	if (v > INT_MAX)
		die(_("number too big"));
	return (int) v;
}

/* returns -1 if it can't find a list */
static int read_list() {
	for (;;) {
		int ch = readc();
		if (ch == '(') {
			if (verbose) fprintf(stderr, " (");
			return 0;
		} else if (ch != ' ' && ch != '\n') {
			unreadc();
			return -1;
		}
	}
}

/* returns 0 if the list is missing or empty (and skips over it), 1 if
 * its present and has values */
static int have_optional() {
	if (read_list()) return 0;
	for (;;) {
		int ch = readc();
		if (ch == ')') {
			if (verbose) fprintf(stderr, " )");
			return 0;
		} else if (ch != ' ' && ch != '\n') {
			unreadc();
			return 1;
		}
	}
}

/* returns NULL if it can't find an atom, string only valid until next
 * call to read_word, not thread-safe */
static const char *read_word() {
	static char buf[256];
	int bufsz = 0;
	int ch;

	for (;;) {
		ch = readc();
		if (('a' <= ch && ch <= 'z') || ('A' <= ch && ch <= 'Z')) {
			break;
		} else if (ch != ' ' && ch != '\n') {
			unreadc();
			return NULL;
		}
	}

	while (('a' <= ch && ch <= 'z') || ('A' <= ch && ch <= 'Z')
			|| ('0' <= ch && ch <= '9')
			|| ch == '-') {
		if (bufsz >= sizeof(buf))
			die(_("atom too long"));

		buf[bufsz++] = ch;
		ch = readc();
	}

	unreadc();
	buf[bufsz] = '\0';
	if (verbose) fprintf(stderr, " %s", buf);
	return bufsz ? buf : NULL;
}


/* reads the string header, returning number of bytes to read from svn
 * afterwards or -1 if no string can be found */
static int64_t read_string_size() {
	int64_t sz = read_number64();
	if (sz < 0)
		return sz;
	if (readc() != ':')
		die(_("malformed string"));
	if (verbose) fprintf(stderr, ":");
	return sz;
}

static int read_strbuf(struct strbuf* s) {
	int64_t n = read_string_size();
	if (n < 0 || n > INT_MAX) return -1;

	strbuf_grow(s, n);
	readfull(s->buf + s->len, &readsvn, NULL, n);
	strbuf_setlen(s, s->len + n);

	if (verbose) {
		print_ascii(&writef, stderr, s->buf + s->len - n, n);
	}
	return 0;
}

static int read_string(writer wf, void* wd) {
	int64_t n = read_string_size();
	if (n < 0) return -1;
	copyn(wf, wd, &readsvn, NULL, n);
	return n;
}

static void read_end() {
	int parens = 1;
	while (parens > 0) {
		int ch = readc();
		if (ch == EOF)
			die(_("socket close whilst looking for list close"));

		if (ch == '(') {
			if (verbose) fprintf(stderr, " (");
			parens++;
		} else if (ch == ')') {
			if (verbose) fprintf(stderr, " )");
			parens--;
		} else if (ch == ' ' || ch == '\n') {
			/* whitespace */
		} else if ('0' <= ch && ch <= '9') {
			/* number or string */
			int64_t n;
			char buf[4096];
			unreadc();
			n = read_number64();

			ch = readc();
			if (ch != ':') {
				/* number */
				unreadc();
				continue;
			}

			/* string */
			if (verbose) fputc(':', stderr);
			while (n) {
				int r = readsvn(NULL, buf, min(n, sizeof(buf)));
				if (r <= 0) die_errno("read");
				if (verbose) print_ascii(&writef, stderr, buf, r);
				n -= r;
			}
		} else {
			unreadc();
			if (!read_word())
				die(_("unexpected character %c"), ch);
		}
	}
}

static const char* read_response() {
	const char *cmd;

	if (read_list()) goto err;

	cmd = read_word();
	if (!cmd) goto err;
	if (read_list()) goto err;

	return cmd;
err:
	die(_("malformed response"));
}

static void read_end_response() {
	read_end();
	read_end();
	if (verbose) fprintf(stderr, "\n");
}

static void read_done() {
	const char* s = read_word();
	if (!s || strcmp(s, "done"))
		die("unexpected failure");
	if (verbose) fputc('\n', stderr);
}

static void read_success() {
	const char* s = read_response();
	if (strcmp(s, "success")) {
		verbose = 1;
		read_end();
		die("unexpected failure");
	}
	read_end_response();
}

static void cram_md5(const char* user, const char* pass) {
	const char *s;
	char chlg[256];
	unsigned char hash[16];
	char hb[32];
	int64_t sz;
	HMAC_CTX hmac;
	struct buf b;

	s = read_response();
	if (strcmp(s, "step")) goto error;

	sz = read_string_size();
	if (sz < 0 || sz >= sizeof(chlg)) goto error;
	readfull(chlg, &readsvn, NULL, (int) sz);

	read_end_response();

	HMAC_Init(&hmac, (unsigned char*) pass, strlen(pass), EVP_md5());
	HMAC_Update(&hmac, (unsigned char*) chlg, sz);
	HMAC_Final(&hmac, hash, NULL);
	HMAC_CTX_cleanup(&hmac);

	b.p = hb;
	b.n = sizeof(hb);
	print_hex(&writeb, &b, hash, sizeof(hash));
	sendf("%d:%s %.*s\n", (int) strlen(user) + 1 + 32, user, 32, hb);

	return;

error:
	die(_("auth failed"));
}

static int64_t deltamore() {
	for (;;) {
		const char* s;
		int64_t n;

		/* finish off the previous textdelta-chunk or
		 * apply-textdelta */
		read_end_response();

		s = read_response();

		if (!strcmp(s, "textdelta-end")) {
			return 0;
		}

		/* if we get some other command we just loop around
		 * again */
		if (strcmp(s, "textdelta-chunk")) {
			continue;
		}

		/* file-token, chunk */
		n = read_string_size();
		if (n < 0) goto err;
		copyn(NULL, NULL, &readsvn, NULL, n);

		n = read_string_size();
		if (n < 0) goto err;
		if (n > 0) return n;
	}

err:
	die("invalid textdelta command");
}

static int deltaeof(void* u) {
	return (*(int64_t*) u) <= 0;
}

static int deltar(void* u, void* p, int n) {
	int64_t* d = u;
	int r;

	if (*d <= 0) return *d;

	r = readsvn(NULL, p, min(n, *d));
	*d -= r;

	if (verbose) print_ascii(&writef, stderr, p, r);

	if (*d == 0) *d = deltamore();

	return r;
}

static void read_name(struct strbuf* name) {
	strbuf_reset(name);
	if (read_strbuf(name)) goto err;
	if (name->buf[0] == '/') strbuf_remove(name, 0, 1);
	if (memchr(name->buf, '\0', name->len)) goto err;
	if (strstr(name->buf, "//")) goto err;
	if (!strcmp(name->buf, "..")) goto err;
	if (!strcmp(name->buf, ".")) goto err;
	if (!prefixcmp(name->buf, "../")) goto err;
	if (!prefixcmp(name->buf, "./")) goto err;
	if (strstr(name->buf, "/../")) goto err;
	if (strstr(name->buf, "/./")) goto err;
	if (!suffixcmp(name->buf, "/..")) goto err;
	if (!suffixcmp(name->buf, "/.")) goto err;

	return;
err:
	die("invalid path name %s", name->buf);
}

struct svnobj {
	struct other* other;
	unsigned long date;
	unsigned char parent[20];
	unsigned char object[20];
	int rev;
	const char* path;
	unsigned int unpacked : 1;
	unsigned int istag : 1;
	struct index_state* index;
};

DEFINE_ALLOCATOR(svnobj, struct svnobj)

static struct svnobj* parse_svnobj(const unsigned char* sha1) {
	struct other_header hdr;
	struct svnobj* s;
	struct other* o;
	char* p;

	if (is_null_sha1(sha1))
		return NULL;

	o = lookup_other(sha1);
	if (!o) return NULL;

	if (parse_other(o))
		return NULL;

	if (strcmp(o->type, "svn")) {
		error("Object %s is a %s, not a svn object",
			sha1_to_hex(sha1), o->type);
		return NULL;
	}

	if (o->util) return o->util;

	p = o->buffer;
	s = alloc_svnobj_node();
	memset(s, 0, sizeof(*s));
	if (parse_other_header(&p, &hdr)) goto err;

	if (strcmp(hdr.key, "type")) goto err;
	if (parse_other_header(&p, &hdr)) goto err;

	if (strcmp(hdr.key, "date")) goto err;
	s->date = strtoul(hdr.value, NULL, 10);
	if (parse_other_header(&p, &hdr)) goto err;

	if (!strcmp(hdr.key, "+object")) {
		hashcpy(s->object, hdr.sha1);
		s->istag = !strcmp(hdr.value, "tag");
		if (parse_other_header(&p, &hdr)) goto err;
	}

	if (!strcmp(hdr.key, "+parent")) {
		hashcpy(s->parent, hdr.sha1);
		if (parse_other_header(&p, &hdr)) goto err;
	}

	if (strcmp(hdr.key, "revision")) goto err;
	s->rev = strtoul(hdr.value, NULL, 10);
	if (parse_other_header(&p, &hdr)) goto err;

	if (strcmp(hdr.key, "path")) goto err;
	s->path = hdr.value;

	s->other = o;
	o->util = s;

	fprintf(stderr, "svnobj %s date %d obj %s tag %d prev %s rev %d\n",
			sha1_to_hex(sha1),
			(int) s->date,
			sha1_to_hex(s->object),
			s->istag,
			sha1_to_hex(s->parent),
			s->rev);
	return s;

err:
	error("Object %s is not a valid svn object", sha1_to_hex(sha1));
	return NULL;
}

/* if index is NULL, then this uses the index in the object */
static struct index_state* checkout_tree(struct svnobj* s, struct index_state* index) {
	struct tree* tree;
	struct tree_desc desc;
	struct unpack_trees_options op;

	if (index == NULL) {
		if (s->index == NULL) {
			s->index = xcalloc(1, sizeof(*s->index));
		}
		index = s->index;
	}

	tree = parse_tree_indirect(s->object);
	if (!tree) die("unpack tree failed on %s", sha1_to_hex(s->object));

	init_tree_desc(&desc, tree->buffer, tree->size);

	memset(&op, 0, sizeof(op));
	op.head_idx = -1;
	op.src_index = index;
	op.dst_index = index;
	op.index_only = 1;
	op.debug_unpack = verbose;

	if (unpack_trees(1, &desc, &op))
		die("unpack tree failed on %s", sha1_to_hex(s->object));

	return index;
}

struct svnref {
	struct strbuf svn; /* svn root */
	struct strbuf ref; /* svn ref path */
	struct strbuf remote; /* remote ref path */
	struct index_state index;

	unsigned int delete : 1;
	unsigned int dirty : 1;
	unsigned int istag : 1;
	unsigned int create : 1;

	struct svnobj* obj;
	struct svnobj* iter;

	unsigned char commit[20];
	unsigned char value[20];

	struct svnref* copysrc;
	int copyrev;
};

static struct svnref** refs;
static size_t refn, refalloc;

static int is_in_dir(char* file, char* dir, char** rel) {
	size_t sz = strlen(dir);
	if (strncmp(file, dir, sz)) return 0;
	if (file[sz] && file[sz] != '/') return 0;
	if (rel) *rel = file[sz] ? &file[sz+1] : &file[sz];
	return 1;
}

static int unpack_svnref = 0;

#define TRUNK_REF 0
#define BRANCH_REF 1
#define TAG_REF 2

static struct svnref* create_ref(int type, const char* name, int create) {
	struct svnref* r = NULL;

	switch (type) {
	case TRUNK_REF:
		r = xcalloc(1, sizeof(*r));
		strbuf_addstr(&r->svn, trunk ? trunk : "");
		strbuf_addstr(&r->ref, "refs/svn/heads/trunk");
		strbuf_addstr(&r->remote, "refs/remotes/svn/trunk");
		break;

	case BRANCH_REF:
		r = xcalloc(1, sizeof(*r));

		strbuf_addstr(&r->svn, branches);
		strbuf_addch(&r->svn, '/');
		strbuf_addstr(&r->svn, name);

		strbuf_addstr(&r->ref, "refs/svn/heads/");
		strbuf_addstr(&r->ref, name);

		strbuf_addstr(&r->remote, "refs/remotes/svn/");
		strbuf_addstr(&r->remote, name);
		break;

	case TAG_REF:
		r = xcalloc(1, sizeof(*r));
		r->istag = 1;

		strbuf_addstr(&r->svn, tags);
		strbuf_addch(&r->svn, '/');
		strbuf_addstr(&r->svn, name);

		strbuf_addstr(&r->ref, "refs/svn/tags/");
		strbuf_addstr(&r->ref, name);

		strbuf_addstr(&r->remote, "refs/tags/");
		strbuf_addstr(&r->remote, name);
		break;
	}

	read_ref(r->ref.buf, r->value);

	if (create) {
		if (!is_null_sha1(r->value))
			die("new ref '%s' already exists", r->ref.buf);

		r->create = 1;
	} else {
		if (is_null_sha1(r->value))
			die("ref '%s' not found", r->ref.buf);

		r->obj = parse_svnobj(r->value);
		if (!r->obj || r->istag != r->obj->istag)
			die("ref '%s' not a valid svn object", r->ref.buf);

		if (r->istag) {
			struct tag* tag = lookup_tag(r->obj->object);
			if (!tag || lookup_tag(r->obj->object) || tag->tagged->type != OBJ_COMMIT)
				die("ref '%s' does not wrap a valid tag", r->ref.buf);
			hashcpy(r->commit, tag->tagged->sha1);
		} else {
			hashcpy(r->commit, r->obj->object);
		}

		if (unpack_svnref) {
			checkout_tree(r->obj, &r->index);
		}

		/* TODO: what do we need to free? */
	}

	fprintf(stderr, "\ncreated ref %d %s %d %s\n",
		       	(int) refn, r->ref.buf, r->obj ? r->obj->rev : 0, r->obj ? sha1_to_hex(r->obj->other->object.sha1) : sha1_to_hex(null_sha1));

	ALLOC_GROW(refs, refn + 1, refalloc);
	refs[refn++] = r;
	return r;
}

static struct svnref* find_svnref_by_path(struct strbuf* name, int create) {
	int i;
	struct svnref* r;
	char *a, *b, *c, *d;

	if (!trunk && !branches && !tags && refn) {
		return refs[0];
	}

	for (i = 0; i < refn; i++) {
		r = refs[i];
		fprintf(stderr, "\n find %s %s\n", name->buf, r->svn.buf);
		if (prefixcmp(name->buf, r->svn.buf)) {
			continue;
		}

		switch (name->buf[r->svn.len]) {
		case '\0':
			strbuf_setlen(name, 0);

			if (create && !is_null_sha1(r->value)) {
				die("new ref '%s' already exists", r->ref.buf);
			} else if (!create && is_null_sha1(r->value)) {
				die("ref '%s' not found", r->ref.buf);
			}

			return r;
		case '/':
			strbuf_remove(name, 0, r->svn.len + 1);
			return r;
		}
	}

	/* names are of the form
	 * branches/foo/...
	 * a        b  c   d
	 */
	a = name->buf;
	d = name->buf + name->len;

	if (!trunk && !branches && !tags) {
		return create_ref(TRUNK_REF, "", name->len == 0);

	} else if (trunk && is_in_dir(a, trunk, &b)) {
		strbuf_remove(name, 0, b - a);
		return create_ref(TRUNK_REF, "", create && name->len == 0);


	} else if (branches && is_in_dir(a, branches, &b) && *b) {
		c = memchr(b, '/', d - b);
		if (!c) c = d;
		*c = '\0';
		r = create_ref(BRANCH_REF, b, create && name->len == c - a);
		strbuf_remove(name, 0, c - a);
		return r;

	} else if (tags && is_in_dir(a, tags, &b) && *b) {
		c = memchr(b, '/', d - b);
		if (!c) c = d;
		*c = '\0';
		r = create_ref(TAG_REF, b, create && name->len == c - a);
		strbuf_remove(name, 0, c - a);
		return r;

	} else {
		return NULL;
	}
}

static struct svnref* find_svnref_by_refname(const char* name, int create) {
	int i;
	char* real_ref = NULL;
	unsigned char sha1[20];
	int refcount = dwim_ref(name, strlen(name), sha1, &real_ref);

	if (refcount > 1) {
		die("ambiguous ref '%s'", name);
	} else if (!refcount) {
		die("can not find ref '%s'", name);
	}

	for (i = 0; i < refn; i++) {
		struct svnref* r = refs[i];
		if (strcmp(r->ref.buf, real_ref)) continue;

		if (create && !is_null_sha1(r->value)) {
			die("new ref '%s' already exists", r->ref.buf);
		} else if (!create && is_null_sha1(r->value)) {
			die("ref '%s' not found", r->ref.buf);
		}

		return r;
	}

	if (!strcmp(real_ref, "refs/heads/trunk")) {
		return create_ref(TRUNK_REF, "", create);

	} else if (!prefixcmp(real_ref, "refs/heads/")) {
		if (!branches)
			die("in order to push a branch, --branches must be specified");

		return create_ref(BRANCH_REF, real_ref + strlen("refs/heads/"), create);

	} else if (!prefixcmp(real_ref, "refs/tags/")) {
		if (!tags)
			die("in order to push a tag, --tags must be specified");

		return create_ref(TAG_REF, real_ref + strlen("refs/tags/"), create);

	} else {
		die("ref '%s' not a local branch/tag", real_ref);
		return NULL;
	}
}

static struct svnobj* find_svnobj(struct svnref* r, int rev) {
	struct svnobj* obj = r->obj;
	while (obj && obj->rev > rev) {
		obj = parse_svnobj(obj->parent);
	}

	return obj;
}

/* reads a path, revision pair */
static struct svnref* read_copy_source(struct strbuf* name, int* rev) {
	int64_t srev;
	struct svnref* sref;

	/* copy-path */
	read_name(name);
	sref = find_svnref_by_path(name, 0);
	if (!sref) goto err;

	/* copy-rev */
	srev = read_number();
	if (srev < 0 || srev > INT_MAX) goto err;
	*rev = srev;

	return sref;
err:
	die("invalid copy source");
}

static int create_ref_cb(const char* refname, const unsigned char* sha1, int flags, void* cb_data) {
	int i;
	for (i = 0; i < refn; i++) {
		if (!strcmp(refs[i]->ref.buf, refname)) {
			return 0;
		}
	}

	if (!strcmp(refname, "refs/svn/heads/trunk")) {
		create_ref(TRUNK_REF, "", 0);
	} else if (!prefixcmp(refname, "refs/svn/heads/")) {
		create_ref(BRANCH_REF, refname + strlen("refs/svn/heads/"), 0);
	} else if (!prefixcmp(refname, "refs/svn/tags/")) {
		create_ref(TAG_REF, refname + strlen("refs/svn/tags/"), 0);
	}

	return 0;
}

static void add_all_refs() {
	for_each_ref_in("refs/svn/", &create_ref_cb, NULL);
}

static struct svnobj* get_next_rev(int rev) {
	int i;

	while (--rev) {
		for (i = 0; i < refn; i++) {
			struct svnref* r = refs[i];

			while (r->iter && r->iter->rev > rev) {
				r->iter = parse_svnobj(r->iter->parent);
			}

			if (r->iter && r->iter->rev == rev) {
				return r->iter;
			}
		}
	}

	return NULL;
}

/* Finds the merge base of the commit with all the svn heads. This is
 * done by searching back through the svn objects from the most recent
 * revno back. */
static struct svnobj* find_copy_source(struct commit* head, int rev) {
	int i;
	struct svnobj *obj, *res;
	struct commit* cmt;

	add_all_refs();

	for (i = 0; i < refn; i++) {
		refs[i]->iter = refs[i]->obj;
	}

	obj = get_next_rev(rev+1);
	cmt = head;

	while (cmt || obj) {
		if (cmt && (!obj || cmt->date > obj->date)) {
			if (cmt->object.flags & SEEN) {
				res = cmt->util;
				break;
			}

			cmt->object.flags |= SEEN;

			if (parse_commit(cmt))
				die("invalid commit %s", sha1_to_hex(cmt->object.sha1));

			if (cmt->parents) {
				cmt = cmt->parents->item;
			} else {
				cmt = NULL;
			}
		} else {
			struct commit* ocmt;

			if (obj->istag) {
				struct tag* tag = lookup_tag(obj->object);
				if (!tag || parse_tag(tag) || !tag->tagged || tag->tagged->type != OBJ_COMMIT)
					die("invalid svn object %s", sha1_to_hex(obj->other->object.sha1));
				ocmt = (struct commit*) tag->tagged;
			} else {
				ocmt = lookup_commit(obj->object);
			}

			if (ocmt->object.flags & SEEN) {
				res = obj;
				break;
			}

			ocmt->object.flags |= SEEN;
			ocmt->util = obj;
			obj = get_next_rev(obj->rev);
		}
	}

	/* now need to clear the SEEN flags so the next lookup works */

	cmt = head;
	while (cmt && (cmt->object.flags & SEEN)) {
		cmt->object.flags &= ~SEEN;
		if (!cmt->parents)
			break;

		cmt = cmt->parents->item;
	}

	for (i = 0; i < refn; i++) {
		refs[i]->iter = refs[i]->obj;
	}

	while ((obj = get_next_rev(obj->rev)) != NULL) {
		struct commit* ocmt;

		if (obj->istag) {
			struct tag* tag = lookup_tag(obj->object);
			if (!tag || !tag->tagged || tag->tagged->type != OBJ_COMMIT)
				break;
			ocmt = (struct commit*) tag->tagged;
		} else {
			ocmt = lookup_commit(obj->object);
		}

		if (!(ocmt->object.flags & SEEN))
			break;

		ocmt->object.flags &= ~SEEN;
		ocmt->util = NULL;
	}

	return res;
}

static void read_update(int rev) {
	void* srcp = NULL;
	size_t srcn;
	struct strbuf name = STRBUF_INIT;
	struct strbuf srcname = STRBUF_INIT;
	struct svnref* ref = NULL;
	const char* cmd = NULL;

	read_success(); /* update */
	read_success(); /* report */

	for (;;) {
		/* finish off previous command */
		if (cmd) read_end_response();

		if (read_list()) goto err;

		cmd = read_word();
		if (!cmd || read_list()) goto err;

		if (!strcmp(cmd, "close-edit")) {
			break;

		} else if (!strcmp(cmd, "abort-edit")) {
			die("update aborted");

		} else if (!strcmp(cmd, "open-root")) {
			strbuf_reset(&name);
			find_svnref_by_path(&name, rev == 1);

		} else if (!strcmp(cmd, "add-dir")) {
			struct cache_entry* ce;
			struct svnref* src;
			struct index_state* srcidx;
			struct svnobj* srcobj;
			int srcrev;
			size_t dlen;
			int i;

			/* path, parent-token, child-token, [copy-path, copy-rev] */
			read_name(&name);
			ref = find_svnref_by_path(&name, 1);
			/* ignore this if we already have the revision */
			if (!ref || (ref->obj && ref->obj->rev >= rev)) continue;

			if (read_string(NULL, NULL) < 0) goto err;
			if (read_string(NULL, NULL) < 0) goto err;

			if (!have_optional()) continue;

			src = read_copy_source(&srcname, &srcrev);
			if (srcrev > rev) goto err;

			srcobj = find_svnobj(src, rev);
			if (!srcobj) goto err;

			srcidx = checkout_tree(srcobj, NULL);

			/* copy over all files which are children of the
			 * source path */

			strbuf_addch(&srcname, '/');
			i = index_name_pos(srcidx, srcname.buf, srcname.len);
			if (i >= 0) goto err;
			i = -i-1;

			strbuf_addch(&name, '/');
			dlen = name.len;

			while (i < srcidx->cache_nr) {
				struct cache_entry* se = srcidx->cache[i];
				if (ce_namelen(se) > name.len) break;
				if (memcmp(se->name, srcname.buf, srcname.len)) break;

				/* since the base path may change, we
				 * can't just copy the previous one */
				strbuf_setlen(&name, dlen);
				strbuf_addstr(&name, se->name + srcname.len);
				ce = make_cache_entry(se->ce_mode, se->sha1, name.buf, 0, 0);
				add_index_entry(&ref->index, ce, ADD_CACHE_OK_TO_ADD | ADD_CACHE_OK_TO_REPLACE);
			}

			read_end();

		} else if (!strcmp(cmd, "open-file")) {
			/* name, dir-token, file-token, rev */
			enum object_type type;
			struct cache_entry* ce;

			read_name(&name);
			ref = find_svnref_by_path(&name, 0);
			/* ignore this if we already have the revision */
			if (!ref || (ref->obj && ref->obj->rev >= rev)) continue;

			ce = index_name_exists(&ref->index, name.buf, name.len, 0);
			if (!ce) goto err;

			srcp = read_sha1_file(ce->sha1, &type, &srcn);
			if (!srcp || type != OBJ_BLOB) goto err;

			if (fseek(tmpf, 0, SEEK_SET) || ftruncate(fileno(tmpf), 0))
				die_errno("truncate");

		} else if (!strcmp(cmd, "add-file")) {
			/* name, dir-token, file-token, [copy-path, copy-rev] */
			enum object_type type;
			struct cache_entry* ce;
			struct svnref* src;
			struct svnobj* srcobj;
			int srcrev;
			struct index_state* srcidx;

			read_name(&name);
			ref = find_svnref_by_path(&name, 0);
			/* ignore this if we already have the revision */
			if (!ref || (ref->obj && ref->obj->rev >= rev)) continue;

			if (read_string(NULL, NULL) < 0) goto err;
			if (read_string(NULL, NULL) < 0) goto err;

			if (fseek(tmpf, 0, SEEK_SET) || ftruncate(fileno(tmpf), 0))
				die_errno("truncate");

				srcp = NULL;
				srcn = 0;

			if (!have_optional()) continue;

			src = read_copy_source(&srcname, &srcrev);
			if (srcrev > rev) goto err;

			srcobj = find_svnobj(src, rev);
			if (!srcobj) goto err;

			srcidx = checkout_tree(srcobj, NULL);

			ce = index_name_exists(srcidx, srcname.buf, srcname.len, 0);
			if (!ce) goto err;

			srcp = read_sha1_file(ce->sha1, &type, &srcn);
			if (!srcp || type != OBJ_BLOB) goto err;

			read_end();

		} else if (!strcmp(cmd, "close-file")) {
			/* file-token, [text-checksum] */
			struct cache_entry* ce;
			unsigned char sha1[20];
			struct stat st;

			/* ignore this if we already have the revision */
			if (!ref || (ref->obj && ref->obj->rev >= rev)) continue;

			if (srcp) {
				free(srcp);
				srcp = NULL;
				srcn = 0;
			}

			fflush(tmpf);
			fseek(tmpf, 0, SEEK_SET);

			if (fstat(fileno(tmpf), &st))
				die_errno("stat temp file");
			if (index_fd(sha1, dup(fileno(tmpf)), &st, OBJ_BLOB, name.buf, HASH_WRITE_OBJECT))
				die_errno("failed to index temp file");

			ce = make_cache_entry(0644, sha1, name.buf, 0, 0);
			if (!ce) die("make_cache_entry failed for path '%s'", name.buf);
			add_index_entry(&ref->index, ce, ADD_CACHE_OK_TO_ADD | ADD_CACHE_OK_TO_REPLACE);
			ref->dirty = 1;

		} else if (!strcmp(cmd, "delete-entry")) {
			int i;

			/* name, [revno], dir-token */
			read_name(&name);
			ref = find_svnref_by_path(&name, 0);

			/* ignore this if we already have the revision */
			if (!ref || (ref->obj && ref->obj->rev >= rev)) continue;

			ref->dirty = 1;

			if (!name.len) {
				/* delete branch */
				ref->delete = 1;
				continue;
			}

			i = index_name_pos(&ref->index, name.buf, name.len);

			if (i >= 0) {
				/* file */
				remove_index_entry_at(&ref->index, i);
				continue;
			}

			/* we've got to re-lookup the path as a < a.c < a/c */
			strbuf_addch(&name, '/');
			i = -index_name_pos(&ref->index, name.buf, name.len) - 1;

			/* directory, index_name_pos returns -first-1
			 * where first is the position the entry would
			 * be added at, and the cache is sorted */
			while (i < ref->index.cache_nr) {
				struct cache_entry* ce = ref->index.cache[i];
				if (ce_namelen(ce) < name.len) break;
				if (memcmp(ce->name, name.buf, name.len)) break;

				ce->ce_flags |= CE_REMOVE;
				i++;
			}

			remove_marked_cache_entries(&ref->index);

		} else if (!strcmp(cmd, "apply-textdelta")) {
			/* file-token, [base-checksum] */
			int64_t d;

			/* ignore this if we already have the revision */
			if (!ref || (ref->obj && ref->obj->rev >= rev)) continue;

			d = deltamore();
			if (d > 0) {
				apply_svndiff(tmpf, srcp, srcn, &deltar, &d, &deltaeof);
			}
		}
	}

	read_end_response(); /* end of close-edit */
	read_success();

	free(srcp);
	strbuf_release(&name);
	return;

err:
	die("malformed update");
}

struct author {
	char* user;
	char* pass;
	char* name;
	char* mail;
};

struct author* authors;
size_t authorn, authoralloc;

static char* strip_space(char* p) {
	char* e = p + strlen(p);

	while (*p == ' ' || *p == '\t') {
		p++;
	}

	while (e > p && (e[-1] == ' ' || e[-1] == '\t')) {
		*(--e) = '\0';
	}

	return p;
}

static void parse_authors() {
	char* p;
	struct stat st;
	int fd = open(git_path("svn-authors"), O_RDONLY);
	if (fd < 0 || fstat(fd, &st)) return;

	p = xmalloc(st.st_size + 1);
	if (xread(fd, p, st.st_size) != st.st_size)
	       	die("read failed on authors");

	p[st.st_size] = '\0';

	while (p && *p) {
		struct author a;
		char* line = strchr(p, '\n');
		if (line) *(line++) = '\0';

		a.user = p;

		p = strchr(p, '=');
		if (!p) goto nextline; /* empty line */
		*(p++) = '\0';
		a.name = p;

		p = strchr(p, '<');
		if (!p) die("invalid author entry for %s", a.user);
		*(p++) = '\0';
		a.mail = p;

		p = strchr(p, '>');
		if (!p) die("invalid author entry for %s", a.user);
		*(p++) = '\0';
		a.pass = p;

		a.user = strip_space(a.user);
		a.name = strip_space(a.name);
		a.mail = strip_space(a.mail);

		p = strchr(a.user, ':');
		if (p) {
			*p = '\0';
			a.pass = p+1;
		} else {
			a.pass = NULL;
		}

		if (*a.user == '#') {
			/* comment */
		} else {
			ALLOC_GROW(authors, authorn + 1, authoralloc);
			authors[authorn++] = a;
		}

nextline:
		p = line;
	}

	close(fd);
}

static void svn_author_to_git(struct strbuf* author) {
	int i;

	for (i = 0; i < authorn; i++) {
		struct author* a = &authors[i];
		if (!strcasecmp(author->buf, a->user)) {
			strbuf_reset(author);
			strbuf_addf(author, "%s <%s>", a->name, a->mail);
			return;
		}
	}

	die("could not find username '%s' in %s\n"
			"Add a line of the form:\n"
			"%s = Full Name <email@example.com>\n",
			author->buf,
			git_path("svn-authors"),
			author->buf);
}

static struct author* get_object_author(struct object* obj) {
	const char *lb, *le, *mb, *me;
	struct strbuf buf = STRBUF_INIT;
	struct author* ret = NULL;
	char* data = NULL;
	int i;

	if (obj->type == OBJ_COMMIT) {
		struct commit* cmt = (struct commit*) obj;
		parse_commit(cmt);
		lb = strstr(cmt->buffer, "\ncommitter ");
		if (!lb) lb = strstr(cmt->buffer, "\nauthor ");
	} else if (obj->type == OBJ_TAG) {
		enum object_type type;
		unsigned long size;
		data = read_sha1_file(obj->sha1, &type, &size);
		if (!data || type != OBJ_TAG) goto err;
		lb = strstr(data, "\ntagger ");
	} else {
		die("invalid commit object");
	}

	if (!lb) goto err;
	le = strchr(lb+1, '\n');
	if (!le) goto err;
	mb = memchr(lb, '<', le - lb);
	if (!mb) goto err;
	me = memchr(mb, '>', le - mb);
	if (!me) goto err;

	strbuf_add(&buf, mb+1, me - (mb+1));

	for (i = 0; i < authorn; i++) {
		struct author* a = &authors[i];
		if (strcasecmp(buf.buf, a->mail)) continue;
		if (!a->pass) {
			die("need password for user '%s' in %s\n"
				"Add a line of the form:\n"
				"%s:password = Full Name <%s>\n",
				a->user,
				git_path("svn-authors"),
				a->user,
				a->mail);
		}

		ret = a;
		break;
	}

	if (!ret) {
		die("could not find username/password for %s in %s\n"
				"Add a line of the form:\n"
				"username:password = Full Name <%s>\n",
				buf.buf,
				git_path("svn-authors"),
				buf.buf);
	}

	strbuf_release(&buf);
	free(data);
	return ret;

err:
	die("can not find author in %s", sha1_to_hex(obj->sha1));
}

static int svn_time_to_git(struct strbuf* time) {
	struct tm tm;
	if (!strptime(time->buf, "%Y-%m-%dT%H:%M:%S", &tm)) return -1;
	strbuf_reset(time);
	strbuf_addf(time, "%"PRId64, (int64_t) mktime(&tm));
	return 0;
}

static int has_tree_changed(unsigned char* tree1, unsigned char* cmt1) {
	struct commit* cmt;
	if (is_null_sha1(tree1) || is_null_sha1(cmt1))
		return 1;

	cmt = lookup_commit(cmt1);
	if (!cmt || parse_commit(cmt))
		return 1;

	return hashcmp(tree1, cmt->tree->object.sha1);
}

static int create_fetched_commit(struct svnref* r, int rev, const char* log, const char* author, const char* time) {
	static struct strbuf buf = STRBUF_INIT;

	unsigned char sha1[20];
	struct ref_lock* ref_lock = NULL;

	if (!r->index.cache_tree)
		r->index.cache_tree = cache_tree();
	if (cache_tree_update(r->index.cache_tree, r->index.cache, r->index.cache_nr, 0))
		die("failed to update cache tree");

	if (r->copysrc) {
		static int fetch_recursion = 0;
		struct svnobj* from;

		if (++fetch_recursion > 5) {
			warning("loop in copys - ignoring any more");
			goto finish_copysrc;
		}

		/* in the corner case where a single commit first edits
		 * a branch and then copies, we need to commit the src
		 * first */
		if (r->copyrev == rev
			&& r->copysrc->dirty
		       	&& create_fetched_commit(r->copysrc, rev, log, author, time)
			) {
			return -1;
		}

		from = find_svnobj(r->copysrc, r->copyrev);
		if (!from) {
			warning("copy from an invalid revision");
			goto finish_copysrc;
		}

		if (!is_null_sha1(r->commit)) {
			warning("copy over an existing parent");
		}

		if (from->istag) {
			struct tag* tag = lookup_tag(from->object);
			if (!tag || parse_tag(tag)) die("expected tag");
			hashcpy(r->commit, tag->tagged->sha1);
		} else {
			hashcpy(r->commit, from->object);
		}

finish_copysrc:
		r->copysrc = NULL;
		fetch_recursion--;
	}

	/* Create the commit object.
	 *
	 * SVN can't create tags and branches without a commit,
	 * but git can. In the cases where new refs are just
	 * created without any changes to the tree, we don't add
	 * a commit. This way git commits pushed to svn and
	 * pulled back again look roughly the same.
	 */
	if (r->delete) {
		hashclr(sha1);
		hashclr(r->commit);

	} else if (r->istag && !has_tree_changed(r->index.cache_tree->sha1, r->commit)) {
		hashcpy(sha1, r->commit);

	} else if (!r->istag
			&& is_null_sha1(r->value)
			&& !is_null_sha1(r->commit)
			&& !has_tree_changed(r->index.cache_tree->sha1, r->commit)
		  ) {
		hashcpy(sha1, r->commit);

	} else {
		strbuf_reset(&buf);
		strbuf_addf(&buf, "tree %s\n", sha1_to_hex(r->index.cache_tree->sha1));

		if (!is_null_sha1(r->commit)) {
			strbuf_addf(&buf, "parent %s\n", sha1_to_hex(r->commit));
		}

		strbuf_addf(&buf, "author %s %s +0000\n", author, time);
		strbuf_addf(&buf, "committer %s %s +0000\n", author, time);

		strbuf_addch(&buf, '\n');
		strbuf_addstr(&buf, log);

		if (write_sha1_file(buf.buf, buf.len, "commit", sha1))
			die("failed to create commit");

		hashcpy(r->commit, sha1);
	}

	/* Create the tag object.
	 *
	 * Now we create an annotated tag wrapped around either
	 * the commit the tag was branched from or the wrapper.
	 * Where a tag is later updated, we either recreate this
	 * tag with a new time (no tree change) or create a new
	 * dummy commit whose parent is the old dummy.
	 */
	if (!r->delete && r->istag) {
		strbuf_reset(&buf);
		strbuf_addf(&buf, "object %s\n"
				"type commit\n"
				"tag %s\n"
				"tagger %s %s +0000\n"
				"\n",
				sha1_to_hex(r->commit),
				r->ref.buf + strlen("refs/tags/"),
				author,
				time);
		strbuf_addstr(&buf, log);

		if (write_sha1_file(buf.buf, buf.len, tag_type, sha1))
			die("failed to create tag");
	}

	/* Create the svn object */
	strbuf_reset(&buf);
	strbuf_addf(&buf, "type svn\n"
			"date %s\n",
			time);

	if (!is_null_sha1(sha1)) {
		strbuf_addf(&buf, "+object %s %s\n",
			       	sha1_to_hex(sha1),
			       	r->istag ? "tag" : "commit");
	}

	if (!is_null_sha1(r->value)) {
		strbuf_addf(&buf, "+parent %s\n", sha1_to_hex(r->value));
	}

	strbuf_addf(&buf, "revision %d\n"
			"path %s\n",
			rev,
			r->svn.buf);

	if (write_sha1_file(buf.buf, buf.len, other_type, sha1))
		die("failed to create svn object");

	/* update the ref */

	ref_lock = lock_ref_sha1(r->ref.buf + strlen("refs/"), r->value);
	if (!ref_lock) die("failed to grab ref lock");

	if (write_ref_sha1(ref_lock, sha1, "svn-fetch")) {
		return error("failed to update ref %s", r->ref.buf);
	}

	/* update the remote or tag ref */

	if (r->delete) {
		if (r->obj && !is_null_sha1(r->obj->object) && delete_ref(r->remote.buf, r->obj->object, 0)) {
			fprintf(stderr, "%p %s %s %s\n", r->obj, sha1_to_hex(r->obj->object), sha1_to_hex(r->commit), sha1_to_hex(r->value));
			return error("failed to delete ref %s", r->remote.buf);
		}
	} else {
		ref_lock = lock_ref_sha1(r->remote.buf + strlen("refs/"),
				r->obj ? r->obj->object : null_sha1);

		if (!ref_lock || write_ref_sha1(ref_lock, r->commit, "svn-fetch"))
			return error("failed to update ref %s", r->remote.buf);
	}

	r->dirty = 0;
	r->delete = 0;
	hashcpy(r->value, sha1);
	r->obj = parse_svnobj(sha1);
	if (!r->obj) die("svn object disappeared?");

	fprintf(stderr, "commited %d %s %s\n", rev, r->ref.buf, sha1_to_hex(r->value));
	return 0;
}

static void get_commit(int rev) {
	static struct strbuf author = STRBUF_INIT;
	static struct strbuf time = STRBUF_INIT;
	static struct strbuf log = STRBUF_INIT;
	static struct strbuf name = STRBUF_INIT;

	int i;
	int errors = 0;

	strbuf_reset(&author);
	strbuf_reset(&time);
	strbuf_reset(&log);
	strbuf_reset(&name);

	fprintf(stderr, "commit start %d\n", rev);

	sendf("( update ( ( %d ) 0: true ) )\n" /* [rev] target recurse */
		"( set-path ( 0: %d %s ) )\n" /* path rev start-empty */
		"( finish-report ( ) )\n"
		"( success ( ) )\n"
		"( log ( ( 0: ) " /* (path...) */
				"( %d ) ( %d ) " /* start/end revno */
				"true false " /* changed-paths strict-node */
				"0 " /* limit */
				"false " /* include-merged-revisions */
				"revprops ( 10:svn:author 8:svn:date 7:svn:log ) "
			") )\n",
		       	rev, /* update target */
			rev - 1, /* set-path rev */
			rev > 1 ? "false" : "true", /* set-path start-empty */
			rev, /* log start */
			rev /* log end */
		);

	read_update(rev);

	/* log response */
	read_success();

	/* commit-info */
	if (read_list()) goto err;

	/* changed path entries */
	if (read_list()) goto err;
	while (!read_list()) {
		const char* s;
		struct svnref *to;

		/* path, A, (copy-path, copy-rev) */
		read_name(&name);
		s = read_word();
		if (!s) goto err;
		if (strcmp(s, "A")) goto finish_changed_path;

		to = find_svnref_by_path(&name, 1);
		if (name.len) goto finish_changed_path;

		if (!have_optional()) goto finish_changed_path;

		to->copysrc = read_copy_source(&name, &to->copyrev);
		if (name.len) {
			warning("copy from non-root path");
			to->copysrc = NULL;
		}

		read_end();
finish_changed_path:
		read_end();
	}
	read_end();

	/* revno */
	if (read_number() != rev) goto err;

	/* author */
	if (read_list()) goto err;
	read_strbuf(&author);
	svn_author_to_git(&author);
	read_end();

	/* timestamp */
	if (read_list()) goto err;
	if (read_strbuf(&time)) goto err;
	if (svn_time_to_git(&time)) goto err;
	read_end();

	/* log message */
	if (read_list()) goto err;
	if (read_strbuf(&log)) goto err;
	strbuf_complete_line(&log);
	read_end();

	/* finish the commit-info */
	read_end();
	if (verbose) fprintf(stderr, "\n");

	read_done();
	read_success();

	/* now commit */

	/* TODO: properly handle multiple refs being updated and bailing
	 * after updating a subset of them */

	for (i = 0; i < refn; i++) {
		if (refs[i]->dirty && create_fetched_commit(refs[i], rev, log.buf, author.buf, time.buf)) {
			errors++;
		}
	}

	if (errors) die("one or more ref updates failed");

	fprintf(stderr, "finished commit %d\n", rev);
	return;
err:
	die(_("malformed commit"));
}

static char* clean_path(char* p) {
	char* e;
	if (*p == '/') p++;
	e = p + strlen(p);
	if (e > p && e[-1] == '/') e[-1] = '\0';
	return p;
}

static void setup_globals() {
	tmpf = tmpfile();
	freopen(NULL, "wb+", tmpf);

	parse_authors();

	if (trunk) trunk = clean_path(trunk);
	if (branches) branches = clean_path(branches);
	if (tags) tags = clean_path(tags);
}

static void reconnect(const char* user, const char* pass) {
	char pathsep;
	char *host, *port, *path;
	struct addrinfo hints, *res, *ai;
	int err;

	if (prefixcmp(url, "svn://"))
		die(_("only svn repositories are supported"));

	if (use_stdin) {
		infd = fileno(stdin);
		outfd = fileno(stdout);
		return;
	}

	if (infd >= 0) close(infd);
	if (outfd >= 0) close(outfd);
	infd = outfd = -1;

	host = (char*) url + strlen("svn://");

	path = strchr(host, '/');
	if (!path) path = host + strlen(host);
	pathsep = *path;
	*path = '\0';

	port = strchr(host, ':');
	if (port) *(port++) = '\0';

	memset(&hints, 0, sizeof(hints));
	hints.ai_socktype = SOCK_STREAM;

	err = getaddrinfo(host, port ? port : "3690", &hints, &res);
	*path = pathsep;

	if (err)
		die("failed to connect to %s:%s", host, port ? port : "3690");

	for (ai = res; ai != NULL; ai = ai->ai_next) {
		int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0) continue;

		if (connect(fd, ai->ai_addr, ai->ai_addrlen)) {
			close(fd);
			continue;
		}

		infd = outfd = fd;
		break;
	}

	if (infd < 0)
		die("failed to connect to %s", url);

	/* TODO: client software version and client capabilities */
	sendf("( 2 ( edit-pipeline svndiff1 ) %d:%s )\n( CRAM-MD5 ( ) )\n",
			(int) strlen(url), url);

	/* TODO: we don't care about capabilities/versions right now */
	if (strcmp(read_response(), "success")) die("server error");

	/* minver then maxver */
	if (read_number() > 2 || read_number() < 2)
		die(_("version mismatch"));

	read_end_response();

	/* TODO: read the mech lists et all */
	read_success();

	if (!user || !pass)
		die("user/pass not specified");

	cram_md5(user, pass);

	sendf("( reparent ( %d:%s ) )\n", (int) strlen(url), url);

	read_success(); /* auth */
	read_success(); /* repo info */
	read_success(); /* reparent */
	read_success(); /* reparent again */
}

static int split_revisions(int* from, int* to) {
	char *s;
	if (!revisions) return 0;

	*to = strtol(revisions, &s, 10);

	if (*to <= 0 || *to >= INT_MAX) return -1;
	if (!*s) return 1;
	if (*s && *s != ':') return -1;

	*from = *to;
	*to = strtol(s, &s, 10);
	if (*to <= 0 || *to >= INT_MAX) return -1;
	if (*to < *from) return -1;

	return 2;
}

static int latest_rev() {
	char buf[256];
	ssize_t len;
	int fd;

	fd = open(git_path("svn-latest"), O_RDONLY);
	if (fd < 0 && errno == ENOENT) return 0;
	if (fd < 0) die_errno("open %s", git_path("svn-latest"));

	len = read_in_full(fd, buf, sizeof(buf)-1);
	if (len < 0) die_errno("read %s", git_path("svn-latest"));
	close(fd);

	buf[len] = '\0';
	return atoi(buf);
}

static void set_latest_rev(int rev) {
	static struct lock_file lk;
	char buf[256];
	int fd;

	sprintf(buf, "%d\n", rev);
	fd = hold_lock_file_for_update(&lk, git_path("svn-latest"), LOCK_DIE_ON_ERROR);
	if (write_in_full(fd, buf, strlen(buf)) != strlen(buf))
		die_errno("write %s", git_path("svn-latest"));
	commit_lock_file(&lk);
}

int cmd_svn_fetch(int argc, const char **argv, const char *prefix) {
	int64_t n;
	int from, to;

	argc = parse_options(argc, argv, prefix, builtin_svn_fetch_options,
		       	builtin_svn_fetch_usage, 0);

	if (argc != 1)
		usage_msg_opt(argc > 1 ? _("Too many arguments.") : _("Too few arguments"),
			builtin_svn_fetch_usage, builtin_svn_fetch_options);

	url = argv[0];
	unpack_svnref = 1;
	setup_globals();
	reconnect(svnuser, svnpass);

	switch (split_revisions(&from, &to)) {
	case 0:
		from = latest_rev() + 1;
		to = INT_MAX;
		break;
	case 1:
		from = latest_rev() + 1;
		break;
	case 2:
		break;
	default:
		die("invalid revision range");
	}

nextpoll:
	sendf("( get-latest-rev ( ) )\n");

	read_success(); /* latest rev */
	read_response(); /* latest rev again */
	n = read_number();
	if (n < 0 || n > INT_MAX) die("latest-rev failed");
	to = min(to, (int) n);
	read_end_response();

	fprintf(stderr, "rev %d %d\n", from, to);
	while (from <= to) {
		get_commit(from);
		set_latest_rev(from);
		from++;
	}

	if (interval > 0) {
		sleep(interval);
		goto nextpoll;
	} else if (interval == 0) {
		select(0, NULL, NULL, NULL, NULL);
		goto nextpoll;
	}

	return 0;
}

static const char* dtoken(int dir) {
	static int bufnum;
	static char bufs[4][32];
	char* buf1 = bufs[bufnum++ & 3];
	char* buf2 = bufs[bufnum++ & 3];
	sprintf(buf1, "d%d", dir);
	sprintf(buf2, "%d:%s", (int) strlen(buf1), buf1);
	return buf2;
}

static int fcount;
static struct svnref* curref;

static const char* ftoken() {
	static char buf[32];
	sprintf(buf, "c%d", ++fcount);
	sprintf(buf, "%d:c%d", (int) strlen(buf), fcount);
	return buf;
}

/* check that no commits have been inserted on our branch between from
 * (the previous revision at which we saw a change) and to (the revision
 * we just commited) */
static void check_for_svn_commits(struct svnref* r, int from, int to) {
	if (from + 1 <= to) {
		return;
	}

	sendf("( log ( ( %d:%s ) " /* (path...) */
			"( %d ) ( %d ) " /* start/end revno */
			"false false " /* changed-paths strict-node */
			"0 false " /* limit include-merged-revisions */
			"revprops ( ) ) )\n",
		(int) r->svn.len,
		r->svn.buf,
		from + 1,
		to - 1);

	read_success();
	if (!read_list()) {
		die("commits inserted during push");
	}

	read_done();
	read_success();
}

static size_t common_directory(const char* a, const char* b, int* depth) {
	int off;
	const char* ab = a;

	off = 0;
	while (*a && *b && *a == *b) {
		if (*a == '/') {
			(*depth)++;
			off = a + 1 - ab;
		}
		a++;
		b++;
	}

	return off;
}

static struct strbuf cpath = STRBUF_INIT;
static int cdepth;

static int change_dir(const char* path) {
	const char *p, *d;
	int off, depth = 0;

	off = common_directory(path, cpath.buf, &depth);

	/* cd .. to the common root */
	while (cdepth > depth) {
		sendf("( close-dir ( %s ) )\n", dtoken(cdepth));
		cdepth--;
	}

	strbuf_setlen(&cpath, off);

	/* cd down to the new path */
	d = p = path + off;
	for (;;) {
		char* d = strchr(p, '/');
		if (!d) break;

		sendf("( open-dir ( %d:%.*s %s %s ( ) ) )\n",
			(int) (d - path), (int) (d - path), path,
			dtoken(cdepth),
			dtoken(cdepth+1));

		/* include the / at the end */
		d++;
		strbuf_add(&cpath, p, d - p);
		p = d;
		cdepth++;
	}

	return cdepth;
}

static void dir_changed(int dir, const char* path) {
	strbuf_reset(&cpath);
	strbuf_addstr(&cpath, path);
	if (*path) strbuf_addch(&cpath, '/');
	cdepth = dir;
}

static void send_delta_chunk(const char* tok, const void* data, size_t sz) {
	sendf("( textdelta-chunk ( %s %d:", tok, (int) sz);
	sendb(data, sz);
	sendf(" ) )\n");
}

static void change(struct diff_options* op,
	       	unsigned omode,
	       	unsigned nmode,
		const unsigned char* osha1,
		const unsigned char* nsha1,
		const char* path,
		unsigned odsubmodule,
		unsigned ndsubmodule)
{
	unsigned char ins[MAX_INS_LEN], *inp = ins;
	unsigned char hdr[5*MAX_VARINT_LEN], *hp = hdr;
	enum object_type type;
	const char* tok;
	void* data;
	size_t sz;
	int dir;

	if (verbose) fprintf(stderr, "change mode %x/%x sha1 %s/%s path %s\n",
			omode, nmode, sha1_to_hex(osha1), sha1_to_hex(nsha1), path);

	/* dont care about changed directories */
	if (!S_ISREG(nmode)) return;

	dir = change_dir(path);

	/* TODO make this actually use diffcore */

	data = read_sha1_file(nsha1, &type, &sz);

	if (type != OBJ_BLOB)
		die("unexpected object type for %s", sha1_to_hex(nsha1));

	inp = encode_instruction(inp, COPY_FROM_NEW, 0, sz);

	hp = encode_varint(hp, 0); /* source off */
	hp = encode_varint(hp, 0); /* source len */
	hp = encode_varint(hp, sz); /* target len */
	hp = encode_varint(hp, inp - ins); /* ins len */
	hp = encode_varint(hp, sz); /* data len */

	tok = ftoken();
	sendf("( open-file ( %d:%s %s %s ( ) ) )\n"
		"( apply-textdelta ( %s ( ) ) )\n",
		(int) strlen(path), path, dtoken(dir), tok,
		tok);

	send_delta_chunk(tok, "SVN\0", 4);
	send_delta_chunk(tok, hdr, hp - hdr);
	send_delta_chunk(tok, ins, inp - ins);
	send_delta_chunk(tok, data, sz);

	sendf("( textdelta-end ( %s ) )\n"
		"( close-file ( %s ( ) ) )\n",
		tok, tok);

	diff_change(op, omode, nmode, osha1, nsha1, path, odsubmodule, ndsubmodule);
}

static void addremove(struct diff_options* op,
		int addrm,
		unsigned mode,
		const unsigned char* sha1,
		const char* path,
		unsigned dsubmodule)
{
	static struct strbuf rmdir = STRBUF_INIT;
	static int in_rmdir;
	int dir;
	size_t plen = strlen(path);

	if (verbose) fprintf(stderr, "%s mode %x sha1 %s path %s\n",
			addrm == '+' ? "add" : "remove", mode, sha1_to_hex(sha1), path);

	/* diff recursively returns deleted folders, but svn only needs
	 * the root */
	if (addrm == '-' && in_rmdir && plen >= rmdir.len && !memcmp(path, rmdir.buf, rmdir.len)) {
		return;
	}

	in_rmdir = 0;
	dir = change_dir(path);

	if (addrm == '-' && S_ISDIR(mode)) {
		strbuf_reset(&rmdir);
		strbuf_add(&rmdir, path, plen);
		strbuf_addch(&rmdir, '/');
		in_rmdir = 1;

		sendf("( delete-entry ( %d:%s ( ) %s ) )\n",
			(int) plen, path, dtoken(dir));

	} else if (addrm == '+' && S_ISDIR(mode)) {
		sendf("( add-dir ( %d:%s %s %s ( ) ) )\n",
			(int) plen, path, dtoken(dir), dtoken(dir+1));

		dir_changed(++dir, path);

	} else if (addrm == '-' && S_ISREG(mode)) {
		sendf("( delete-entry ( %d:%s ( ) %s) )\n",
			(int) plen, path, dtoken(dir));

	} else if (addrm == '+' && S_ISREG(mode)) {
		unsigned char ins[MAX_INS_LEN], *inp = ins;
		unsigned char hdr[5*MAX_VARINT_LEN], *hp = hdr;
		enum object_type type;
		const char* tok;
		void* data;
		size_t sz;

		data = read_sha1_file(sha1, &type, &sz);

		if (type != OBJ_BLOB)
			die("unexpected object type for %s", sha1_to_hex(sha1));

		inp = encode_instruction(inp, COPY_FROM_NEW, 0, sz);

		hp = encode_varint(hp, 0); /* source off */
		hp = encode_varint(hp, 0); /* source len */
		hp = encode_varint(hp, sz); /* target len */
		hp = encode_varint(hp, inp - ins); /* ins len */
		hp = encode_varint(hp, sz); /* data len */

		/* TOOD: find copies */

		tok = ftoken();
		sendf("( add-file ( %d:%s %s %s ( ) ) )\n"
			"( apply-textdelta ( %s ( ) ) )\n",
			(int) strlen(path), path, dtoken(dir), tok,
			tok);

		send_delta_chunk(tok, "SVN\0", 4);
		send_delta_chunk(tok, hdr, hp - hdr);
		send_delta_chunk(tok, ins, inp - ins);
		send_delta_chunk(tok, data, sz);

		sendf("( textdelta-end ( %s ) )\n"
			"( close-file ( %s ( ) ) )\n",
			tok, tok);
	}

	diff_addremove(op, addrm, mode, sha1, path, dsubmodule);
}

static void output(struct diff_queue_struct *q,
		struct diff_options* op,
		void* data)
{
	int i;
	fprintf(stderr, "output %d %p\n", q->nr, data);
	for (i = 0; i < q->nr; i++) {
		struct diff_filepair* p = q->queue[i];
		fprintf(stderr, "output %s %s\n", p->one->path, p->two->path);
	}
}

static int read_commit_revno(struct strbuf* time) {
	int64_t n;

	read_success();
	read_success();

	/* commit-info */
	if (read_list()) goto err;
	n = read_number();
	if (n < 0 || n > INT_MAX) goto err;
	if (time) {
		read_strbuf(time);
		svn_time_to_git(time);
	}
	read_end();
	if (verbose) fputc('\n', stderr);

	return (int) n;

err:
	die("commit failed");
}

/* returns the rev number */
static int send_commit(struct svnref* r, struct commit* cmt, struct svnobj* copysrc, const char* log, struct strbuf* time) {
	struct diff_options op;
	int dir;

	fcount = 0;
	curref = r;

	sendf("( commit ( %d:%s ) )\n"
		"( open-root ( ( ) %s ) )\n",
		(int) strlen(log), log,
		dtoken(0));

	read_success();
	read_success();

	dir = change_dir(r->svn.buf);

	if (cmt) {
		if (copysrc) {
			if (!r->create) die("huh? copysrc without create");
			sendf("( add-dir ( %d:%s %s %s ( %d:%s %d ) ) )\n",
					(int) r->svn.len,
					r->svn.buf,
					dtoken(dir),
					dtoken(dir+1),
					(int) strlen(copysrc->path),
					copysrc->path,
					copysrc->rev);
		} else {
			sendf("( %s-dir ( %d:%s %s %s ( ) ) )\n",
				r->create ? "add" : "open",
				(int) r->svn.len,
				r->svn.buf,
				dtoken(dir),
				dtoken(dir+1));
		}

		dir_changed(++dir, r->svn.buf);

		diff_setup(&op);
		op.output_format = DIFF_FORMAT_CALLBACK;
		op.change = &change;
		op.add_remove = &addremove;
		op.format_callback = &output;
		DIFF_OPT_SET(&op, RECURSIVE);
		DIFF_OPT_SET(&op, IGNORE_SUBMODULES);
		DIFF_OPT_SET(&op, TREE_IN_RECURSIVE);

		fprintf(stderr, "diff %s to %s\n", sha1_to_hex(r->commit), sha1_to_hex(cmt->object.sha1));
		if (diff_tree_sha1(r->commit, cmt->object.sha1, "/", &op))
			die("diff tree failed");
	} else {
		sendf("( delete-entry ( %d:%s ( ) %s ) )\n",
				(int) r->svn.len,
				r->svn.buf,
				dtoken(dir));
	}

	change_dir("");
	sendf("( close-dir ( %s ) )\n"
		"( close-edit ( ) )\n",
		dtoken(0));

	return read_commit_revno(time);
}

struct push {
	struct push* next;
	struct commit* old;
	struct commit* new;
	struct tag* tag;
	struct svnref* ref;
	struct svnobj* copysrc;
};

/* returns the rev number, cmt may be NULL if this is a forced ref
 * creation/deletion, in which case tag is used for the author and
 * message or a fallback author/msg is used (for branch creation or
 * branch/tag deletion) */
static int push_commit(struct push* p, struct commit* cmt, struct tag* tag) {
	static struct strbuf buf = STRBUF_INIT;
	static struct strbuf time = STRBUF_INIT;
	static struct strbuf logbuf = STRBUF_INIT;
	static const char* prevuser;

	const char* user;
	const char* pass;
	unsigned char sha1[20];
	struct ref_lock* ref_lock;
	const char* log;
	int rev;
	struct svnref* r = p->ref;

	fprintf(stderr, "push_commit %s %s\n", sha1_to_hex(cmt ? cmt->object.sha1 : null_sha1), sha1_to_hex(tag ? tag->object.sha1 : null_sha1));

	if (cmt || tag) {
		struct author* a = get_object_author(tag ? &tag->object : &cmt->object);
		user = a->user;
		pass = a->pass;
	} else {
		user = svnuser;
		pass = svnpass;
	}

	if (user != prevuser) {
		reconnect(user, pass);
		prevuser = user;
	}

	if (tag) {
		parse_tag(tag);
		strbuf_reset(&logbuf);
		strbuf_addstr(&logbuf, tag->tag);
		strbuf_setlen(&logbuf, parse_signature(logbuf.buf, logbuf.len));
		log = logbuf.buf;
	} else if (cmt) {
		find_commit_subject(cmt->buffer, &log);
	} else {
		strbuf_reset(&logbuf);
		strbuf_addf(&logbuf, "%s %s",
			r->create ? "creating" : "removing",
			r->svn.buf);
		log = logbuf.buf;
	}

	strbuf_reset(&time);
	rev = send_commit(r, cmt, p->copysrc, log, &time);
	p->copysrc = NULL;

	/* If we find any intermediate commits, we die. They
	 * will be picked up the next time the user does a pull.
	 */
	check_for_svn_commits(r, r->obj ? r->obj->rev : 0, rev);

	/* create the svn object */

	strbuf_reset(&buf);
	strbuf_addf(&buf, "type svn\n"
			"date %s\n",
			time.buf);

	if (tag) {
		strbuf_addf(&buf, "+object %s tag\n",
			       	sha1_to_hex(tag->object.sha1));
	} else if (cmt) {
		strbuf_addf(&buf, "+object %s commit\n",
			       	sha1_to_hex(cmt->object.sha1));
	}

	if (!is_null_sha1(r->value)) {
		strbuf_addf(&buf, "+parent %s\n", sha1_to_hex(r->value));
	}

	strbuf_addf(&buf, "revision %d\n"
			"path %s\n",
			rev,
			r->svn.buf);

	if (write_sha1_file(buf.buf, buf.len, other_type, sha1))
		die("failed to create svn object");

	/* update the ref */

	ref_lock = lock_ref_sha1(r->ref.buf + strlen("refs/"), r->value);
	if (!ref_lock) die("failed to grab ref lock");

	if (write_ref_sha1(ref_lock, sha1, "svn-push"))
		die("failed to update ref %s", r->ref.buf);

	if (r->istag) {
		/* since tag 'remotes' are stored in refs/tags/, we let
		 * the caller delete it */
	} else if (cmt) {
		ref_lock = lock_ref_sha1(r->remote.buf + strlen("refs/"), r->commit);
		if (!ref_lock || write_ref_sha1(ref_lock, cmt->object.sha1, "svn-push"))
			die("failed to update ref %s", r->remote.buf);
	} else {
		if (delete_ref(r->remote.buf + strlen("refs/"), r->commit, 0))
			die("failed to delete ref %s", r->remote.buf);
	}

	hashcpy(r->value, sha1);
	hashcpy(r->commit, cmt ? cmt->object.sha1 : null_sha1);
	r->obj = parse_svnobj(sha1);
	if (!r->obj) die("svn object disappeared?");

	return rev;
}

/* returns the committed revno */
static int do_push(struct push* p) {
	struct svnref* r = p->ref;
	struct commit* cmt;
	struct rev_info walk;
	int rev = 0;

	if (hashcmp(r->commit, p->old ? p->old->object.sha1 : null_sha1)) {
		die("non fast-forward for %s", r->ref.buf);
	}

	if (p->new) {
		init_revisions(&walk, NULL);
		add_pending_object(&walk, &p->new->object, "to");
		walk.first_parent_only = 1;
		walk.reverse = 1;

		if (p->old) {
			p->old->object.flags |= UNINTERESTING;
			add_pending_object(&walk, &p->old->object, "from");
		}

		if (prepare_revision_walk(&walk))
			die("prepare rev walk failed");

		while ((cmt = get_revision(&walk)) != NULL) {
			/* when updating a tag that has multiple
			 * commits, the first few are wrapped commits
			 * and only the last is a wrapped tag */
			rev = push_commit(p, cmt, cmt == p->new ? p->tag : NULL);
			r->create = 0;
		}

		/* if there were no commits we may have to create a fake
		 * commit to create the branch in svn */
		if (r->create) {
			rev = push_commit(p, NULL, p->tag);
		}
	} else {
		rev = push_commit(p, NULL, NULL);
	}

	return rev;
}

static struct commit* lookup_commit_indirect(const unsigned char* sha1, struct tag** ptag) {
	struct object* obj;
	struct tag* tag;

	if (is_null_sha1(sha1)) {
		return NULL;
	}

	obj = parse_object(sha1);

	switch (obj->type) {
	case OBJ_COMMIT:
		return (struct commit*) obj;

	case OBJ_TAG:
		tag = (struct tag*) obj;
		if (!tag->tagged || tag->tagged->type != OBJ_COMMIT) {
			die("invalid tag");
		}
		if (ptag) *ptag = tag;
		return (struct commit*) tag->tagged;

	default:
		die("invalid object");
	}
}

int cmd_svn_push(int argc, const char **argv, const char *prefix) {
	struct push *updates = NULL, *p;
	unsigned char old[20], new[20];
	char buf[256];
	int latest = 0;
	int i;

	argc = parse_options(argc, argv, prefix, builtin_svn_push_options,
			builtin_svn_push_usage, 0);

	setup_globals();

	if (!svnuser || !svnpass) {
		die("need to specify user and pass");
	}

	/* url set by --pre-receive option */
	if (!url) {
		if (argc != 4)
			usage_msg_opt(argc > 4 ? _("Too many arguments.") : _("Too few arguments"),
				builtin_svn_push_usage, builtin_svn_push_options);

		url = argv[0];

		p = xcalloc(1, sizeof(*p));

		if (get_sha1(argv[2], old))
		       die("invalid ref %s", argv[2]);
		if (get_sha1(argv[3], new))
		       die("invalid ref %s", argv[3]);

		p->old = lookup_commit_indirect(old, NULL);
		p->new = lookup_commit_indirect(new, &p->tag);

		p->ref = find_svnref_by_refname(argv[1], p->old == NULL);
		do_push(p);
		return 0;
	}

	if (argc)
		usage_msg_opt( _("Too many arguments."),
			builtin_svn_push_usage, builtin_svn_push_options);

	add_all_refs();
	for (i = 0; i < refn; i++) {
		struct svnref* r = refs[i];
		if (r->obj && r->obj->rev > latest) {
			latest = r->obj->rev;
		}
	}

	while (fgets(buf, sizeof(buf), stdin)
			&& strlen(buf) >= 41 + 41
			&& buf[strlen(buf)-1] == '\n'
			&& !get_sha1_hex(buf, old)
			&& !get_sha1_hex(&buf[41], new)) {

		buf[strlen(buf)-1] = '\0';
		p = xcalloc(1, sizeof(*p));

		p->old = lookup_commit_indirect(old, NULL);
		p->new = lookup_commit_indirect(new, &p->tag);

		p->ref = find_svnref_by_refname(&buf[82], p->old == NULL);
		p->next = updates;
		updates = p;
	}

	/* modify and delete refs */
	for (p = updates; p != NULL; p = p->next) {
		if (p->old) {
			latest = do_push(p);
		}
	}

	/* add refs - do this last so we can find copy bases in the
	 * modified refs. Note if two added refs have a common commit
	 * branching off of svn then those common commits will be
	 * assigned to whichever ref comes first (i.e. unspecified). */
	for (p = updates; p != NULL; p = p->next) {
		if (!p->old) {
			p->copysrc = find_copy_source(p->new, latest);
			latest = do_push(p);
		}

	}

	return 0;
}
