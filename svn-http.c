#include "remote-svn.h"
#include "cache.h"
#include "http.h"
#include "credential.h"
#include "object.h"
#include <expat.h>

/* url does not have a trailing slash, includes rootpath */
static struct strbuf url = STRBUF_INIT;
static int pathoff;

struct request {
	XML_Parser parser;
	struct active_request_slot *slot;
	struct slot_results res;
	struct buffer in;
	struct strbuf url;
	struct strbuf header;
	struct strbuf cdata;
	struct curl_slist *hdrs;
	const char *method;
	curl_write_callback hdrfunc;
	void *callback_data;
	void (*callback_func)(void *data);
	unsigned int started : 1;
	unsigned int just_opened : 1;
};

static struct request main_request;

static void append_path(struct strbuf *buf, const char *p) {
	while (*p) {
		if ('a' <= *p && *p <= 'z') {
			strbuf_addch(buf, *p);
		} else if ('A' <= *p && *p <= 'Z') {
			strbuf_addch(buf, *p);
		} else if ('0' <= *p && *p <= '9') {
			strbuf_addch(buf, *p);
		} else if (*p == '/' || *p == '-' || *p == '_' || *p == '.' || *p == '(' || *p == ')' || *p == '[' || *p == ']' || *p == ',') {
			strbuf_addch(buf, *p);
		} else {
			strbuf_addf(buf, "%%%02X", *(unsigned char*)p);
		}

		p++;
	}
}

static void encode_xml(struct strbuf *buf, const char *p) {
	while (*p) {
		switch (*p) {
		case '"':
			strbuf_addstr(buf, "&quot;");
			break;
		case '\'':
			strbuf_addstr(buf, "&apos;");
			break;
		case '<':
			strbuf_addstr(buf, "&lt;");
			break;
		case '>':
			strbuf_addstr(buf, "&gt;");
			break;
		case '&':
			strbuf_addstr(buf, "&amp;");
			break;
		default:
			strbuf_addch(buf, *p);
			break;
		}

		p++;
	}
}

static const char* shorten_tag(const char* name) {
	static struct strbuf buf = STRBUF_INIT;
	strbuf_reset(&buf);

	if (!prefixcmp(name, "svn:|")) {
		strbuf_addf(&buf, "S:%s", name + strlen("svn:|"));
	} else if (!prefixcmp(name, "DAV:|")) {
		strbuf_addf(&buf, "D:%s", name + strlen("DAV:|"));
	} else if (!prefixcmp(name, "http://subversion.tigris.org/xmlns/dav/|")) {
		strbuf_addf(&buf, "V:%s", name + strlen("http://subversion.tigris.org/xmlns/dav/|"));
	}

	return buf.len ? buf.buf : name;
}

static void xml_start(void *user, const char *name, const char **attrs) {
	struct request *h = user;

	strbuf_reset(&h->cdata);

	if (svndbg >= 2) {
		if (h->just_opened) {
			fprintf(stderr, ">\n");
		}
		fprintf(stderr, "%s<%s", h->header.buf, shorten_tag(name));
		while (attrs[0] && attrs[1]) {
			fprintf(stderr, " %s=\"%s\"", attrs[0], attrs[1]);
			attrs += 2;
		}
		strbuf_addch(&h->header, ' ');
		h->just_opened = 1;
	}
}

static void xml_end(void *user, const char *name) {
	struct request *h = user;
	strbuf_trim(&h->cdata);

	if (svndbg >= 2) {
		strbuf_setlen(&h->header, h->header.len - 1);

		if (h->just_opened && !h->cdata.len) {
			fprintf(stderr, "/>\n");
		} else if (h->just_opened) {
			fprintf(stderr, ">");
		} else {
			fprintf(stderr, "%s", h->header.buf);
		}

		if (h->cdata.len > INT_MAX) {
			fprintf(stderr, "%.*s...</%s>\n", 64, h->cdata.buf, shorten_tag(name));
		} else if (h->cdata.len || !h->just_opened) {
			fprintf(stderr, "%s</%s>\n", h->cdata.buf, shorten_tag(name));
		}

		h->just_opened = 0;
	}
}

static size_t write_xml(char *ptr, size_t eltsize, size_t sz, void *report_) {
	struct request *h = report_;
	sz *= eltsize;
	if (h && !XML_Parse(h->parser, ptr, sz, 0)) {
		die("xml parse error");
	}
	return sz;
}

static void xml_cdata_cb(void *user, const XML_Char *s, int len) {
	struct request *h = user;
	strbuf_add(&h->cdata, s, len);
}

static void process_request(struct request *h, XML_StartElementHandler start, XML_EndElementHandler end) {
	strbuf_release(&h->cdata);
	strbuf_reset(&h->header);
	XML_SetCharacterDataHandler(h->parser, &xml_cdata_cb);
	XML_SetElementHandler(h->parser, start, end);
	XML_SetUserData(h->parser, h);
}

static void request_finished(void *user);

static void init_request(struct request *h) {
	memset(h, 0, sizeof(*h));
	h->parser = XML_ParserCreateNS("UTF-8", '|');
	strbuf_init(&h->in.buf, 0);
	strbuf_init(&h->header, 0);
	strbuf_init(&h->cdata, 0);
	strbuf_init(&h->url, 0);
}

static void reset_request(struct request *h) {
	strbuf_reset(&h->url);
	strbuf_add(&h->url, url.buf, url.len);
	strbuf_reset(&h->in.buf);
	h->hdrs = NULL;
	h->hdrfunc = NULL;
	h->method = NULL;
	XML_ParserReset(h->parser, "UTF-8");
	h->started = 0;
	h->callback_func = &request_finished;
	h->callback_data = h;
}

static void start_request(struct request *h) {
	static struct curl_slist *defhdrs;
	CURL *c;

	if (!defhdrs) {
		defhdrs = curl_slist_append(defhdrs, "Expect:");
		defhdrs = curl_slist_append(defhdrs, "DAV: http://subversion.tigris.org/xmlns/dav/svn/depth");
	}

	h->in.posn = 0;
	h->just_opened = 0;
	h->slot = get_active_slot();
	h->slot->results = &h->res;
	h->slot->callback_func = h->callback_func;
	h->slot->callback_data = h->callback_data;

	c = h->slot->curl;
	curl_easy_setopt(c, CURLOPT_PUT, 1);
	curl_easy_setopt(c, CURLOPT_NOBODY, 0);
	curl_easy_setopt(c, CURLOPT_UPLOAD, 1);
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, h->hdrs ? h->hdrs : defhdrs);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_xml);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, XML_GetUserData(h->parser));
	curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, h->hdrfunc);
	curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, h->method);
	curl_easy_setopt(c, CURLOPT_URL, h->url.buf);
	curl_easy_setopt(c, CURLOPT_READFUNCTION, fread_buffer);
	curl_easy_setopt(c, CURLOPT_INFILE, &h->in);
	curl_easy_setopt(c, CURLOPT_INFILESIZE, h->in.buf.len);
	curl_easy_setopt(c, CURLOPT_IOCTLFUNCTION, ioctl_buffer);
	curl_easy_setopt(c, CURLOPT_IOCTLDATA, &h->in);
	curl_easy_setopt(c, CURLOPT_ENCODING, "svndiff1;q=0.9,svndiff;q=0.8");

	if (svndbg >= 2)
		fprintf(stderr, "start report %s\n%s\n", h->url.buf, h->in.buf.buf);

	if (!start_active_slot(h->slot)) {
		die("request-log failed %d\n", (int) h->res.http_code);
	}

	h->started = 1;
}

static void request_finished(void *user) {
	/* only cleanup the slot related data here, as this may be
	 * called before we get a chance to parse the data */
	struct request *h = user;

	if (h->res.curl_result && h->res.http_code == 401) {
		credential_fill(http_auth);
		start_request(h);
	} else {
		h->slot = NULL;
	}
}

static int run_request(struct request *h) {
	if (!h->started) {
		start_request(h);
	}

	while (h->slot) {
		run_active_slot(h->slot);
	}

	return h->res.curl_result;
}

static int get_header(struct strbuf* buf, const char* hdr, char* ptr, size_t size) {
	size_t hsz = strlen(hdr);
	if (size < hsz || memcmp(ptr, hdr, hsz))
		return 0;

	strbuf_reset(buf);
	strbuf_add(buf, ptr + hsz, size - hsz);
	strbuf_trim(buf);
	return 1;
}

static int latest_rev = -1;
static struct strbuf *uuid;

static size_t options_header(char *ptr, size_t size, size_t nmemb, void *userdata) {
	struct strbuf buf = STRBUF_INIT;

	size *= nmemb;

	if (get_header(&buf, "SVN-Youngest-Rev: ", ptr, size)) {
		latest_rev = atoi(buf.buf);

	} else if (get_header(&buf, "SVN-Repository-Root: ", ptr, size)) {
		clean_svn_path(&buf);
		strbuf_setlen(&url, pathoff + buf.len);

	} else if (uuid && get_header(uuid, "SVN-Repository-UUID: ", ptr, size)) {
	}

	strbuf_release(&buf);
	return size;
}

static struct strbuf latest_href = STRBUF_INIT;

static void latest_xml_start(void *user, const char *name, const char **attrs) {
	xml_start(user, name, attrs);

	if (!strcmp(name, "DAV:|href")) {
		strbuf_reset(&latest_href);
	}
}

static void latest_xml_end(void *user, const char *name) {
	struct request *h = user;
	xml_end(h, name);

	if (!strcmp(name, "DAV:|href")) {
		strbuf_swap(&latest_href, &h->cdata);

	} else if (!strcmp(name, "DAV:|version-name")) {
		latest_rev = atoi(h->cdata.buf);

	} else if (!strcmp(name, "http://subversion.tigris.org/xmlns/dav/|baseline-relative-path")) {
		clean_svn_path(&h->cdata);
		strbuf_setlen(&url, url.len - h->cdata.len);

	} else if (uuid && !strcmp(name, "http://subversion.tigris.org/xmlns/dav/|repository-uuid")) {
		strbuf_swap(uuid, &h->cdata);

	}

	strbuf_reset(&h->cdata);
}

static int http_get_latest(void) {
	return latest_rev;
}

static void http_get_options(void) {
	struct request *h = &main_request;

	reset_request(h);
	h->hdrfunc = &options_header;
	h->method = "OPTIONS";

	strbuf_addstr(&h->in.buf,
		"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
		"<D:options xmlns:D=\"DAV:\">\n"
		" <D:activity-collection-set/>\n"
		"</D:options>\n");

	if (run_request(h))
		goto err;

	if (h->res.curl_result && (h->res.http_code == 401 || h->res.http_code == 403)) {
		credential_reject(http_auth);
		die("auth failed");
	}

	credential_approve(http_auth);

	/* Pre 1.7 doesn't send the uuid/version-number as OPTION headers */
	if (latest_rev < 0) {
		reset_request(h);
		h->method = "PROPFIND";
		h->hdrs = curl_slist_append(NULL, "Expect:");
		h->hdrs = curl_slist_append(h->hdrs, "Depth: 0");

		strbuf_addstr(&h->in.buf,
			"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
			"<D:propfind xmlns:D=\"DAV:\" xmlns:S=\"http://subversion.tigris.org/xmlns/dav/\">\n"
			" <D:prop>\n"
			"  <S:baseline-relative-path/>\n"
			"  <S:repository-uuid/>\n"
			"  <D:version-name/>\n"
			" </D:prop>\n"
			"</D:propfind>\n");

		process_request(h, &latest_xml_start, &latest_xml_end);
		if (run_request(h))
			goto err;

		curl_slist_free_all(h->hdrs);
	}

	return;

err:
	die("get_latest failed %d %d", (int) h->res.curl_result, (int) h->res.http_code);
}

static int list_collection, list_off;
static struct string_list *list_dirs;
static char *list_href;

static void list_xml_end(void *user, const char *name) {
	struct request *h = user;

	xml_end(h, name);

	if (!strcmp(name, "DAV:|collection")) {
		list_collection = 1;

	} else if (!strcmp(name, "DAV:|href") && h->cdata.len >= list_off) {
		strbuf_remove(&h->cdata, 0, list_off);
		clean_svn_path(&h->cdata);
		free(list_href);
		list_href = url_decode_mem(h->cdata.buf, h->cdata.len);
		if (!list_href)
			list_href = strdup("");

	} else if (!strcmp(name, "DAV:|response")) {
		if (list_collection && list_href) {
			string_list_insert(list_dirs, list_href);
		}

		free(list_href);
		list_href = NULL;
		list_collection = 0;
	}

	strbuf_reset(&h->cdata);
}

static void do_list(const char *path, int rev, struct string_list *dirs, struct curl_slist *hdrs) {
	struct request *h = &main_request;

	reset_request(h);
	h->hdrs = hdrs;
	h->method = "PROPFIND";

	strbuf_addf(&h->url, "/!svn/bc/%d", rev);
	append_path(&h->url, path);

	strbuf_addstr(&h->in.buf,
		"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
		"<propfind xmlns=\"DAV:\">\n"
		" <prop><resourcetype/></prop>\n"
		"</propfind>\n");

	list_dirs = dirs;
	list_off = h->url.len - pathoff;
	list_collection = 0;

	process_request(h, &xml_start, &list_xml_end);

	free(list_href);
	list_href = NULL;

	if (run_request(h) && h->res.http_code != 404) {
		die("propfind failed %d %d", (int) h->res.curl_result, (int) h->res.http_code);
	}
}

static void http_list(const char *path, int rev, struct string_list *dirs) {
	static struct curl_slist *hdrs;
	if (!hdrs) {
		hdrs = curl_slist_append(hdrs, "Expect:");
		hdrs = curl_slist_append(hdrs, "Depth: 1");
	}
	do_list(path, rev, dirs, hdrs);
}

static int http_isdir(const char *path, int rev) {
	static struct curl_slist *hdrs;
	struct string_list dirs = STRING_LIST_INIT_NODUP;
	int ret;

	if (!hdrs) {
		hdrs = curl_slist_append(hdrs, "Expect:");
		hdrs = curl_slist_append(hdrs, "Depth: 0");
	}

	do_list(path, rev, &dirs, hdrs);

	ret = dirs.nr != 0;
	string_list_clear(&dirs, 0);
	return ret;
}







struct log {
	struct request req;
	struct strbuf msg, author, time, copy;
	int rev, copyrev;
	struct svn_log svn;
	struct log *next;
};

static void log_xml_start(void *user, const XML_Char *name, const XML_Char **attrs) {
	struct log *l = user;
	struct request *h = &l->req;

	xml_start(h, name, attrs);

	if (!strcmp(name, "svn:|log-item")) {
		strbuf_reset(&l->msg);
		strbuf_reset(&l->author);
		strbuf_reset(&l->time);
		l->rev = 0;
	}
}

static void log_xml_end(void *user, const XML_Char *name) {
	struct log *l = user;
	struct request *h = &l->req;

	xml_end(h, name);

	if (!strcmp(name, "svn:|log-item")) {
		cmt_read(&l->svn, l->rev, l->author.buf, l->time.buf, l->msg.buf);

	} else if (!strcmp(name, "DAV:|version-name")) {
		l->rev = atoi(h->cdata.buf);

	} else if (!strcmp(name, "DAV:|comment")) {
		strbuf_swap(&h->cdata, &l->msg);

	} else if (!strcmp(name, "DAV:|creator-displayname")) {
		strbuf_swap(&h->cdata, &l->author);

	} else if (!strcmp(name, "svn:|date")) {
		strbuf_swap(&h->cdata, &l->time);
	}

	strbuf_reset(&h->cdata);
}

static void copysrc_xml_start(void *user, const XML_Char *name, const XML_Char **attrs) {
	struct log *l = user;
	struct request *h = &l->req;
	const char **p;

	xml_start(h, name, attrs);

	l->copyrev = 0;
	strbuf_reset(&l->copy);

	p = attrs;
	while (p[0] && p[1]) {
		const char *key = *(p++);
		const char *val = *(p++);

		if (!strcmp(key, "copyfrom-path")) {
			strbuf_addstr(&l->copy, val);
			clean_svn_path(&l->copy);
		} else if (!strcmp(key, "copyfrom-rev")) {
			l->copyrev = atoi(val);
		}
	}
}

static void copysrc_xml_end(void *user, const XML_Char *name) {
	struct log *l = user;
	struct request *h = &l->req;
	struct svn_log *s = &l->svn;

	xml_end(h, name);

	if (!strcmp(name, "svn:|added-path")
	|| !strcmp(name, "svn:|replaced-path")
	|| !strcmp(name, "svn:|deleted-path")
	|| !strcmp(name, "svn:|modified-path"))
	{
		size_t plen = strlen(s->path);
		clean_svn_path(&h->cdata);

		if (h->cdata.len > plen && !memcmp(h->cdata.buf, s->path, plen)) {
			s->copy_modified = 1;

		} else if (h->cdata.len <= plen
			&& !memcmp(h->cdata.buf, s->path, h->cdata.len)
			&& l->copyrev)
		{
			strbuf_addstr(&l->copy, s->path + h->cdata.len);
			s->copysrc = strbuf_detach(&l->copy, NULL);
			s->copyrev = l->copyrev;
		}
	}

	strbuf_reset(&h->cdata);
}

static struct log *free_log;

static void log_finished(void *user) {
	struct log *l = user;
	struct request *h = &l->req;

	if (h->res.curl_result) {
		die("log failed %d %d", (int) h->res.curl_result, (int) h->res.http_code);
	}

	log_read(&l->svn);
	l->next = free_log;
	free_log = l;
}

static int fill_read_logs(void *user) {
	struct strbuf *b;
	struct request *h;
	struct log *l;
	struct svn_log *s;

	if (free_log) {
		l = free_log;
		free_log = l->next;
	} else {
		l = xcalloc(1, sizeof(*l));
		init_request(&l->req);
		strbuf_init(&l->msg, 0);
		strbuf_init(&l->author, 0);
		strbuf_init(&l->time, 0);
		strbuf_init(&l->copy, 0);
	}

	if (next_log(&l->svn)) {
		l->next = free_log;
		free_log = l;
		return 0;
	}

	s = &l->svn;
	h = &l->req;

	reset_request(h);
	h->method = "REPORT";

	strbuf_addf(&h->url, "/!svn/ver/%d", s->end);
	append_path(&h->url, s->path);

	b = &h->in.buf;
	strbuf_addstr(b, "<S:log-report xmlns:S=\"svn:\">\n");
	strbuf_addstr(b, " <S:strict-node-history/>\n");
	strbuf_addf(b, " <S:start-revision>%d</S:start-revision>\n", s->end);
	strbuf_addf(b, " <S:end-revision>%d</S:end-revision>\n", s->start);

	if (s->get_copysrc) {
		strbuf_addstr(b, " <S:discover-changed-paths/>\n");
		strbuf_addstr(b, " <S:no-revprops/>\n");
	} else {
		strbuf_addstr(b, " <S:revprop>svn:author</S:revprop>\n");
		strbuf_addstr(b, " <S:revprop>svn:date</S:revprop>\n");
		strbuf_addstr(b, " <S:revprop>svn:log</S:revprop>\n");
	}

	strbuf_addstr(b, " <S:path/>\n");
	strbuf_addstr(b, "</S:log-report>\n");

	if (s->get_copysrc) {
		process_request(h, &copysrc_xml_start, &copysrc_xml_end);
	} else {
		process_request(h, &log_xml_start, &log_xml_end);
	}

	h->callback_func = &log_finished;
	h->callback_data = l;

	start_request(h);
	return 1;
}

static void http_read_logs(void) {
	add_fill_function(NULL, &fill_read_logs);
	fill_active_slots();
	finish_all_active_slots();
	remove_fill_function(NULL, &fill_read_logs);
}






struct update {
	struct request req;
	struct strbuf path, diff, hash;
	struct svn_update svn;
	struct update *next;
};

static void add_name(struct strbuf *buf, const XML_Char **p) {
	while (p[0] && p[1]) {
		if (!strcmp(p[0], "name")) {
			strbuf_addch(buf, '/');
			strbuf_addstr(buf, p[1]);
			clean_svn_path(buf);
			return;
		}
		p += 2;
	}
}

static void update_xml_start(void *user, const XML_Char *name, const XML_Char **attrs) {
	struct update *u = user;
	struct request *h = &u->req;
	struct strbuf *b = &u->svn.head;

	xml_start(h, name, attrs);

	if (!strcmp(name, "svn:|open-directory")
			|| !strcmp(name, "svn:|add-file")
			|| !strcmp(name, "svn:|open-file"))
	{
		add_name(&u->path, attrs);

	} else if (!strcmp(name, "svn:|add-directory")) {
		add_name(&u->path, attrs);

		strbuf_addf(b, "add-dir");
		arg_quote(b, u->path.buf);
		strbuf_addch(b, '\n');

	} else if (!strcmp(name, "svn:|delete-entry")) {
		add_name(&u->path, attrs);

		strbuf_addf(b, "delete-entry");
		arg_quote(b, u->path.buf);
		strbuf_addch(b, '\n');
	}
}

static void update_xml_end(void *user, const XML_Char *name) {
	struct update *u = user;
	struct request *h = &u->req;
	struct strbuf *b = &u->svn.head;
	char *p;

	xml_end(h, name);

	if (!strcmp(name, "svn:|txdelta") && h->cdata.len) {
		decode_64(&h->cdata);
		strbuf_swap(&h->cdata, &u->diff);

	} else if (!strcmp(name, "http://subversion.tigris.org/xmlns/dav/|md5-checksum")) {
		strbuf_swap(&u->hash, &h->cdata);

	} else if (!strcmp(name, "svn:|add-directory")
		|| !strcmp(name, "svn:|open-directory")
		|| !strcmp(name, "svn:|delete-entry"))
	{
		p = strrchr(u->path.buf, '/');
		if (p) strbuf_setlen(&u->path, p - u->path.buf);

	} else if (!strcmp(name, "svn:|add-file") || !strcmp(name, "svn:|open-file")) {
		if (u->diff.len) {
			strbuf_addstr(b, name + strlen("svn:|"));
			arg_quote(b, u->path.buf);
			strbuf_addf(b, " %d \"\" %s\n", (int) u->diff.len, u->hash.buf);
			strbuf_add(b, u->diff.buf, u->diff.len);
		}

		strbuf_reset(&u->hash);
		strbuf_reset(&u->diff);

		p = strrchr(u->path.buf, '/');
		if (p) strbuf_setlen(&u->path, p - u->path.buf);
	}

	strbuf_reset(&h->cdata);
}

static struct update *free_update;

static void update_finished(void *user) {
	struct update *u = user;
	struct request *h = &u->req;

	if (h->res.curl_result) {
		die("update failed %d %d", (int) h->res.curl_result, (int) h->res.http_code);
	}

	update_read(&u->svn);
	u->next = free_update;
	free_update = u;
}

static int fill_read_updates(void *user) {
	struct svn_update *s;
	struct update *u;
	struct request *h;
	struct strbuf *b;

	if (free_update) {
		u = free_update;
		free_update = u->next;
		strbuf_reset(&u->svn.head);
		strbuf_reset(&u->svn.tail);
	} else {
		u = xcalloc(1, sizeof(*u));
		init_request(&u->req);
		strbuf_init(&u->path, 0);
		strbuf_init(&u->diff, 0);
		strbuf_init(&u->hash, 0);
		strbuf_init(&u->svn.head, 0);
		strbuf_init(&u->svn.tail, 0);
	}

	if (next_update(&u->svn)) {
		u->next = free_update;
		free_update = u;
		return 0;
	}

	s = &u->svn;
	h = &u->req;

	reset_request(h);
	h->method = "REPORT";

	b = &h->url;
	strbuf_addstr(b, "/!svn/vcc/default");

	b = &h->in.buf;
	strbuf_addstr(b, "<S:update-report send-all=\"true\" xmlns:S=\"svn:\">\n");

	strbuf_addstr(b, " <S:src-path>");
	strbuf_add(b, url.buf, url.len);
	encode_xml(b, s->copyrev ? s->copy : s->path);
	strbuf_addstr(b, "</S:src-path>\n");

	strbuf_addstr(b, " <S:dst-path>");
	strbuf_add(b, url.buf, url.len);
	encode_xml(b, s->path);
	strbuf_addstr(b, "</S:dst-path>\n");

	strbuf_addf(b, " <S:target-revision>%d</S:target-revision>\n", s->rev);
	strbuf_addstr(b, " <S:depth>unknown</S:depth>\n");
	strbuf_addstr(b, " <S:ignore-ancestry>yes</S:ignore-ancestry>\n");

	if (s->copyrev) {
		strbuf_addf(b, " <S:entry rev=\"%d\" depth=\"infinity\"/>\n", s->copyrev);
	} else if (s->new_branch) {
		strbuf_addf(b, " <S:entry rev=\"%d\" depth=\"infinity\" start-empty=\"true\"/>\n", s->rev);
	} else {
		strbuf_addf(b, " <S:entry rev=\"%d\" depth=\"infinity\"/>\n", s->rev - 1);
	}

	strbuf_addstr(b, "</S:update-report>\n");

	process_request(h, &update_xml_start, &update_xml_end);

	h->callback_func = &update_finished;
	h->callback_data = u;

	start_request(h);
	return 1;
}

static void http_read_updates(void) {
	add_fill_function(NULL, &fill_read_updates);
	fill_active_slots();
	finish_all_active_slots();
	remove_fill_function(NULL, &fill_read_updates);
}





struct svn_proto proto_http = {
	&http_get_latest,
	&http_list,
	&http_isdir,
	&http_read_logs,
	&http_read_updates,
};

struct svn_proto *svn_http_connect(struct remote *remote, struct strbuf *purl, struct credential *cred, struct strbuf *puuid) {
	char *p;
	http_init(remote, purl->buf, 0);

	http_auth = cred;
	strbuf_add(&url, purl->buf, purl->len);

	init_request(&main_request);

	p = strchr(url.buf + strlen(cred->protocol) + 3, '/');
	pathoff = p ? p - url.buf : url.len;

	uuid = puuid;
	http_get_options();

	p = strchr(url.buf + strlen(cred->protocol) + 3, '/');
	pathoff = p ? p - url.buf : url.len;

	strbuf_reset(purl);
	strbuf_add(purl, url.buf, url.len);

	return &proto_http;
}
