/*
 * Copyright (c) 2000-2002,2010 by Solar Designer.  See LICENSE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <pwd.h>

#include "passwdqc.h"
#include "wordset_4k.h"

#define REASON_ERROR \
	"check failed"

#define REASON_SAME \
	"is the same as the old one"
#define REASON_SIMILAR \
	"is based on the old one"

#define REASON_SHORT \
	"too short"
#define REASON_LONG \
	"too long"

#define REASON_SIMPLESHORT \
	"not enough different characters or classes for this length"
#define REASON_SIMPLE \
	"not enough different characters or classes"

#define REASON_PERSONAL \
	"based on personal login information"

#define REASON_WORD \
	"based on a dictionary word and not a passphrase"

#define REASON_SEQ \
	"based on a common sequence of characters and not a passphrase"

#define FIXED_BITS			15

typedef unsigned long fixed;

/*
 * Calculates the expected number of different characters for a random
 * password of a given length.  The result is rounded down.  We use this
 * with the _requested_ minimum length (so longer passwords don't have
 * to meet this strict requirement for their length).
 */
static int expected_different(int charset, int length)
{
	fixed x, y, z;

	x = ((fixed)(charset - 1) << FIXED_BITS) / charset;
	y = x;
	while (--length > 0)
		y = (y * x) >> FIXED_BITS;
	z = (fixed)charset * (((fixed)1 << FIXED_BITS) - y);

	return (int)(z >> FIXED_BITS);
}

/*
 * A password is too simple if it is too short for its class, or doesn't
 * contain enough different characters for its class, or doesn't contain
 * enough words for a passphrase.
 *
 * The bias may be positive or negative.  It is added to the length,
 * except that a negative bias is not considered in the passphrase
 * length check because a passphrase is expected to contain words.
 * The bias does not apply to the number of different characters; the
 * actual number is used in all checks.
 */
static int is_simple(const passwdqc_params_qc_t *params, const char *newpass,
    int bias)
{
	int length, classes, words, chars;
	int digits, lowers, uppers, others, unknowns;
	int c, p;

	length = classes = words = chars = 0;
	digits = lowers = uppers = others = unknowns = 0;
	p = ' ';
	while ((c = (unsigned char)newpass[length])) {
		length++;

		if (!isascii(c))
			unknowns++;
		else if (isdigit(c))
			digits++;
		else if (islower(c))
			lowers++;
		else if (isupper(c))
			uppers++;
		else
			others++;

		if (isascii(c) && isalpha(c) && isascii(p) && !isalpha(p))
			words++;
		p = c;

		if (!strchr(&newpass[length], c))
			chars++;
	}

	if (!length)
		return 1;

/* Upper case characters and digits used in common ways don't increase the
 * strength of a password */
	c = (unsigned char)newpass[0];
	if (uppers && isascii(c) && isupper(c))
		uppers--;
	c = (unsigned char)newpass[length - 1];
	if (digits && isascii(c) && isdigit(c))
		digits--;

/* Count the number of different character classes we've seen.  We assume
 * that there are no non-ASCII characters for digits. */
	classes = 0;
	if (digits)
		classes++;
	if (lowers)
		classes++;
	if (uppers)
		classes++;
	if (others)
		classes++;
	if (unknowns && (!classes || (digits && classes == 1)))
		classes++;

	for (; classes > 0; classes--)
	switch (classes) {
	case 1:
		if (length + bias >= params->min[0] &&
		    chars >= expected_different(10, params->min[0]) - 1)
			return 0;
		return 1;

	case 2:
		if (length + bias >= params->min[1] &&
		    chars >= expected_different(36, params->min[1]) - 1)
			return 0;
		if (!params->passphrase_words ||
		    words < params->passphrase_words)
			continue;
		if (length + (bias > 0 ? bias : 0) >= params->min[2] &&
		    chars >= expected_different(27, params->min[2]) - 1)
			return 0;
		continue;

	case 3:
		if (length + bias >= params->min[3] &&
		    chars >= expected_different(62, params->min[3]) - 1)
			return 0;
		continue;

	case 4:
		if (length + bias >= params->min[4] &&
		    chars >= expected_different(95, params->min[4]) - 1)
			return 0;
		continue;
	}

	return 1;
}

static char *unify(const char *src)
{
	const char *sptr;
	char *dst, *dptr;
	int c;

	if (!(dst = malloc(strlen(src) + 1)))
		return NULL;

	sptr = src;
	dptr = dst;
	do {
		c = (unsigned char)*sptr;
		if (isascii(c) && isupper(c))
			c = tolower(c);
		switch (c) {
		case 'a': case '@':
			c = '4'; break;
		case 'e':
			c = '3'; break;
/* Unfortunately, if we translate both 'i' and 'l' to '1', this would
 * associate these two letters with each other - e.g., "mile" would
 * match "MLLE", which is undesired.  To solve this, we'd need to test
 * different translations separately, which is not implemented yet. */
		case 'i': case '|':
			c = '!'; break;
		case 'l':
			c = '1'; break;
		case 'o':
			c = '0'; break;
		case 's': case '$':
			c = '5'; break;
		case 't': case '+':
			c = '7'; break;
		}
		*dptr++ = c;
	} while (*sptr++);

	return dst;
}

static char *reverse(const char *src)
{
	const char *sptr;
	char *dst, *dptr;

	if (!(dst = malloc(strlen(src) + 1)))
		return NULL;

	sptr = &src[strlen(src)];
	dptr = dst;
	while (sptr > src)
		*dptr++ = *--sptr;
	*dptr = '\0';

	return dst;
}

static void clean(char *dst)
{
	if (dst) {
		memset(dst, 0, strlen(dst));
		free(dst);
	}
}

/*
 * Needle is based on haystack if both contain a long enough common
 * substring and needle would be too simple for a password with the
 * substring either removed with partial length credit for it added
 * or partially discounted for the purpose of the length check.
 */
static int is_based(const passwdqc_params_qc_t *params,
    const char *haystack, const char *needle, const char *original,
    int mode)
{
	char *scratch;
	int length;
	int i, j, k;
	const char *p;
	int bias;

	if (!params->match_length)	/* disabled */
		return 0;

	if (params->match_length < 0)	/* misconfigured */
		return 1;

	if (strstr(haystack, needle))	/* based on haystack entirely */
		return 1;

	scratch = NULL;

	length = strlen(needle);
	for (i = 0; i <= length - params->match_length; i++)
	for (j = params->match_length; i + j <= length; j++) {
		bias = 0;
		for (p = haystack; *p; p++)
		if (*p == needle[i] && !strncmp(p, &needle[i], j)) {
			if (mode == 0) { /* remove & credit */
				if (!scratch) {
					if (!(scratch = malloc(length + 1)))
						return 1;
				}
				/* remove j chars */
				memcpy(scratch, original, i);
				memcpy(&scratch[i], &original[i + j],
				    length + 1 - (i + j));
				/* add credit for match_length - 1 chars */
				bias = params->match_length - 1;
				if (is_simple(params, scratch, bias)) {
					clean(scratch);
					return 1;
				}
			} else { /* discount */
/* Require a 1 character longer match for substrings containing leetspeak
 * when matching against dictionary words */
				bias = -1;
				if (mode == 1)
				for (k = i; k < i + j; k++)
				if (!isalpha((int)(unsigned char)original[k])) {
					if (j == params->match_length)
						goto next_match_length;
					bias = 0;
					break;
				}

				/* discount j - (match_length + bias) chars */
				bias += (int)params->match_length - j;
				/* bias <= -1 */
				if (is_simple(params, original, bias))
					return 1;
			}
		}
/* Zero bias implies that there were no matches for this length.  If so,
 * there's no reason to try the next substring length (it would result in
 * no matches as well).  We break out of the substring length loop and
 * proceed with all substring lengths for the next position in needle. */
		if (!bias)
			break;
next_match_length:
		;
	}

	clean(scratch);

	return 0;
}

const char *seq[] = {
	"0123456789",
	"`1234567890-=",
	"~!@#$%^&*()_+",
	"abcdefghijklmnopqrstuvwxyz",
	"qwertyuiop[]\\asdfghjkl;'zxcvbnm,./",
	"qwertyuiop{}|asdfghjkl:\"zxcvbnm<>?",
	"qwertyuiopasdfghjklzxcvbnm",
	"1qaz2wsx3edc4rfv5tgb6yhn7ujm8ik,9ol.0p;/-['=]\\",
	"qazwsxedcrfvtgbyhnujmikolp"
};

/*
 * This wordlist check is now the least important given the checks above
 * and the support for passphrases (which are based on dictionary words,
 * and checked by other means).  It is still useful to trap simple short
 * passwords (if short passwords are allowed) that are word-based, but
 * passed the other checks due to uncommon capitalization, digits, and
 * special characters.  We (mis)use the same set of words that are used
 * to generate random passwords.  This list is much smaller than those
 * used for password crackers, and it doesn't contain common passwords
 * that aren't short English words.  Perhaps support for large wordlists
 * should still be added, even though this is now of little importance.
 */
static const char *is_word_based(const passwdqc_params_qc_t *params,
    const char *needle, const char *original)
{
	char word[7];
	char *unified;
	int i;

	if (!params->match_length)	/* disabled */
		return NULL;

	word[6] = '\0';
	for (i = 0; i < 0x1000; i++) {
		memcpy(word, _passwdqc_wordset_4k[i], 6);
		if ((int)strlen(word) < params->match_length)
			continue;
		unified = unify(word);
		if (is_based(params, unified, needle, original, 1)) {
			free(unified);
			return REASON_WORD;
		}
		free(unified);
	}

	for (i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
		unified = unify(seq[i]);
		if (is_based(params, unified, needle, original, 2)) {
			free(unified);
			return REASON_SEQ;
		}
		free(unified);
	}

	if (params->match_length <= 4)
	for (i = 1900; i < 2099; i++) {
		sprintf(word, "%d", i);
		if (is_based(params, word, needle, original, 2))
			return REASON_SEQ;
	}

	return NULL;
}

const char *passwdqc_check(const passwdqc_params_qc_t *params,
    const char *newpass, const char *oldpass, const struct passwd *pw)
{
	char truncated[9], *reversed;
	char *u_newpass, *u_reversed;
	char *u_oldpass;
	char *u_name, *u_gecos, *u_dir;
	const char *reason;
	int length;

	reversed = NULL;
	u_newpass = u_reversed = NULL;
	u_oldpass = NULL;
	u_name = u_gecos = u_dir = NULL;

	reason = NULL;

	if (oldpass && !strcmp(oldpass, newpass))
		reason = REASON_SAME;

	length = strlen(newpass);

	if (!reason && length < params->min[4])
		reason = REASON_SHORT;

	if (!reason && length > params->max) {
		if (params->max == 8) {
			truncated[0] = '\0';
			strncat(truncated, newpass, 8);
			newpass = truncated;
			if (oldpass && !strncmp(oldpass, newpass, 8))
				reason = REASON_SAME;
		} else
			reason = REASON_LONG;
	}

	if (!reason && is_simple(params, newpass, 0)) {
		if (length < params->min[1] && params->min[1] <= params->max)
			reason = REASON_SIMPLESHORT;
		else
			reason = REASON_SIMPLE;
	}

	if (!reason) {
		if ((reversed = reverse(newpass))) {
			u_newpass = unify(newpass);
			u_reversed = unify(reversed);
			if (oldpass)
				u_oldpass = unify(oldpass);
			if (pw) {
				u_name = unify(pw->pw_name);
				u_gecos = unify(pw->pw_gecos);
				u_dir = unify(pw->pw_dir);
			}
		}
		if (!reversed ||
		    !u_newpass || !u_reversed ||
		    (oldpass && !u_oldpass) ||
		    (pw && (!u_name || !u_gecos || !u_dir)))
			reason = REASON_ERROR;
	}

	if (!reason && oldpass && params->similar_deny &&
	    (is_based(params, u_oldpass, u_newpass, newpass, 0) ||
	     is_based(params, u_oldpass, u_reversed, reversed, 0)))
		reason = REASON_SIMILAR;

	if (!reason && pw &&
	    (is_based(params, u_name, u_newpass, newpass, 0) ||
	     is_based(params, u_name, u_reversed, reversed, 0) ||
	     is_based(params, u_gecos, u_newpass, newpass, 0) ||
	     is_based(params, u_gecos, u_reversed, reversed, 0) ||
	     is_based(params, u_dir, u_newpass, newpass, 0) ||
	     is_based(params, u_dir, u_reversed, reversed, 0)))
		reason = REASON_PERSONAL;

	if (!reason)
		(reason = is_word_based(params, u_newpass, newpass)) ||
		(reason = is_word_based(params, u_reversed, reversed));

	memset(truncated, 0, sizeof(truncated));
	clean(reversed);
	clean(u_newpass);
	clean(u_reversed);
	clean(u_oldpass);
	clean(u_name);
	clean(u_gecos);
	clean(u_dir);

	return reason;
}
