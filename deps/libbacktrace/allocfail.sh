#!/bin/sh

# allocfail.sh -- Test for libbacktrace library.
# Copyright (C) 2018-2024 Free Software Foundation, Inc.

# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met:

#     (1) Redistributions of source code must retain the above copyright
#     notice, this list of conditions and the following disclaimer.

#     (2) Redistributions in binary form must reproduce the above copyright
#     notice, this list of conditions and the following disclaimer in
#     the documentation and/or other materials provided with the
#     distribution.

#     (3) The name of the author may not be used to
#     endorse or promote products derived from this software without
#     specific prior written permission.

# THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
# IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
# WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
# DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
# INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
# (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
# SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
# HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
# STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
# IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
# POSSIBILITY OF SUCH DAMAGE.

set -e

if [ ! -f ./allocfail ]; then
    # Hard failure.
    exit 99
fi

allocs=$(./allocfail 2>&1)
if [ "$allocs" = "" ]; then
    # Hard failure.
    exit 99
fi

# This generates the following output:
# ...
# $ allocfail.sh
# allocs: 80495
# Status changed to 0 at 1
# Status changed to 1 at 3
# Status changed to 0 at 11
# Status changed to 1 at 12
# Status changed to 0 at 845
# ...
#
# We have status 0 for an allocation failure at:
# - 1 because backtrace_create_state handles failure robustly
# - 2 because the fail switches backtrace_full to !can_alloc mode.
# - 11 because failure of elf_open_debugfile_by_buildid does not generate an
#   error callback beyond the one for the allocation failure itself.

echo "allocs: $allocs"

step=1
i=1
passes=0
prev_status=-1
while [ $i -le $allocs ]; do
    if ./allocfail $i >/dev/null 2>&1; status=$?; then
	true
    fi
    if [ $status -gt 1 ]; then
	echo "Unallowed fail found: $i"
	# Failure.
	exit 1
    fi

    # The test-case would run too long if we would excercise all allocs.
    # So, run with step 1 initially, and increase the step once we have 10
    # subsequent passes, and drop back to step 1 once we encounter another
    # failure.  This takes ~2.6 seconds on an i7-6600U CPU @ 2.60GHz.
    if [ $status -eq 0 ]; then
	passes=$(($passes + 1))
	if [ $passes -ge 10 ]; then
	    step=$((step * 10))
	    passes=0
	fi
    elif [ $status -eq 1 ]; then
	passes=0
	step=1
    fi

    if [ $status -ne $prev_status ]; then
	echo "Status changed to $status at $i"
    fi
    prev_status=$status

    i=$(($i + $step))
done

# Success.
exit 0
