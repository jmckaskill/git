#include "cache.h"
#include "builtin.h"
#include "svn.h"
#include "quote.h"
#include "refs.h"
#include "cache-tree.h"
#include <openssl/md5.h>

static struct index_state svn_index;
static int svn_eol = EOL_UNSET;

static void trypause(void) {
	static int env = -1;

	if (env == -1) {
		env = getenv("GIT_REMOTE_SVN_HELPER_PAUSE") != NULL;
	}

	if (env) {
		struct stat st;
		int fd = open("remote-svn-helper-pause", O_CREAT|O_RDWR, 0666);
		close(fd);
		while (!stat("remote-svn-helper-pause", &st)) {
			sleep(1);
		}
	}
}

static const char* cmt_to_hex(struct commit* c) {
	return sha1_to_hex(c ? c->object.sha1 : null_sha1);
}

static const unsigned char *idx_tree(struct index_state *idx) {
	if (!idx->cache_tree)
		idx->cache_tree = cache_tree();
	if (cache_tree_update(idx->cache_tree, idx->cache, idx->cache_nr, 0))
		die("failed to update cache tree");
	return idx->cache_tree->sha1;
}

static const unsigned char *cmt_tree(struct commit *c) {
	if (parse_commit(c))
		die("invalid commit %s", cmt_to_hex(c));
	return c->tree->object.sha1;
}

static int config(const char *key, const char *value, void *dummy) {
	if (!strcmp(key, "svn.eol")) {
		if (value && !strcasecmp(value, "lf"))
			svn_eol = EOL_LF;
		else if (value && !strcasecmp(value, "crlf"))
			svn_eol = EOL_CRLF;
		else if (value && !strcasecmp(value, "native"))
			svn_eol = EOL_NATIVE;
		else
			svn_eol = EOL_UNSET;
		return 0;
	}

	return git_default_config(key, value, dummy);
}

static void read_atom(struct strbuf* buf) {
	strbuf_reset(buf);
	while ((ch = getc()) != EOF) {
		int ch = getc();
		if (ch == EOF || (isspace(ch) && buf.len)) {
			break;
		} else if (!isspace(ch)) {
			strbuf_addch(&buf, ch);
		}
	}

	return buf.buf;
}

static int read_number(void) {
	int num = 0;

	for (;;) {
		int ch = getc();
		if (isspace(ch))
			continue;
		if (ch == ':')
			break;
		if (ch == EOF || ch < '0' || ch > '9')
			die("invalid argument");
		num = (num * 10) + (ch - '0');
	}

	return num;
}

static void read_string(struct strbuf *buf) {
	int len = read_number();
	strbuf_reset(buf);
	if (strbuf_fread(buf, len, stdin) != len)
		die_errno("read");
}

static void add_dir(const char *name) {
	struct strbuf buf = STRBUF_INIT;
	struct cache_entry* ce;
	char *p = strrchr(name, '/');
	unsigned char sha1[20];

	/* add ./.gitempty */
	if (write_sha1_file(NULL, 0, "blob", sha1))
		die("failed to write .gitempty object");
	strbuf_addstr(&buf, name);
	strbuf_addstr(&buf, "/.gitempty");
	ce = make_cache_entry(create_ce_mode(0644), sha1, buf.buf, 0, 0);
	add_index_entry(&the_index, ce, ADD_CACHE_OK_TO_ADD);

	/* remove ../.gitempty */
	if (p) {
		strbuf_reset(&buf);
		strbuf_add(&buf, name, p - name);
		strbuf_addstr(&buf, "/.gitempty");
		remove_file_from_index(&the_index, buf.buf);
	}

	strbuf_release(&buf);
}

static void checkmd5(const char *hash, const void *data, size_t sz) {
	unsigned char h1[16], h2[16];
	MD5_CTX ctx;

	if (get_md5_hex(hash, h1))
		die("invalid md5 hash %s", hash);

	MD5_Init(&ctx);
	MD5_Update(&ctx, data, sz);
	MD5_Final(h2, &ctx);

	if (memcmp(h1, h2, sizeof(h2)))
		die("hash mismatch");
}

static void change_file(
		int add, const char *name,
		struct strbuf *diff,
		const char *before, const char *after)
{
	struct strbuf buf = STRBUF_INIT;
	struct cache_entry* ce;
	char* p = strrchr(name, '/');
	void *src = NULL;
	unsigned long srcn = 0;
	unsigned char sha1[20];

	if (p) {
		/* remove ./.gitempty */
		strbuf_reset(&buf);
		strbuf_add(&buf, name, p - name);
		strbuf_addstr(&buf, "/.gitempty");
		remove_file_from_index(&the_index, buf.buf);
	}

	if (!add) {
		enum object_type type;
		ce = index_name_exists(&svn_index, name, strlen(name), 0);
		if (!ce) goto err;

		src = read_sha1_file(ce->sha1, &type, &srcn);
		if (!src || type != OBJ_BLOB) goto err;
	}

	if (*before)
		checkmd5(before, src, srcn);

	apply_svndiff(&buf, src, srcn, diff->buf, diff->len);

	if (*after)
		checkmd5(after, buf.buf, buf.len);

	if (write_sha1_file(buf.buf, buf.len, "blob", sha1))
		die_errno("write blob");

	ce = make_cache_entry(0644, sha1, name, 0, 0);
	if (!ce) die("make_cache_entry failed for path '%s'", name);
	add_index_entry(&svn_index, ce, ADD_CACHE_OK_TO_ADD);

	if (convert_to_git(name, buf.buf, buf.len, &buf, SAFE_CRLF_FALSE)) {
		if (write_sha1_file(buf.buf, buf.len, "blob", sha1)) {
			die_errno("write blob");
		}
	}

	ce = make_cache_entry(0644, sha1, name, 0, 0);
	if (!ce) die("make_cache_entry failed for path '%s'", name);
	add_index_entry(&the_index, ce, ADD_CACHE_OK_TO_ADD);

	strbuf_release(&buf);
	free(src);
	return;

err:
	die("malformed update");
}

static struct commit *checkout_git;
static struct mergeinfo *mergeinfo, *svn_mergeinfo;

int cmd_remote_svn__helper(int argc, const char **argv, const char *prefix) {
	struct strbuf cmd = STRBUF_INIT;
	struct strbuf ref = STRBUF_INIT;
	struct strbuf buf = STRBUF_INIT;
	struct strbuf copyref = STRBUF_INIT;
	struct strbuf path = STRBUF_INIT;
	struct strbuf ident = STRBUF_INIT;
	struct strbuf msg = STRBUF_INIT;
	struct strbuf before = STRBUF_INIT;
	struct strbuf after = STRBUF_INIT;
	struct strbuf diff = STRBUF_INIT;
	unsigned char sha1[20];

	trypause();

	git_config(&config, NULL);
	core_eol = svn_eol;

	for (;;) {
		read_atom(&cmd);

		if (!strcmp(cmd.buf, "")) {
			break;

		} else if (!strcmp(cmd.buf, "checkout")) {
			int rev;
			struct commit *svn;

			read_string(&ref);
			rev = read_number();

			svn = lookup_commit_reference_by_name(ref);

			while (svn && get_svn_revision(svn) > rev) {
				svn = svn_parent(svn);
			}

			checkout_git = svn_commit(svn);
			mergeinfo = get_mergeinfo(svn);
			svn_mergeinfo = get_svn_mergeinfo(svn);

			svn_checkout_index(&svn_index, svn);
			svn_checkout_index(&the_index, checkout_git);

		} else if (!strcmp(cmd.buf, "reset")) {
			svn_checkout_index(&svn_index, NULL);
			svn_checkout_index(&the_index, NULL);

			checkout_git = NULL;
			mergeinfo = parse_svn_mergeinfo("");
			svn_mergeinfo = parse_svn_mergeinfo("");

			/* add .gitattributes so the eol behaviour is
			 * maintained. This can be changed on the git side later
			 * if need be. */
			if (svn_eol != EOL_UNSET) {
				struct cache_entry *ce;
				static const char text[] = "* text=auto\n";

				if (write_sha1_file(text, strlen(text), "blob", sha1))
					return;

				ce = make_cache_entry(0644, sha1, ".gitattributes", 0, 0);
				if (!ce) return;

				add_index_entry(&the_index, ce, ADD_CACHE_OK_TO_ADD);
			}

		} else if (!strcmp(cmd.buf, "report")) {
			struct commit *git, *svn;

			read_string(&ref);

			svn = lookup_commit_reference_by_name(ref);
			git = svn_commit(svn);

			strbuf_reset(&buf);
			strbuf_addf(&buf, "%s.tag", ref);
			if (read_ref(buf.buf, sha1)) {
				hashcpy(sha1, git->object.sha1);
			}

			for (;;) {
				char *gref = next_arg(arg, &arg);
				if (!gref || !*gref) break;

				if (!prefixcmp(gref, "refs/tags/")) {
					printf("fetched %s %s\n", sha1_to_hex(sha1), gref);
				} else {
					printf("fetched %s %s\n", cmt_to_hex(git), gref);
				}
			}

		} else if (!strcmp(cmd.buf, "branch")) {
			int copyrev, rev;
			struct commit *svn;
			char *slash;

			read_string(&copyref);
			copyrev = read_number();
			read_string(&ref);
			rev = read_number();
			read_string(&path);
			read_string(&ident);
			read_string(&msg);
			strbuf_complete_line(&msg);

			svn = lookup_commit_reference_by_name(copyref);
			while (svn && get_svn_revision(svn) > copyrev) {
				svn = svn_parent(svn);
			}

			mergeinfo = get_mergeinfo(svn);
			svn_mergeinfo = get_svn_mergeinfo(svn);
			add_svn_mergeinfo(mergeinfo, path, trev, trev);

			if (write_svn_commit(NULL, svn_commit(svn), cmt_tree(svn), ident, path, trev, mergeinfo, svn_mergeinfo, sha1))
				die_errno("write svn commit");

			free_svn_mergeinfo(mergeinfo);
			free_svn_mergeinfo(svn_mergeinfo);
			mergeinfo = svn_mergeinfo = NULL;

			update_ref("remote-svn", ref, sha1, null_sha1, 0, DIE_ON_ERR);

			slash = strrchr(path.buf, '/');

			strbuf_reset(&buf);
			strbuf_addf(&buf,
				"object %s\n"
				"type commit\n"
				"tag %s\n"
				"tagger %s\n"
				"\n"
				"%s",
				cmt_to_hex(svn_commit(svn)),
				slash ? slash+1 : path.buf,
				ident.buf,
				msg.buf);

			if (!write_sha1_file(buf.buf, buf.len, "tag", sha1)) {
				strbuf_reset(&buf);
				strbuf_addf(&buf, "%s.tag", tref);
				update_ref("remote-svn", buf.buf, sha1, null_sha1, 0, DIE_ON_ERR);
			}

		} else if (!strcmp(cmd.buf, "commit")) {
			struct commit *git, *svn;
			int baserev, rev;

			read_string(&ref);
			baserev = read_number();
			rev = read_number();
			read_string(&path);
			read_string(&ident);
			read_string(&msg);
			strbuf_complete_line(&msg);

			svn = lookup_commit_reference_by_name(ref);
			git = checkout_git;

			if (get_svn_revision(svn) != baserev)
				die("unexpected intermediate commit");

			if (!git || hashcmp(idx_tree(&the_index), cmt_tree(git))) {
				strbuf_reset(&buf);
				strbuf_addf(&buf, "tree %s\n", sha1_to_hex(idx_tree(&the_index)));

				if (git)
					strbuf_addf(&buf, "parent %s\n", cmt_to_hex(git));

				strbuf_addf(&buf,
					"author %s\n"
					"committer %s\n"
					"\n"
					"%s",
					ident, ident, msg.buf);

				if (write_sha1_file(buf.buf, buf.len, "commit", sha1))
					die_errno("write git commit");

				git = lookup_commit(sha1);
			}

			add_svn_mergeinfo(mergeinfo, path, baserev+1, newrev);

			if (write_svn_commit(svn, git, idx_tree(&svn_index), ident, path, newrev, mergeinfo, svn_mergeinfo, sha1))
				die_errno("write svn commit");

			update_ref("remote-svn", ref, sha1, svn ? svn->object.sha1 : null_sha1, 0, DIE_ON_ERR);

			strbuf_reset(&buf);
			strbuf_addf(&buf, "%s.tag", ref);
			if (!read_ref(buf.buf, sha1)) {
				delete_ref(buf.buf, sha1, 0);
			}

			free_svn_mergeinfo(mergeinfo);
			free_svn_mergeinfo(svn_mergeinfo);
			mergeinfo = svn_mergeinfo = NULL;

		} else if (!strcmp(cmd.buf, "add-dir")) {
			read_string(&path);
			add_dir(path.buf);

		} else if (!strcmp(cmd.buf, "delete-entry")) {
			read_string(&path);
			remove_path_from_index(&svn_index, path.buf);
			remove_path_from_index(&the_index, path.buf);

		} else if (!strcmp(cmd.buf, "add-file") || !strcmp(cmd.buf, "open-file")) {
			read_string(&path);
			read_string(&before);
			read_string(&after);
			read_string(&diff);

			change_file(cmd.buf[0] == 'a', path.buf, &diff, before.buf, after.buf);

		} else if (!strcmp(cmd.buf, "set-mergeinfo")) {
			read_string(&buf);
			free_svn_mergeinfo(svn_mergeinfo);
			svn_mergeinfo = parse_svn_mergeinfo(buf.buf);
		}
	}

	return 0;
}

