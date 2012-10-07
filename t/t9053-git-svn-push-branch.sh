#!/bin/sh

test_description="git remote-svn push branch $svn_proto"
. ./lib-git-remote-svn.sh

function check_branched() {
	copyfrom_path="$1"
	copyfrom_rev="$2"
	echo check_branched $1 $2
	svn_cmd log --stop-on-copy -v --xml | grep copyfrom-path=\"/$copyfrom_path\" &&
	svn_cmd log --stop-on-copy -v --xml | grep copyfrom-rev=\"$copyfrom_rev\"
}

test_expect_success 'setup branches' '
	echo "#!/bin/sh" > askpass &&
	echo "echo pass" >> askpass &&
	chmod +x askpass &&
	git config core.askpass "$PWD/askpass" &&
	git config "credential.$svnurl.username" committer &&
	git config --add remote.svn.map Trunk:refs/heads/trunk &&
	git config --add remote.svn.map Branches/*:refs/heads/* &&
	git config --add remote.svn.map Tags/*:refs/tags/* &&
	git config svn.emptymsg emptymsg &&
	cd svnco &&
	svn_cmd mkdir Branches &&
	svn_cmd mkdir Tags &&
	svn_cmd ci -m "svn init" &&
	cd ..
'

test_expect_success 'init trunk' '
	git checkout -b trunk &&
	echo "foo" > file.txt &&
	git add file.txt &&
	git commit -a -m "init trunk" &&
	git push -v svn trunk &&
	git fetch -v svn &&
	test `show_ref svn/trunk` = `show_ref trunk` &&
	cd svnco &&
	svn_cmd up &&
	test -d Trunk &&
	test_svn_subject "init trunk" &&
	test_svn_author committer &&
	test_file Trunk/file.txt "foo" &&
	git show-ref | grep Trunk.2 &&
	cd ..
'

test_expect_success 'modify file' '
	cd svnco/Trunk &&
	echo "bar23" > file.txt &&
	svn_cmd ci -m "svn edit" &&
	cd ../.. &&
	git fetch -v svn &&
	git reset --hard svn/trunk &&
	test_file file.txt "bar23" &&
	echo "foo" > file.txt &&
	git commit -a -m "git edit" &&
	git push -v svn trunk &&
	cd svnco &&
	svn_cmd up &&
	cd Trunk &&
	test_svn_subject "git edit" &&
	test_svn_subject "svn edit" PREV &&
	test_file file.txt "foo" &&
	cd ../..
'

test_expect_success 'modify file2' '
	cd svnco/Trunk &&
	echo "bar" > svn.txt &&
	svn_cmd add svn.txt &&
	svn_cmd ci -m "svn edit" &&
	cd ../.. &&
	git fetch -v svn &&
	git reset --hard svn/trunk &&
	test_file svn.txt "bar" &&
	echo "foo" > svn.txt &&
	git commit -a -m "git edit" &&
	git push -v svn trunk &&
	cd svnco &&
	svn_cmd up &&
	cd Trunk &&
	test_svn_subject "git edit" &&
	test_svn_subject "svn edit" PREV &&
	test_file svn.txt "foo" &&
	cd ../..
'

init_trunk_rev=`latest_revision`
init_trunk_path=Trunk

test_expect_success 'create standalone branch' '
	git symbolic-ref HEAD refs/heads/standalone &&
	git rm -r --cached . &&
	echo "bar" > file.txt &&
	git add file.txt &&
	rm svn.txt &&
	git commit -a -m "init standalone" &&
	git push -v svn standalone &&
	cd svnco &&
	svn_cmd up &&
	cd Branches/standalone &&
	test_file file.txt "bar" &&
	test_svn_subject "init standalone" &&
	test_must_fail svn_cmd log -r PREV &&
	cd ../../..
'

test_expect_success 'create branch' '
	git checkout -b CreateBranch trunk &&
	git push -v svn CreateBranch &&
	cd svnco &&
	svn_cmd up &&
	cd Branches/CreateBranch &&
	test_file file.txt "foo" &&
	test_svn_subject "emptymsg" &&
	check_branched $init_trunk_path $init_trunk_rev &&
	cd ../../..
'

init_trunk_rev=`latest_revision`
init_trunk_path=Branches/CreateBranch

test_expect_success 'create and edit branch' '
	git checkout -b CreateEditBranch trunk &&
	echo "foo2" > file2.txt &&
	git add file2.txt &&
	git commit -a -m "create/edit branch" &&
	git push -v svn CreateEditBranch &&
	cd svnco &&
	svn_cmd up &&
	cd Branches/CreateEditBranch &&
	test_file file.txt "foo" &&
	test_file file2.txt "foo2" &&
	test_svn_subject "create/edit branch" &&
	check_branched $init_trunk_path $init_trunk_rev &&
	cd ../../..
'

test_expect_success 'create tag' '
	git checkout trunk &&
	git tag SimpleTag &&
	git push -v svn SimpleTag &&
	cd svnco &&
	svn_cmd up &&
	cd Tags/SimpleTag &&
	test_file file.txt "foo" &&
	test_svn_subject "emptymsg" &&
	check_branched $init_trunk_path $init_trunk_rev &&
	cd ../../..
'

init_trunk_rev=`latest_revision`
init_trunk_path=Tags/SimpleTag

test_expect_success 'create annotated tag' '
	git checkout trunk &&
	git tag -m "annotate tag" AnnotatedTag &&
	git push -v svn AnnotatedTag &&
	sha1=`show_tag AnnotatedTag` &&
	git fetch -v -t svn &&
	test `show_tag AnnotatedTag` = $sha1 &&
	cd svnco &&
	svn_cmd up &&
	cd Tags/AnnotatedTag &&
	test_file file.txt "foo" &&
	test_svn_subject "annotate tag" &&
	check_branched $init_trunk_path $init_trunk_rev &&
	cd ../../..
'

init_trunk_rev=`latest_revision`
init_trunk_path=Tags/AnnotatedTag

test_expect_success 'replace branch' '
	git checkout -b ReplaceBranch trunk &&
	echo "before replace" > file2.txt &&
	git add file2.txt &&
	git commit -a -m "before replace" &&
	git push -v svn ReplaceBranch &&
	cd svnco &&
	svn_cmd up &&
	cd Branches/ReplaceBranch &&
	test_file file2.txt "before replace" &&
	test_svn_subject "before replace" &&
	check_branched $init_trunk_path $init_trunk_rev &&
	cd ../../.. &&
	git checkout trunk &&
	git branch -D ReplaceBranch &&
	git checkout -b ReplaceBranch trunk &&
	echo "after replace" > file3.txt &&
	git add file3.txt &&
	git commit -a -m "after replace" &&
	test_must_fail git push -v svn ReplaceBranch &&
	git push -v -f svn ReplaceBranch &&
	cd svnco/Branches/ReplaceBranch &&
	svn_cmd up &&
	test ! -e file2.txt &&
	test_file file3.txt "after replace" &&
	test_svn_subject "after replace" &&
	check_branched $init_trunk_path $init_trunk_rev &&
	cd ../../..
'

test_expect_success 'replace tag' '
	git checkout -b temp trunk &&
	echo "foo" > file3.txt &&
	git add file3.txt &&
	git commit -a -m "before replace tag" &&
	old_sha1=`show_ref HEAD` &&
	git tag ReplaceTag &&
	git push -v svn ReplaceTag &&
	cd svnco &&
	svn_cmd up &&
	cd Tags/ReplaceTag &&
	test_svn_subject "before replace tag" &&
	check_branched $init_trunk_path $init_trunk_rev &&
	cd ../../.. &&
	git reset --hard trunk &&
	echo "bar" > file2.txt &&
	git add file2.txt &&
	git commit -a -m "after replace tag" &&
	git tag -f ReplaceTag &&
	test_must_fail git push -v svn ReplaceTag &&
	git push -v -f svn ReplaceTag &&
	cd svnco &&
	svn_cmd up &&
	cd Tags/ReplaceTag &&
	test_svn_subject "after replace tag" &&
	check_branched $init_trunk_path $init_trunk_rev &&
	cd ../../.. &&
	echo "bar2" > file2.txt &&
	git add file2.txt &&
	git commit -a -m "dummy commit" &&
	git tag -f -m "create replace tag" ReplaceTag &&
	test_must_fail git push -v svn ReplaceTag &&
	git push -v -f svn ReplaceTag &&
	cd svnco &&
	svn_cmd up &&
	cd Tags/ReplaceTag &&
	test_svn_subject "create replace tag" &&
	test_svn_subject "after replace tag" PREV &&
	cd ../../.. &&
	git checkout trunk &&
	git branch -D temp
'

test_expect_success 'delete branch' '
	git checkout -b DeleteBranch trunk &&
	git push -v svn DeleteBranch &&
	cd svnco &&
	svn_cmd up &&
	cd Branches/DeleteBranch &&
	test_svn_subject "emptymsg" &&
	cd ../../.. &&
	git checkout trunk &&
	git push -v svn :DeleteBranch &&
	git branch -D DeleteBranch &&
	cd svnco &&
	svn_cmd up &&
	test ! -e Branches/DeleteBranch &&
	cd ..
'

test_expect_success 'delete tag' '
	git checkout trunk &&
	git tag DeleteTag &&
	git push -v svn DeleteTag &&
	cd svnco &&
	svn_cmd up &&
	cd Tags/DeleteTag &&
	test_svn_subject "emptymsg" &&
	cd ../../.. &&
	git push -v svn :DeleteTag &&
	git tag -d DeleteTag &&
	cd svnco &&
	svn_cmd up &&
	test ! -e Tags/DeleteTag &&
	cd ..
'

# A - B
# |    \
# trunk MCBranch1
#
# A - B - C - MCBranch2
# |        \
# trunk    MCBranch1
#
# Check that C is pushed to MCBranch1 and that MCBranch2 uses that
# commit by being copied from MCBranch1

test_expect_success 'modify and create branch' '
	git checkout -b MCBranch1 trunk &&
	git push -v svn MCBranch1 &&
	cd svnco &&
	svn_cmd up &&
	cd .. &&
	init_trunk_rev=`latest_revision`
	init_trunk_path=Branches/MCBranch1 &&
	echo "bar" > file2.txt &&
	git add file2.txt &&
	git commit -a -m "some modification on MCBranch1" &&
	git branch MCBranch2 &&
	git push -v svn MCBranch2 MCBranch1 &&
	rm -f cmds.txt &&
	latest_mcbranch1=`latest_revision Branches/MCBranch1` &&
	cd svnco &&
	svn_cmd up &&
	cd Branches/MCBranch1 &&
	test_svn_subject "some modification on MCBranch1" &&
	cd ../MCBranch2 &&
	test_svn_subject "emptymsg" &&
	check_branched Branches/MCBranch1 $latest_mcbranch1 &&
	cd ../../..
'
# MRBranch2
# |
# A - B
# |    \
# trunk MRBranch1
#
# trunk
# |
# A - B
#   \
#     C - MRBranch2
#       \
#         MRBranch1
#
# Check that C is pushed to MRBranch2 and that MRBranch1 gets replaced
# with a copy from MRBranch2 that includes that commit.

test_expect_success 'modify and replace branch' '
	git checkout -b MRBranch2 trunk &&
	git checkout -b MRBranch1 trunk &&
	echo "change" > file2.txt &&
	git add file2.txt &&
	git commit -a -m "B" &&
	git push -v svn MRBranch1 &&
	git push -v svn MRBranch2 &&
	cd svnco &&
	svn_cmd up &&
	cd Branches/MRBranch1 &&
	test_svn_subject "B" &&
	cd ../MRBranch2 &&
	test_svn_subject "emptymsg" &&
	cd ../../.. &&
	before_rev=`latest_revision` &&
	git checkout MRBranch2 &&
	echo "bar" > file2.txt &&
	git add file2.txt &&
	git commit -a -m "C" &&
	git branch -D MRBranch1 &&
	git branch MRBranch1 &&
	test_must_fail git push -v svn MRBranch1 MRBranch2 &&
	git push -v -f svn MRBranch1 MRBranch2 &&
	cd svnco &&
	svn_cmd up &&
	cd Branches/MRBranch1 &&
	test_svn_subject "emptymsg" &&
	check_branched Branches/MRBranch2 $(($before_rev+1)) &&
	cd ../MRBranch2 &&
	test_svn_subject "C" &&
	test_svn_subject "emptymsg" PREV &&
	check_branched $init_trunk_path $init_trunk_rev &&
	cd ../../..
'

test_expect_success 'tag deleted branch' '
	git checkout -b DeleteBranch trunk &&
	echo "foo" > file2.txt &&
	git add file2.txt &&
	git commit -a -m "commit on deleted branch" &&
	git push -v svn DeleteBranch &&
	git tag TagOnDeleteBranch &&
	git checkout trunk &&
	git push -v svn :DeleteBranch &&
	git push -v svn TagOnDeleteBranch &&
	cd svnco &&
	svn_cmd up &&
	test ! -e Branches/DeleteBranch &&
	cd Tags/TagOnDeleteBranch &&
	test_svn_subject "emptymsg" &&
	test_must_fail svn_cmd log -r PREV --stop-on-copy &&
	cd ../../.. &&
	git branch -D DeleteBranch
'

# trunk
# |     \
# left   right
# |
# LeftMerged
#
# trunk
# |    \
# left  right
# |    /
# merge
# |
# LeftMerged
#
# Check that LeftMerged ends up as trunk, left, merge.

test_expect_success 'push left merge' '
	git checkout -b LeftLeft trunk &&
	echo "left" > left.txt &&
	git add left.txt &&
	git commit -m "left" &&
	git push -v svn HEAD:LeftMerged &&
	cd svnco &&
	svn_cmd up &&
	cd Branches/LeftMerged &&
	prev_path=`svn_cmd log --stop-on-copy -v --xml | grep copyfrom-path | sed -re "s#.*copyfrom-path=\"([^\"]*)\".*#copyfrom-path=\"\1\"#g"` &&
	prev_rev=`svn_cmd log --stop-on-copy -v --xml | grep copyfrom-rev | sed -re "s#.*copyfrom-rev=\"([^\"]*)\".*#copyfrom-rev=\"\1\"#g"` &&
	echo prev_path $prev_path &&
	echo prev_rev $prev_rev &&
	cd ../../.. &&
	git checkout -b LeftRight trunk &&
	echo "right" > right.txt &&
	git add right.txt &&
	git commit -m "right" &&
	git checkout LeftLeft &&
	git merge --no-ff "merge commit" HEAD LeftRight &&
	git push -v svn HEAD:LeftMerged &&
	cd svnco &&
	svn_cmd up &&
	cd Branches/LeftMerged &&
	test_file left.txt "left" &&
	test_file right.txt "right" &&
	test_svn_subject "merge commit" &&
	test_svn_subject "left" PREV &&
	svn_cmd log --stop-on-copy -v --xml | grep $prev_path &&
	svn_cmd log --stop-on-copy -v --xml | grep $prev_rev &&
	cd ../../..
'

# trunk
# |     \
# left   right
#        |
#        RightMerged
#
# trunk
# |    \
# left  right
# |    /
# merge
# |
# RightMerged
#
# Check that RightMerged ends up as trunk, right, merge.

test_expect_success 'push right merge' '
	git checkout -b RightRight trunk &&
	echo "right" > right.txt &&
	git add right.txt &&
	git commit -m "right" &&
	git push -v svn HEAD:RightMerged &&
	cd svnco &&
	svn_cmd up &&
	cd Branches/RightMerged &&
	prev_path=`svn_cmd log --stop-on-copy -v --xml | grep copyfrom-path | sed -re "s#.*copyfrom-path=\"([^\"]*)\".*#copyfrom-path=\"\1\"#g"` &&
	prev_rev=`svn_cmd log --stop-on-copy -v --xml | grep copyfrom-rev | sed -re "s#.*copyfrom-rev=\"([^\"]*)\".*#copyfrom-rev=\"\1\"#g"` &&
	echo prev_path $prev_path &&
	echo prev_rev $prev_rev &&
	cd ../../.. &&
	git checkout -b RightLeft trunk &&
	echo "left" > left.txt &&
	git add left.txt &&
	git commit -m "left" &&
	git checkout RightRight &&
	git merge --no-ff "merge commit" HEAD RightLeft &&
	git push -v svn HEAD:RightMerged &&
	cd svnco &&
	svn_cmd up &&
	cd Branches/RightMerged &&
	test_svn_subject "merge commit" &&
	test_svn_subject "right" PREV &&
	svn_cmd log --stop-on-copy -v --xml | grep $prev_path &&
	svn_cmd log --stop-on-copy -v --xml | grep $prev_rev &&
	cd ../../..
'

test_done
