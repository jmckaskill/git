#!/bin/sh

. ./test-lib.sh

export GIT_TRANSPORT_HELPER_DEBUG=1

if test -z "$SVNSERVE_PORT"
then
	skip_all='skipping remote-svn test. (set $SVNSERVE_PORT to enable)'
	test_done
fi

svnrepo=$PWD/svnrepo
svnconf=$PWD/svnconf
svnurl="svn://localhost:$SVNSERVE_PORT/svnrepo"
null_sha1=0000000000000000000000000000000000000000

# We should pass an empty configuration directory to the 'svn commit' to
# avoid automated property changes and other stuff that could be set
# from user's configuration files in ~/.subversion.
svn_cmd () {
	[ -d "$svnconf" ] || mkdir "$svnconf"
	cat > "$svnconf/servers" <<!
[global]
store-plaintext-passwords = yes
!
	orig_svncmd="$1"; shift
	if [ -z "$orig_svncmd" ]; then
		svn
		return
	fi
	echo svn $orig_svncmd $@
	svn "$orig_svncmd" --username committer --password pass --no-auth-cache --non-interactive --config-dir "$svnconf" "$@"
}

test_file () {
	file_contents="`cat $1`"
	test_contents="$2"
	test "$file_contents" = "$test_contents"
}

svn_date () {
	revision="$1"
	directory="$2"
	svn_cmd log -r "$revision" --xml -l 1 "$directory" | grep "<date>" | sed -re 's#^<date>([^\.Z]*)\.[0-9]+Z</date>#\1Z#g'
}

test_git_subject () {
	commit="$1"
	subject="$2"
	commit_subject="`git log -1 --pretty=format:%s $commit`"
	echo test_git_subject "$commit_subject" "$subject"
	test "$commit_subject" = "$subject"
}

test_git_author () {
	commit="$1"
	author="$2"
	commit_author="`git log -1 --pretty=format:'%an <%ae>' $commit`"
	echo test_git_author "$commit_author" "$author"
	test "$commit_author" = "$author"
}

test_git_date () {
	commit="$1"
	date="$2"
	commit_date="`git log -1 --pretty=format:%ai $commit | sed -re 's#^([^ ]*) ([^ ]*) \+0000$#\1T\2Z#g'`"
	echo test_git_date "$commit_date" "$date"
	test "$commit_date" = "$date"
}

show_ref () {
	(git show-ref -s --head $1 | head -n 1) || echo $1
}

show_tag () {
	show_ref refs/tags/$1 | git cat-file --batch | grep object | cut -f 2 -d ' '
}

merge_base () {
	git merge-base `show_ref $1` `show_ref $2`
}

latest_revision () {
	svn_cmd info "$svnurl/$1" | grep "Last Changed Rev" | cut -f 2 -d ':' | tr -d ' '
}

delete_remote_refs () {
	for ref in `git show-ref | grep refs/remotes/svn | cut -f 2 -d ' '`; do
		git update-ref -d $ref
	done
	for ref in `git show-ref | grep refs/tags/ | cut -f 2 -d ' '`; do
		git update-ref -d $ref
	done
}

test_expect_success 'start svnserve' '
	kill `cat ../svnserve.pid` &> /dev/null
	mkdir "$svnrepo" &&
	svnadmin create "$svnrepo" &&
	uuid=`cat "$svnrepo/db/uuid"` &&
	cat > "$svnrepo/conf/svnserve.conf" <<!
[general]
auth-access = write
anon-access = read
password-db = passwd
!
	cat > "$svnrepo/conf/passwd" <<!
[users]
committer = pass
!
	cat > .git/svn-authors <<!
 #committer = comment <foo@example.com>
committer = C O Mitter <committer@example.com> # some comment
!
	svnserve --daemon \
		--listen-port $SVNSERVE_PORT \
		--root "$svnrepo/.." \
		--listen-host localhost \
		--pid-file=../svnserve.pid \
		--log-file=log &&
	git remote add svn svn::$svnurl &&
	git config user.name "C O Mitter" &&
	git config user.email "committer@example.com" &&
	git config svn.authors .git/svn-authors &&
	svn_cmd co "file://$PWD/svnrepo" svnco
'
