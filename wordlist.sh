#!/bin/bash
#
# This script (re)generates the built-in common password list for passwdqc
#
# Copyright (c) 2025 by Solar Designer
# See LICENSE

set -e
trap 'echo "Terminated early" >&2' EXIT PIPE TERM

COUNT=10000
#IN1="git -C ../john show b1cb78e1db0243f9dd4c9295c570f482eaf7236f:run/password.lst"
IN2="cat ../john/run/password.lst"
IN1=$IN2
LIST=wordlist.lst
TRY1=$LIST.try1
TRY2=$LIST.try2
PROG=wordlist
C=$PROG.c
H=$PROG.h

# Include some common passwords first, even if many of them are unneeded for our test case below
$IN1 | grep -v '^#!comment:' | grep -E '^.{7,}' | tr A-Z a-z |
	grep -Ev '^(jan|febr)uary|(septem|octo|novem|decem)ber$' |
	awk '!seen[$0]++' | head -n $COUNT > $LIST
set -o pipefail # "head" above exits early and breaks the pipe
test "`wc -l $LIST | cut -f1 -d' '`" -eq $COUNT

# Pass 1: List all passwords that passed check
$IN2 | grep -v '^#!comment:' | LD_LIBRARY_PATH=. ./pwqcheck --multi -1 wordlist=$LIST | sed -n 's/^OK: // p' > $TRY1

# Pass 2: Add them to wordlist one by one as needed (previous ones may have eliminated the need)
while IFS= read -r LINE; do
	echo "$LINE" | LD_LIBRARY_PATH=. ./pwqcheck -1 wordlist=$LIST > /dev/null && echo "$LINE" >> $LIST
done < $TRY1

# Pass 3: Try removing them one by one (later ones may have eliminated the need)
while IFS= read -r LINE; do
	grep -Fvxe "$LINE" $LIST > $TRY2
	echo "$LINE" | LD_LIBRARY_PATH=. ./pwqcheck -1 wordlist=$TRY2 > /dev/null && continue
	set +o pipefail # "grep -q" may exit early and thus break the pipe
	LD_LIBRARY_PATH=. ./pwqcheck --multi -1 wordlist=$TRY2 < $TRY1 | grep -q '^OK: ' || mv $TRY2 $LIST
	set -o pipefail
done < <(tail -n +$((COUNT+1)) $LIST)

rm -f $TRY1 $TRY2

# Encode into a C program with a variation of Alec Muffett's DAWG compression

exec > $C
echo "/* Generated with ${0##*/} */"
echo "#include \"$H\""
echo 'const char * const _passwdqc_wordlist ='
prev=
max=0
while IFS= read -r LINE; do
	if [ -z "$prev" ]; then
		printf '\t"%s\"' "$LINE"
	else
		a=$prev b=$LINE
		i=0
		while (( i < ${#a} && i < ${#b} )) && [ "${a:i:1}" = "${b:i:1}" ]; do
			((++i))
		done

		test $i -le 30 || i=30
		c=${b:i}
		c=${c/\\/\\\\}
		if [[ "$c" = [0-9]* ]]; then
			printf '\n\t"\\%o""%s\"' $((i+1)) "$c"
		else
			printf '\n\t"\\%o%s\"' $((i+1)) "$c"
		fi
	fi
	prev=$LINE
	if [ ${#prev} -gt $max ]; then
		max=${#prev}
	fi
done < <(sort -u $LIST)
echo ';'

exec > $H
echo "/* Generated with ${0##*/} */"
echo "#define WORDLIST_LENGTH_MAX $max"
echo 'extern const char * const _passwdqc_wordlist;'

trap '' EXIT
