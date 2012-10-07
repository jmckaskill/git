#include "remote-svn.h"
#include "version.h"
#include "quote.h"
#include <openssl/md5.h>

#ifndef NO_PTHREADS
#include <pthread.h>
static pthread_mutex_t lock;
#else
#define pthread_mutex_lock(x)
#define pthread_mutex_unlock(x)
#endif

#ifndef min
#define min(a,b) ((a) < (b) ? (a) : (b))
#endif

#define MAX_DBG_WIDTH INT_MAX

struct conn {
	int fd, b, e;
	char in[4096];
	struct strbuf indbg, buf, word;
};

static const char *user_agent;
static struct strbuf url;
static struct credential *svn_auth;
struct conn main_connection;

static int readc(struct conn *c) {
	if (c->b == c->e) {
		c->b = 0;
		c->e = xread(c->fd, c->in, sizeof(c->in));
		if (c->e <= 0) return EOF;
	}

	return c->in[c->b++];
}

static void unreadc(struct conn *c) {
	c->b--;
}

static ssize_t read_svn(struct conn *c, void* p, size_t n) {
	/* big reads we may as well read directly into the target */
	if (c->e == c->b && n >= sizeof(c->in) / 2) {
		return xread(c->fd, p, n);

	} else if (c->e == c->b) {
		c->b = 0;
		c->e = xread(c->fd, c->in, sizeof(c->in));
		if (c->e <= 0) return c->e;
	}

	n = min(n, c->e - c->b);
	memcpy(p, c->in + c->b, n);
	c->b += n;
	return n;
}

static void writedebug(struct conn *c, struct strbuf *s, int rxtx) {
	struct strbuf buf = STRBUF_INIT;
	strbuf_addf(&buf, "%d %c", c->fd, rxtx);
	if (s->len && s->buf[0] != ' ')
		strbuf_addch(&buf, ' ');
	quote_c_style_counted(s->buf, min(s->len - 1, MAX_DBG_WIDTH), &buf, NULL, 1);
	if (buf.len >= MAX_DBG_WIDTH-3) {
		strbuf_setlen(&buf, MAX_DBG_WIDTH-3);
		strbuf_addstr(&buf, "...");
	}
	strbuf_addch(&buf, '\n');
	fwrite(buf.buf, 1, buf.len, stderr);
	strbuf_release(&buf);
}

static void sendbuf(struct conn *c) {
	struct strbuf *s = &c->buf;

	if (svndbg >= 2)
		writedebug(c, s, '+');

	if (write_in_full(c->fd, s->buf, s->len) != s->len)
		die_errno("write");
}

__attribute__((format (printf,2,3)))
static void sendf(struct conn *c, const char* fmt, ...);

static void sendf(struct conn *c, const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	strbuf_reset(&c->buf);
	strbuf_vaddf(&c->buf, fmt, ap);
	sendbuf(c);
}

/* returns -1 if it can't find a number */
static ssize_t read_number(struct conn *c) {
	ssize_t v;

	for (;;) {
		int ch = readc(c);
		if ('0' <= ch && ch <= '9') {
			v = ch - '0';
			break;
		} else if (ch != ' ' && ch != '\n') {
			unreadc(c);
			return -1;
		}
	}

	for (;;) {
		int ch = readc(c);
		if (ch < '0' || ch > '9') {
			unreadc(c);
			if (svndbg >= 2)
				strbuf_addf(&c->indbg, " %d", (int) v);
			return v;
		}

		if (v > INT64_MAX/10) {
			die(_("number too big"));
		} else {
			v = 10*v + (ch - '0');
		}
	}
}

/* returns -1 if it can't find a list */
static int read_list(struct conn *c) {
	for (;;) {
		int ch = readc(c);
		if (ch == '(') {
			if (svndbg >= 2)
				strbuf_addstr(&c->indbg, " (");
			return 0;
		} else if (ch != ' ' && ch != '\n') {
			unreadc(c);
			return -1;
		}
	}
}

/* returns NULL if it can't find an atom, string only valid until next
 * call to read_word */
static const char *read_word(struct conn *c) {
	int ch;

	strbuf_reset(&c->word);

	for (;;) {
		ch = readc(c);
		if (('a' <= ch && ch <= 'z') || ('A' <= ch && ch <= 'Z')) {
			break;
		} else if (ch != ' ' && ch != '\n') {
			unreadc(c);
			return NULL;
		}
	}

	while (('a' <= ch && ch <= 'z') || ('A' <= ch && ch <= 'Z')
			|| ('0' <= ch && ch <= '9')
			|| ch == '-')
	{
		strbuf_addch(&c->word, ch);
		ch = readc(c);
	}

	unreadc(c);
	if (svndbg >= 2)
		strbuf_addf(&c->indbg, " %s", c->word.buf);
	return c->word.len ? c->word.buf : NULL;
}

/* returns -1 if no string or an invalid string */
static int append_string(struct conn *c, struct strbuf* s) {
	size_t i;
	ssize_t n = read_number(c);
	if (n < 0 || unsigned_add_overflows(s->len, (size_t) n))
		return -1;
	if (readc(c) != ':')
		die(_("malformed string"));
	if (svndbg >= 2)
		strbuf_addch(&c->indbg, ':');

	strbuf_grow(s, s->len + n);

	i = 0;
	while (i < n) {
		ssize_t r = read_svn(c, s->buf + s->len, n-i);
		if (r < 0)
			die_errno("read error");
		if (r == 0)
			die("short read");
		strbuf_setlen(s, s->len + r);
		i += r;
	}

	if (svndbg >= 2)
		strbuf_add(&c->indbg, s->buf + s->len - n, n);

	return 0;
}

static int read_string(struct conn *c, struct strbuf *s) {
	strbuf_reset(s);
	return append_string(c, s);
}

static void read_end(struct conn *c) {
	int parens = 1;
	while (parens > 0) {
		int ch = readc(c);
		if (ch == EOF)
			die(_("socket close whilst looking for list close"));

		if (ch == '(') {
			if (svndbg >= 2)
				strbuf_addstr(&c->indbg, " (");
			parens++;
		} else if (ch == ')') {
			if (svndbg >= 2)
				strbuf_addstr(&c->indbg, " )");
			parens--;
		} else if (ch == ' ' || ch == '\n') {
			/* whitespace */
		} else if ('0' <= ch && ch <= '9') {
			/* number or string */
			size_t n;
			char buf[4096];

			unreadc(c);
			n = read_number(c);

			ch = readc(c);
			if (ch != ':') {
				/* number */
				unreadc(c);
				continue;
			}

			/* string */
			if (svndbg >= 2)
				strbuf_addch(&c->indbg, ':');

			while (n) {
				ssize_t r = read_svn(c, buf, min(n, sizeof(buf)));
				if (r <= 0)
					die_errno("read");
				if (svndbg >= 2)
					strbuf_add(&c->indbg, buf, r);
				n -= r;
			}
		} else {
			unreadc(c);
			if (!read_word(c))
				die(_("unexpected character %c"), ch);
		}
	}
}

static const char* read_command(struct conn *c) {
	const char *cmd;

	if (read_list(c)) goto err;

	cmd = read_word(c);
	if (!cmd) goto err;
	if (read_list(c)) goto err;

	if (!strcmp(cmd, "failure")) {
		while (!read_list(c)) {
			struct strbuf msg = STRBUF_INIT;
			read_number(c);
			read_string(c, &msg);
			error("%s", msg.buf);
			strbuf_release(&msg);
			read_end(c);
		}
	}

	return cmd;
err:
	die(_("malformed response"));
}

static void read_newline(struct conn *c) {
	if (svndbg >= 2) {
		strbuf_addch(&c->indbg, '\n');
		writedebug(c, &c->indbg, '-');
		strbuf_reset(&c->indbg);
	}
}

static void read_command_end(struct conn *c) {
	read_end(c);
	read_end(c);
	read_newline(c);
}

static int read_success(struct conn *c) {
	int ret = strcmp(read_command(c), "success");
	read_command_end(c);
	return ret;
}

static int read_done(struct conn *c) {
	const char* s = read_word(c);
	if (!s || strcmp(s, "done"))
		return -1;
	read_newline(c);
	return 0;
}

/* returns 0 if the list is missing or empty (and skips over it), 1 if
 * its present and has values */
static int have_optional(struct conn *c) {
	if (read_list(c))
		return 0;
	for (;;) {
		int ch = readc(c);
		if (ch == ')') {
			if (svndbg >= 2)
				strbuf_addstr(&c->indbg, " )");
			return 0;
		} else if (ch != ' ' && ch != '\n') {
			unreadc(c);
			return 1;
		}
	}
}

static void cram_md5(struct conn *c, const char* user, const char* pass) {
	const char *s;
	unsigned char hash[16];
	struct strbuf chlg = STRBUF_INIT;
	HMAC_CTX hmac;

	s = read_command(c);
	if (strcmp(s, "step")) goto error;
	if (read_string(c, &chlg)) goto error;

	read_command_end(c);

	HMAC_Init(&hmac, (unsigned char*) pass, strlen(pass), EVP_md5());
	HMAC_Update(&hmac, (unsigned char*) chlg.buf, chlg.len);
	HMAC_Final(&hmac, hash, NULL);
	HMAC_CTX_cleanup(&hmac);

	sendf(c, "%d:%s %s\n", (int) (strlen(user) + 1 + 32), user, md5_to_hex(hash));

	strbuf_release(&chlg);
	return;

error:
	die(_("auth failed"));
}

static void do_connect(struct conn *c, struct strbuf *uuid) {
	static char *host, *port;

	struct addrinfo hints, *res, *ai;
	int err;
	int fd = -1;
	int anon = 0, md5auth = 0;

	if (c->fd >= 0)
		return;

	memset(&hints, 0, sizeof(hints));
	hints.ai_socktype = SOCK_STREAM;

	if (!host) {
		host = xstrdup(svn_auth->host);
		port = strchr(host, ':');
		if (port) *(port++) = '\0';
	}

	err = getaddrinfo(host, port ? port : "3690", &hints, &res);

	if (err)
		die_errno("failed to connect to %s", url.buf);

	for (ai = res; ai != NULL; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0) continue;

		if (connect(fd, ai->ai_addr, ai->ai_addrlen)) {
			int err = errno;
			close(fd);
			errno = err;
			continue;
		}

		break;
	}

	if (fd < 0)
		die_errno("failed to connect to %s", url.buf);

	c->fd = fd;

	/* TODO: capabilities, mechs */
	sendf(c, "( 2 ( edit-pipeline svndiff1 ) %d:%s %d:%s ( ) )\n",
			(int) url.len, url.buf,
			(int) strlen(user_agent), user_agent);

	/* server hello: ( success ( minver maxver ) ) */
	if (strcmp(read_command(c), "success")) goto err;
	if (read_number(c) > 2 || read_number(c) < 2) goto err;
	read_command_end(c);

	/* server mechs: ( success ( ( mech... ) realm ) ) */
	if (strcmp(read_command(c), "success")) goto err;
	if (read_list(c)) goto err;
	for (;;) {
		const char *mech = read_word(c);
		if (!mech) {
			break;
		} else if (!strcmp(mech, "ANONYMOUS")) {
			anon = 1;
		} else if (!strcmp(mech, "CRAM-MD5")) {
			md5auth = 1;
		}
	}
	read_end(c);
	read_command_end(c);

	if (!svn_auth->username && anon) {
		/* argument is "anonymous" encoded in base64 */
		sendf(c, "( ANONYMOUS ( 16:YW5vbnltb3VzCg== ) )\n");

		if (!read_success(c))
			goto auth_success;
	}

	if (md5auth) {
		sendf(c, "( CRAM-MD5 ( ) )\n");
		credential_fill(svn_auth);
		cram_md5(c, svn_auth->username, svn_auth->password);

		if (!read_success(c))
			goto auth_success;

		if (c == &main_connection)
			credential_reject(svn_auth);
	}

	die("auth failure");

auth_success:
	if (c == &main_connection)
		credential_approve(svn_auth);

	/* repo-info: ( success ( uuid repos-url ) ) */
	if (strcmp(read_command(c), "success")) goto err;
	if (uuid) {
		if (read_string(c, uuid)) goto err;
		if (read_string(c, &url)) goto err;
	}
	read_command_end(c);

	/* reparent */
	sendf(c, "( reparent ( %d:%s ) )\n", (int) url.len, url.buf);
	if (read_success(c)) goto err;
	if (read_success(c)) goto err;

	return;
err:
	die("protocol error");
}

static int svn_get_latest(void) {
	struct conn *c = &main_connection;
	int64_t n;
	sendf(c, "( get-latest-rev ( ) )\n");

	if (read_success(c)) goto err;
	if (strcmp(read_command(c), "success")) goto err;
	n = read_number(c);
	if (n < 0 || n > INT_MAX) goto err;
	read_command_end(c);

	return (int) n;

err:
	die("latest-rev failed");
}

static int svn_isdir(const char *path, int rev) {
	struct conn *c = &main_connection;
	const char *s = NULL;

	sendf(c, "( check-path ( %d:%s ( %d ) ) )\n",
		(int) strlen(path),
		path,
		rev);

	if (read_success(c)) return 0;

	if (!strcmp(read_command(c), "success")) {
		s = read_word(c);
	}
	read_command_end(c);

	return s && !strcmp(s, "dir");
}

static void svn_list(const char *path, int rev, struct string_list *dirs) {
	struct conn *c = &main_connection;
	struct strbuf buf = STRBUF_INIT;

	sendf(c, "( get-dir ( %d:%s ( %d ) false true ( kind ) ) )\n",
		(int) strlen(path), path, rev);

	if (read_success(c)) return;

	if (!strcmp(read_command(c), "success")) {
		read_number(c);
		read_list(c); /* props */
		read_end(c);
		read_list(c); /* dirents */
		while (!read_list(c)) {
			const char *s;
			read_string(c, &buf);
			s = read_word(c);
			if (s && !strcmp(s, "dir")) {
				clean_svn_path(&buf);
				string_list_insert(dirs, buf.buf);
			}
			read_end(c);
		}
		read_end(c);
	}
	read_command_end(c);

	strbuf_release(&buf);
}

static int log_worker(struct conn *c) {
	struct strbuf name = STRBUF_INIT;
	struct strbuf author = STRBUF_INIT;
	struct strbuf time = STRBUF_INIT;
	struct strbuf msg = STRBUF_INIT;
	struct svn_log l;
	size_t plen;
	int ret;

	pthread_mutex_lock(&lock);
	ret = next_log(&l);
	pthread_mutex_unlock(&lock);

	if (ret) return ret;

	plen = strlen(l.path);
	do_connect(c, NULL);

	sendf(c, "( log ( ( %d:%s ) " /* (path...) */
		"( %d ) ( %d ) " /* start/end revno */
		"%s true " /* changed-paths strict-node */
		") )\n",
		(int) plen,
		l.path,
		l.end,
		l.start,
		l.get_copysrc ? "true" : "false"
	     );

	if (read_success(c)) goto err;

	/* svn log reply is of the form
	 * ( ( ( n:changed-path A|D|R|M ( n:copy-path copy-rev ) ) ... ) rev n:author n:date n:message )
	 * ....
	 * done
	 * ( success ( ) )
	 */

	for (;;) {
		/* start of log entry */
		if (read_list(c)) {
			if (read_done(c)) goto err;
			if (read_success(c)) goto err;
			break;
		}

		/* start changed path entries */
		if (read_list(c)) goto err;

		while (l.get_copysrc && !read_list(c)) {
			/* path A|D|R|M [copy-path copy-rev] */
			if (read_string(c, &name)) goto err;
			read_word(c);

			clean_svn_path(&name);

			if (name.len > plen && !memcmp(name.buf, l.path, plen)) {
				l.copy_modified = 1;

			} else if (name.len <= plen
				&& !memcmp(name.buf, l.path, name.len)
				&& have_optional(c))
			{
				struct strbuf copy = STRBUF_INIT;
				int64_t copyrev;

				/* copy-path, copy-rev */
				if (read_string(c, &copy)) goto err;
				copyrev = read_number(c);
				if (copyrev <= 0 || copyrev > INT_MAX) goto err;
				read_end(c);

				clean_svn_path(&copy);
				strbuf_addstr(&copy, l.path + name.len);

				l.copysrc = strbuf_detach(&copy, NULL);
				l.copyrev = (int) copyrev;
			}

			read_end(c);
		}

		/* end of changed path entries */
		read_end(c);

		if (!l.get_copysrc) {
			/* rev number */
			int64_t rev = read_number(c);
			if (rev <= 0) goto err;

			/* author */
			if (read_list(c)) goto err;
			read_string(c, &author);
			read_end(c);

			/* timestamp */
			if (read_list(c)) goto err;
			read_string(c, &time);
			read_end(c);

			/* log message */
			if (read_list(c)) goto err;
			read_string(c, &msg);
			read_end(c);

			pthread_mutex_lock(&lock);
			cmt_read(&l, rev, author.buf, time.buf, msg.buf);
			pthread_mutex_unlock(&lock);
		}

		read_end(c);
		read_newline(c);
	}

	pthread_mutex_lock(&lock);
	log_read(&l);
	pthread_mutex_unlock(&lock);

	strbuf_release(&name);
	strbuf_release(&author);
	strbuf_release(&time);
	strbuf_release(&msg);
	return 0;

err:
	die("malformed log");
}


static void init_connection(struct conn *c) {
	memset(c, 0, sizeof(*c));
	c->fd = -1;
	strbuf_init(&c->buf, 0);
	strbuf_init(&c->indbg, 0);
	strbuf_init(&c->word, 0);
}

typedef int (*worker_fn)(struct conn*);

#ifndef NO_PTHREADS
static void *run_worker(void *data) {
	worker_fn fn = data;
	struct conn c;
	init_connection(&c);

	while (!fn(&c)) {
	}

	strbuf_release(&c.word);
	strbuf_release(&c.indbg);
	strbuf_release(&c.buf);
	close(c.fd);

	return NULL;
}
#endif

static void spawn_workers(worker_fn fn) {
#ifndef NO_PTHREADS
	int i;
	pthread_t *threads = malloc((svn_max_requests - 1) * sizeof(threads[0]));
	pthread_mutex_init(&lock, NULL);
	for (i = 0; i < svn_max_requests-1; i++) {
		pthread_create(&threads[i], NULL, &run_worker, fn);
	}
#endif

	while (!fn(&main_connection)) {
	}

#ifndef NO_PTHREADS
	for (i = 0; i < svn_max_requests-1; i++) {
		pthread_join(threads[i], NULL);
	}
	pthread_mutex_destroy(&lock);
	free(threads);
#endif
}

static void svn_read_logs(void) {
	spawn_workers(&log_worker);
}

struct svn_proto proto_svn = {
	&svn_get_latest,
	&svn_list,
	&svn_isdir,
	&svn_read_logs,
};

struct svn_proto* svn_connect(struct strbuf *purl, struct credential *cred, struct strbuf *uuid) {
	struct conn *c = &main_connection;
	svn_auth = cred;
	strbuf_add(&url, purl->buf, purl->len);

	user_agent = git_user_agent();

	init_connection(&main_connection);
	do_connect(c, uuid);

	strbuf_reset(purl);
	strbuf_add(purl, url.buf, url.len);

	return &proto_svn;
}
