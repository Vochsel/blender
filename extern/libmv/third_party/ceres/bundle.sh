#!/bin/sh

if [ "x$1" = "x--i-really-know-what-im-doing" ] ; then
  echo Proceeding as requested by command line ...
else
  echo "*** Please run again with --i-really-know-what-im-doing ..."
  exit 1
fi

repo="https://ceres-solver.googlesource.com/ceres-solver"
branch="master"
#tag="1.4.0"
tag=""
tmp=`mktemp -d`
checkout="$tmp/ceres"

GIT="git --git-dir $tmp/ceres/.git --work-tree $checkout"

git clone $repo $checkout

if [ $branch != "master" ]; then
    $GIT checkout -t remotes/origin/$branch
else
  if [ "x$tag" != "x" ]; then
      $GIT checkout $tag
  fi
fi

$GIT log -n 50 > ChangeLog

for p in `cat ./patches/series`; do
  echo "Applying patch $p..."
  cat ./patches/$p | patch -d $tmp/ceres -p1
done

find include -type f -not -iwholename '*.svn*' -exec rm -rf {} \;
find internal -type f -not -iwholename '*.svn*' -exec rm -rf {} \;

cat "files.txt" | while read f; do
  mkdir -p `dirname $f`
  cp $tmp/ceres/$f $f
done

rm -rf $tmp

sources=`find ./include ./internal -type f -iname '*.cc' -or -iname '*.cpp' -or -iname '*.c' | sed -r 's/^\.\//\t/' | \
  grep -v -E 'schur_eliminator_[0-9]_[0-9d]_[0-9d].cc' | \
  grep -v -E 'partitioned_matrix_view_[0-9]_[0-9d]_[0-9d].cc' | sort -d`
generated_sources=`find ./include ./internal -type f -iname '*.cc' -or -iname '*.cpp' -or -iname '*.c' | sed -r 's/^\.\//#\t\t/' | \
  grep -E 'schur_eliminator_[0-9]_[0-9d]_[0-9d].cc|partitioned_matrix_view_[0-9]_[0-9d]_[0-9d].cc' | sort -d`
headers=`find ./include ./internal -type f -iname '*.h' | sed -r 's/^\.\//\t/' | sort -d`

src_dir=`find ./internal -type f -iname '*.cc' -exec dirname {} \; -or -iname '*.cpp' -exec dirname {} \; -or -iname '*.c' -exec dirname {} \; | sed -r 's/^\.\//\t/' | sort -d | uniq`
src=""
for x in $src_dir $src_third_dir; do
  t=""

  if test  `echo "$x" | grep -c glog ` -eq 1; then
    continue;
  fi

  if test  `echo "$x" | grep -c generated` -eq 1; then
    continue;
  fi

  if stat $x/*.cpp > /dev/null 2>&1; then
    t="src += env.Glob('`echo $x'/*.cpp'`')"
  fi

  if stat $x/*.c > /dev/null 2>&1; then
    if [ -z "$t" ]; then
      t="src += env.Glob('`echo $x'/*.c'`')"
    else
      t="$t + env.Glob('`echo $x'/*.c'`')"
    fi
  fi

  if stat $x/*.cc > /dev/null 2>&1; then
    if [ -z "$t" ]; then
      t="src += env.Glob('`echo $x'/*.cc'`')"
    else
      t="$t + env.Glob('`echo $x'/*.cc'`')"
    fi
  fi

  if [ -z "$src" ]; then
    src=$t
  else
    src=`echo "$src\n$t"`
  fi
done

cat > CMakeLists.txt << EOF
# ***** BEGIN GPL LICENSE BLOCK *****
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
#
# The Original Code is Copyright (C) 2012, Blender Foundation
# All rights reserved.
#
# Contributor(s): Blender Foundation,
#                 Sergey Sharybin
#
# ***** END GPL LICENSE BLOCK *****

# NOTE: This file is automatically generated by bundle.sh script
#       If you're doing changes in this file, please update template
#       in that script too

set(INC
	.
	include
	internal
	../gflags
	../../
)

set(INC_SYS
	../../../Eigen3
)

set(SRC
${sources}

${headers}
)

#if(FALSE)
#	list(APPEND SRC
${generated_sources}
#	)
#endif()

if(WIN32)
	list(APPEND INC
		../glog/src/windows
	)

	if(NOT MINGW)
		list(APPEND INC
			../msinttypes
		)
	endif()
else()
	list(APPEND INC
		../glog/src
	)
endif()

add_definitions(
	-DCERES_HAVE_PTHREAD
	-DCERES_NO_SUITESPARSE
	-DCERES_NO_CXSPARSE
	-DCERES_NO_LAPACK
	-DCERES_RESTRICT_SCHUR_SPECIALIZATION
	-DCERES_HAVE_RWLOCK
)

if(WITH_OPENMP)
	add_definitions(
		-DCERES_USE_OPENMP
	)
endif()

include(CheckIncludeFileCXX)
CHECK_INCLUDE_FILE_CXX(unordered_map HAVE_STD_UNORDERED_MAP_HEADER)
if(HAVE_STD_UNORDERED_MAP_HEADER)
	# Even so we've found unordered_map header file it doesn't
	# mean unordered_map and unordered_set will be declared in
	# std namespace.
	#
	# Namely, MSVC 2008 have unordered_map header which declares
	# unordered_map class in std::tr1 namespace. In order to support
	# this, we do extra check to see which exactly namespace is
	# to be used.

	include(CheckCXXSourceCompiles)
	CHECK_CXX_SOURCE_COMPILES("#include <unordered_map>
	                          int main() {
	                            std::unordered_map<int, int> map;
	                            return 0;
	                          }"
	                          HAVE_UNURDERED_MAP_IN_STD_NAMESPACE)
	if(HAVE_UNURDERED_MAP_IN_STD_NAMESPACE)
		add_definitions(-DCERES_STD_UNORDERED_MAP)
		message(STATUS "Found unordered_map/set in std namespace.")
	else()
		CHECK_CXX_SOURCE_COMPILES("#include <unordered_map>
		                          int main() {
		                            std::tr1::unordered_map<int, int> map;
		                            return 0;
		                          }"
		                          HAVE_UNURDERED_MAP_IN_TR1_NAMESPACE)
		if(HAVE_UNURDERED_MAP_IN_TR1_NAMESPACE)
			add_definitions(-DCERES_STD_UNORDERED_MAP_IN_TR1_NAMESPACE)
			message(STATUS "Found unordered_map/set in std::tr1 namespace.")
		else()
			message(STATUS "Found <unordered_map> but cannot find either std::unordered_map "
			        "or std::tr1::unordered_map.")
			message(STATUS "Replacing unordered_map/set with map/set (warning: slower!)")
			add_definitions(-DCERES_NO_UNORDERED_MAP)
		endif()
	endif()
else()
	CHECK_INCLUDE_FILE_CXX("tr1/unordered_map" UNORDERED_MAP_IN_TR1_NAMESPACE)
	if(UNORDERED_MAP_IN_TR1_NAMESPACE)
		add_definitions(-DCERES_TR1_UNORDERED_MAP)
		message(STATUS "Found unordered_map/set in std::tr1 namespace.")
	else()
		message(STATUS "Unable to find <unordered_map> or <tr1/unordered_map>. ")
		message(STATUS "Replacing unordered_map/set with map/set (warning: slower!)")
		add_definitions(-DCERES_NO_UNORDERED_MAP)
	endif()
endif()

blender_add_lib(extern_ceres "\${SRC}" "\${INC}" "\${INC_SYS}")
EOF

cat > SConscript << EOF
#!/usr/bin/python

# NOTE: This file is automatically generated by bundle.sh script
#       If you're doing changes in this file, please update template
#       in that script too

import sys
import os

Import('env')

src = []
defs = []

$src
src += env.Glob('internal/ceres/generated/schur_eliminator_d_d_d.cc')
src += env.Glob('internal/ceres/generated/partitioned_matrix_view_d_d_d.cc')
#src += env.Glob('internal/ceres/generated/*.cc')

defs.append('CERES_HAVE_PTHREAD')
defs.append('CERES_NO_SUITESPARSE')
defs.append('CERES_NO_CXSPARSE')
defs.append('CERES_NO_LAPACK')
defs.append('CERES_RESTRICT_SCHUR_SPECIALIZATION')
defs.append('CERES_HAVE_RWLOCK')

if env['WITH_BF_OPENMP']:
    defs.append('CERES_USE_OPENMP')

conf = Configure(env)
if conf.CheckCXXHeader("unordered_map"):
    # Even so we've found unordered_map header file it doesn't
    # mean unordered_map and unordered_set will be declared in
    # std namespace.
    #
    # Namely, MSVC 2008 have unordered_map header which declares
    # unordered_map class in std::tr1 namespace. In order to support
    # this, we do extra check to see which exactly namespace is
    # to be used.

    if conf.CheckType('std::unordered_map<int, int>', language = 'CXX', includes="#include <unordered_map>"):
        defs.append('CERES_STD_UNORDERED_MAP')
        print("-- Found unordered_map/set in std namespace.")
    elif conf.CheckType('std::tr1::unordered_map<int, int>', language = 'CXX', includes="#include <unordered_map>"):
        defs.append('CERES_STD_UNORDERED_MAP_IN_TR1_NAMESPACE')
        print("-- Found unordered_map/set in std::tr1 namespace.")
    else:
        print("-- Found <unordered_map> but can not find neither std::unordered_map nor std::tr1::unordered_map.")
        print("-- Replacing unordered_map/set with map/set (warning: slower!)")
        defs.append('CERES_NO_UNORDERED_MAP')
elif conf.CheckCXXHeader("tr1/unordered_map"):
    defs.append('CERES_TR1_UNORDERED_MAP')
    print("-- Found unordered_map/set in std::tr1 namespace.")
else:
    print("-- Unable to find <unordered_map> or <tr1/unordered_map>. ")
    print("-- Replacing unordered_map/set with map/set (warning: slower!)")
    defs.append('CERES_NO_UNORDERED_MAP')

env = conf.Finish()

incs = '. ../../ ../../../Eigen3 ./include ./internal ../gflags'

# work around broken hashtable in 10.5 SDK
if env['OURPLATFORM'] == 'darwin' and env['WITH_BF_BOOST']:
    incs += ' ' + env['BF_BOOST_INC']
    defs.append('CERES_HASH_BOOST')

if env['OURPLATFORM'] in ('win32-vc', 'win32-mingw', 'linuxcross', 'win64-vc', 'win64-mingw'):
    if env['OURPLATFORM'] in ('win32-vc', 'win64-vc'):
        incs += ' ../msinttypes'

    incs += ' ../glog/src/windows'
else:
    incs += ' ../glog/src'

env.BlenderLib ( libname = 'extern_ceres', sources=src, includes=Split(incs), defines=defs, libtype=['extern', 'player'], priority=[20,137])
EOF
