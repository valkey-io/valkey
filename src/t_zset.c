/*
 * Copyright (c) 2009-2012, Redis Ltd.
 * Copyright (c) 2009-2012, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*-----------------------------------------------------------------------------
 * Sorted set API
 *----------------------------------------------------------------------------*/

/* ZSETs are ordered sets using two data structures to hold the same elements
 * in order to get O(log(N)) INSERT and REMOVE operations into a sorted
 * data structure.
 *
 * The elements are added to a hash table mapping elements to scores.
 * At the same time the elements are added to an ordered index mapping scores
 * to elements (so elements are sorted by scores in this "view").
 *
 * Note that the element string is shared between the hash table and the
 * ordered index in order to save memory. The element is freed only in
 * orderedIndexItemFree(). The hash table has no value free method set.
 * So we should always remove an element from the hash table, and later from
 * the ordered index. */

#include "server.h"
#include "ordered_index.h"
#include "intset.h" /* Compact integer set structure */
#include "mt19937-64.h"
#include <math.h>

#include "valkey_strtod.h"

/*-----------------------------------------------------------------------------
 * Zset range comparison utilities
 *
 * These are generic range-spec helpers used by both listpack and ordered index
 * encoded zsets. They have no dependency on any specific data structure.
 *----------------------------------------------------------------------------*/

int zsetScoreGteMin(double value, zrangespec *spec) {
    return spec->minex ? (value > spec->min) : (value >= spec->min);
}

int zsetScoreLteMax(double value, zrangespec *spec) {
    return spec->maxex ? (value < spec->max) : (value <= spec->max);
}

/* Compare two sds strings handling shared.minstring/maxstring as -inf/+inf. */
/* Compare a raw element (ptr+len) against an sds range bound.
 * Either side may be shared.minstring/maxstring as -inf/+inf sentinels. */
int zsetLexCompare(const char *a, size_t alen, sds b) {
    if ((const char *)a == (const char *)b) return 0;
    if (a == shared.minstring || b == shared.maxstring) return -1;
    if (a == shared.maxstring || b == shared.minstring) return 1;
    int cmp = memcmp(a, b, alen < sdslen(b) ? alen : sdslen(b));
    if (cmp != 0) return cmp;
    return alen < sdslen(b) ? -1 : (alen > sdslen(b) ? 1 : 0);
}

int zsetLexGteMin(const char *value, size_t len, zlexrangespec *spec) {
    return spec->minex ? (zsetLexCompare(value, len, spec->min) > 0) : (zsetLexCompare(value, len, spec->min) >= 0);
}

int zsetLexLteMax(const char *value, size_t len, zlexrangespec *spec) {
    return spec->maxex ? (zsetLexCompare(value, len, spec->max) < 0) : (zsetLexCompare(value, len, spec->max) <= 0);
}

void zsetConvertAndExpand(robj *zobj, int encoding, unsigned long cap);

static int zslParseRange(robj *min, robj *max, zrangespec *spec) {
    char *eptr;
    spec->minex = spec->maxex = 0;

    /* Parse the min-max interval. If one of the values is prefixed
     * by the "(" character, it's considered "open". For instance
     * ZRANGEBYSCORE zset (1.5 (2.5 will match min < x < max
     * ZRANGEBYSCORE zset 1.5 2.5 will instead match min <= x <= max */
    if (min->encoding == OBJ_ENCODING_INT) {
        spec->min = (long)objectGetVal(min);
    } else {
        char *s = objectGetVal(min);
        size_t len = sdslen(s);
        if (s[0] == '(') {
            spec->min = valkey_strtod_n(s + 1, len - 1, &eptr);
            if (eptr[0] != '\0' || isnan(spec->min)) return C_ERR;
            spec->minex = 1;
        } else {
            spec->min = valkey_strtod_n(s, len, &eptr);
            if (eptr[0] != '\0' || isnan(spec->min)) return C_ERR;
        }
    }
    if (max->encoding == OBJ_ENCODING_INT) {
        spec->max = (long)objectGetVal(max);
    } else {
        char *s = objectGetVal(max);
        size_t len = sdslen(s);
        if (s[0] == '(') {
            spec->max = valkey_strtod_n(s + 1, len - 1, &eptr);
            if (eptr[0] != '\0' || isnan(spec->max)) return C_ERR;
            spec->maxex = 1;
        } else {
            spec->max = valkey_strtod_n(s, len, &eptr);
            if (eptr[0] != '\0' || isnan(spec->max)) return C_ERR;
        }
    }

    return C_OK;
}
static int zslParseLexRangeItem(robj *item, sds *dest, int *ex) {
    char *c = objectGetVal(item);

    switch (c[0]) {
    case '+':
        if (c[1] != '\0') return C_ERR;
        *ex = 1;
        *dest = shared.maxstring;
        return C_OK;
    case '-':
        if (c[1] != '\0') return C_ERR;
        *ex = 1;
        *dest = shared.minstring;
        return C_OK;
    case '(':
        *ex = 1;
        *dest = sdsnewlen(c + 1, sdslen(c) - 1);
        return C_OK;
    case '[':
        *ex = 0;
        *dest = sdsnewlen(c + 1, sdslen(c) - 1);
        return C_OK;
    default: return C_ERR;
    }
}
/* Free a lex range structure, must be called only after zsetParseLexRange()
 * populated the structure with success (C_OK returned). */
void zsetFreeLexRange(zlexrangespec *spec) {
    if (spec->min != shared.minstring && spec->min != shared.maxstring) sdsfree(spec->min);
    if (spec->max != shared.minstring && spec->max != shared.maxstring) sdsfree(spec->max);
}

/* Populate the lex rangespec according to the objects min and max.
 *
 * Return C_OK on success. On error C_ERR is returned.
 * When OK is returned the structure must be freed with zsetFreeLexRange(),
 * otherwise no release is needed. */
int zsetParseLexRange(robj *min, robj *max, zlexrangespec *spec) {
    /* The range can't be valid if objects are integer encoded.
     * Every item must start with ( or [. */
    if (min->encoding == OBJ_ENCODING_INT || max->encoding == OBJ_ENCODING_INT) return C_ERR;

    spec->min = spec->max = NULL;
    if (zslParseLexRangeItem(min, &spec->min, &spec->minex) == C_ERR ||
        zslParseLexRangeItem(max, &spec->max, &spec->maxex) == C_ERR) {
        zsetFreeLexRange(spec);
        return C_ERR;
    } else {
        return C_OK;
    }
}

/*-----------------------------------------------------------------------------
 * Listpack-backed sorted set API
 *----------------------------------------------------------------------------*/

static double zzlStrtod(unsigned char *vstr, unsigned int vlen) {
    return valkey_strtod_n((const char *)vstr, vlen, NULL);
}

double zzlGetScore(unsigned char *sptr) {
    unsigned char *vstr;
    unsigned int vlen;
    long long vlong;
    double score;

    serverAssert(sptr != NULL);
    vstr = lpGetValue(sptr, &vlen, &vlong);

    if (vstr) {
        score = zzlStrtod(vstr, vlen);
    } else {
        score = vlong;
    }

    return score;
}

/* Validate that none of the scores in a listpack-encoded sorted set is NAN.
 * The structural layout (member, score, member, score, ...) must already have
 * been validated by lpValidateIntegrityAndDups. Returns 1 if all scores are
 * valid, 0 if a NAN score is found. This guards against crafted RESTORE
 * payloads: zslInsertNode() asserts the score is not NAN, so a NAN score in a
 * listpack zset would crash the server when it is later converted to a
 * skiplist. The skiplist RDB format rejects NAN scores at load time; this is
 * the equivalent check for the listpack format. */
int zzlValidateScores(unsigned char *zl) {
    unsigned char *eptr = lpSeek(zl, 0), *sptr;
    while (eptr != NULL) {
        sptr = lpNext(zl, eptr);
        if (sptr == NULL) return 0; /* odd number of elements */
        if (isnan(zzlGetScore(sptr))) return 0;
        eptr = lpNext(zl, sptr);
    }
    return 1;
}

/* Return a listpack element as an SDS string. */
sds lpGetObject(unsigned char *sptr) {
    unsigned char *vstr;
    unsigned int vlen;
    long long vlong;

    serverAssert(sptr != NULL);
    vstr = lpGetValue(sptr, &vlen, &vlong);

    if (vstr) {
        return sdsnewlen((char *)vstr, vlen);
    } else {
        return sdsfromlonglong(vlong);
    }
}

/* Compare element in sorted set with given element. */
static int zzlCompareElements(unsigned char *eptr, unsigned char *cstr, unsigned int clen) {
    unsigned char *vstr;
    unsigned int vlen;
    long long vlong;
    unsigned char vbuf[32];
    int minlen, cmp;

    vstr = lpGetValue(eptr, &vlen, &vlong);
    if (vstr == NULL) {
        /* Store string representation of long long in buf. */
        vlen = ll2string((char *)vbuf, sizeof(vbuf), vlong);
        vstr = vbuf;
    }

    minlen = (vlen < clen) ? vlen : clen;
    cmp = memcmp(vstr, cstr, minlen);
    if (cmp == 0) return vlen - clen;
    return cmp;
}

static unsigned int zzlLength(unsigned char *zl) {
    return lpLength(zl) / 2;
}

/* Move to next entry based on the values in eptr and sptr. Both are set to
 * NULL when there is no next entry. */
void zzlNext(unsigned char *zl, unsigned char **eptr, unsigned char **sptr) {
    unsigned char *_eptr, *_sptr;
    serverAssert(*eptr != NULL && *sptr != NULL);

    _eptr = lpNext(zl, *sptr);
    if (_eptr != NULL) {
        _sptr = lpNext(zl, _eptr);
        serverAssert(_sptr != NULL);
    } else {
        /* No next entry. */
        _sptr = NULL;
    }

    *eptr = _eptr;
    *sptr = _sptr;
}

/* Move to the previous entry based on the values in eptr and sptr. Both are
 * set to NULL when there is no prev entry. */
void zzlPrev(unsigned char *zl, unsigned char **eptr, unsigned char **sptr) {
    unsigned char *_eptr, *_sptr;
    serverAssert(*eptr != NULL && *sptr != NULL);

    _sptr = lpPrev(zl, *eptr);
    if (_sptr != NULL) {
        _eptr = lpPrev(zl, _sptr);
        serverAssert(_eptr != NULL);
    } else {
        /* No previous entry. */
        _eptr = NULL;
    }

    *eptr = _eptr;
    *sptr = _sptr;
}

/* Returns if there is a part of the zset is in range. Should only be used
 * internally by zzlFirstInRange and zzlLastInRange. */
int zzlIsInRange(unsigned char *zl, zrangespec *range) {
    unsigned char *p;
    double score;

    /* Test for ranges that will always be empty. */
    if (range->min > range->max || (range->min == range->max && (range->minex || range->maxex))) return 0;

    p = lpSeek(zl, -1);      /* Last score. */
    if (p == NULL) return 0; /* Empty sorted set */
    score = zzlGetScore(p);
    if (!zsetScoreGteMin(score, range)) return 0;

    p = lpSeek(zl, 1); /* First score. */
    serverAssert(p != NULL);
    score = zzlGetScore(p);
    if (!zsetScoreLteMax(score, range)) return 0;

    return 1;
}

/* Find pointer to the first element contained in the specified range.
 * Returns NULL when no element is contained in the range. */
unsigned char *zzlFirstInRange(unsigned char *zl, zrangespec *range) {
    unsigned char *eptr = lpSeek(zl, 0), *sptr;
    double score;

    /* If everything is out of range, return early. */
    if (!zzlIsInRange(zl, range)) return NULL;

    while (eptr != NULL) {
        sptr = lpNext(zl, eptr);
        serverAssert(sptr != NULL);

        score = zzlGetScore(sptr);
        if (zsetScoreGteMin(score, range)) {
            /* Check if score <= max. */
            if (zsetScoreLteMax(score, range)) return eptr;
            return NULL;
        }

        /* Move to next element. */
        eptr = lpNext(zl, sptr);
    }

    return NULL;
}

/* Find pointer to the last element contained in the specified range.
 * Returns NULL when no element is contained in the range. */
unsigned char *zzlLastInRange(unsigned char *zl, zrangespec *range) {
    unsigned char *eptr = lpSeek(zl, -2), *sptr;
    double score;

    /* If everything is out of range, return early. */
    if (!zzlIsInRange(zl, range)) return NULL;

    while (eptr != NULL) {
        sptr = lpNext(zl, eptr);
        serverAssert(sptr != NULL);

        score = zzlGetScore(sptr);
        if (zsetScoreLteMax(score, range)) {
            /* Check if score >= min. */
            if (zsetScoreGteMin(score, range)) return eptr;
            return NULL;
        }

        /* Move to previous element by moving to the score of previous element.
         * When this returns NULL, we know there also is no element. */
        sptr = lpPrev(zl, eptr);
        if (sptr != NULL)
            serverAssert((eptr = lpPrev(zl, sptr)) != NULL);
        else
            eptr = NULL;
    }

    return NULL;
}

int zzlLexValueGteMin(unsigned char *p, zlexrangespec *spec) {
    unsigned int len;
    long long lval;
    unsigned char *vstr = lpGetValue(p, &len, &lval);
    if (vstr) {
        return zsetLexGteMin((char *)vstr, len, spec);
    } else {
        char buf[LONG_STR_SIZE];
        int blen = ll2string(buf, sizeof(buf), lval);
        return zsetLexGteMin(buf, blen, spec);
    }
}

int zzlLexValueLteMax(unsigned char *p, zlexrangespec *spec) {
    unsigned int len;
    long long lval;
    unsigned char *vstr = lpGetValue(p, &len, &lval);
    if (vstr) {
        return zsetLexLteMax((char *)vstr, len, spec);
    } else {
        char buf[LONG_STR_SIZE];
        int blen = ll2string(buf, sizeof(buf), lval);
        return zsetLexLteMax(buf, blen, spec);
    }
}

/* Returns if there is a part of the zset is in range. Should only be used
 * internally by zzlFirstInLexRange and zzlLastInLexRange. */
int zzlIsInLexRange(unsigned char *zl, zlexrangespec *range) {
    unsigned char *p;

    /* Test for ranges that will always be empty. */
    int cmp = zsetLexCompare(range->min, sdslen(range->min), range->max);
    if (cmp > 0 || (cmp == 0 && (range->minex || range->maxex))) return 0;

    p = lpSeek(zl, -2); /* Last element. */
    if (p == NULL) return 0;
    if (!zzlLexValueGteMin(p, range)) return 0;

    p = lpSeek(zl, 0); /* First element. */
    serverAssert(p != NULL);
    if (!zzlLexValueLteMax(p, range)) return 0;

    return 1;
}

/* Find pointer to the first element contained in the specified lex range.
 * Returns NULL when no element is contained in the range. */
unsigned char *zzlFirstInLexRange(unsigned char *zl, zlexrangespec *range) {
    unsigned char *eptr = lpSeek(zl, 0), *sptr;

    /* If everything is out of range, return early. */
    if (!zzlIsInLexRange(zl, range)) return NULL;

    while (eptr != NULL) {
        if (zzlLexValueGteMin(eptr, range)) {
            /* Check if score <= max. */
            if (zzlLexValueLteMax(eptr, range)) return eptr;
            return NULL;
        }

        /* Move to next element. */
        sptr = lpNext(zl, eptr); /* This element score. Skip it. */
        serverAssert(sptr != NULL);
        eptr = lpNext(zl, sptr); /* Next element. */
    }

    return NULL;
}

/* Find pointer to the last element contained in the specified lex range.
 * Returns NULL when no element is contained in the range. */
unsigned char *zzlLastInLexRange(unsigned char *zl, zlexrangespec *range) {
    unsigned char *eptr = lpSeek(zl, -2), *sptr;

    /* If everything is out of range, return early. */
    if (!zzlIsInLexRange(zl, range)) return NULL;

    while (eptr != NULL) {
        if (zzlLexValueLteMax(eptr, range)) {
            /* Check if score >= min. */
            if (zzlLexValueGteMin(eptr, range)) return eptr;
            return NULL;
        }

        /* Move to previous element by moving to the score of previous element.
         * When this returns NULL, we know there also is no element. */
        sptr = lpPrev(zl, eptr);
        if (sptr != NULL)
            serverAssert((eptr = lpPrev(zl, sptr)) != NULL);
        else
            eptr = NULL;
    }

    return NULL;
}

static unsigned char *zzlFind(unsigned char *lp, sds ele, double *score) {
    unsigned char *eptr, *sptr;

    if ((eptr = lpFirst(lp)) == NULL) return NULL;
    eptr = lpFind(lp, eptr, (unsigned char *)ele, sdslen(ele), 1);
    if (eptr) {
        sptr = lpNext(lp, eptr);
        serverAssert(sptr != NULL);

        /* Matching element, pull out score. */
        if (score != NULL) *score = zzlGetScore(sptr);
        return eptr;
    }

    return NULL;
}

/* Delete (element,score) pair from listpack. Use local copy of eptr because we
 * don't want to modify the one given as argument. */
static unsigned char *zzlDelete(unsigned char *zl, unsigned char *eptr) {
    return lpDeleteRangeWithEntry(zl, &eptr, 2);
}

static unsigned char *zzlInsertAt(unsigned char *zl, unsigned char *eptr, const char *ele, size_t ele_len, double score) {
    unsigned char *sptr;
    char scorebuf[MAX_D2STRING_CHARS];
    int scorelen = 0;
    long long lscore;
    int score_is_long = double2ll(score, &lscore);
    if (!score_is_long) scorelen = d2string(scorebuf, sizeof(scorebuf), score);
    if (eptr == NULL) {
        zl = lpAppend(zl, (unsigned char *)ele, ele_len);
        if (score_is_long)
            zl = lpAppendInteger(zl, lscore);
        else
            zl = lpAppend(zl, (unsigned char *)scorebuf, scorelen);
    } else {
        /* Insert member before the element 'eptr'. */
        zl = lpInsertString(zl, (unsigned char *)ele, ele_len, eptr, LP_BEFORE, &sptr);

        /* Insert score after the member. */
        if (score_is_long)
            zl = lpInsertInteger(zl, lscore, sptr, LP_AFTER, NULL);
        else
            zl = lpInsertString(zl, (unsigned char *)scorebuf, scorelen, sptr, LP_AFTER, NULL);
    }
    return zl;
}

/* Insert (element,score) pair in listpack. This function assumes the element is
 * not yet present in the list. */
static unsigned char *zzlInsert(unsigned char *zl, sds ele, double score) {
    unsigned char *eptr = lpSeek(zl, 0), *sptr;
    double s;

    while (eptr != NULL) {
        sptr = lpNext(zl, eptr);
        serverAssert(sptr != NULL);
        s = zzlGetScore(sptr);

        if (s > score) {
            /* First element with score larger than score for element to be
             * inserted. This means we should take its spot in the list to
             * maintain ordering. */
            zl = zzlInsertAt(zl, eptr, ele, sdslen(ele), score);
            break;
        } else if (s == score) {
            /* Ensure lexicographical ordering for elements. */
            if (zzlCompareElements(eptr, (unsigned char *)ele, sdslen(ele)) > 0) {
                zl = zzlInsertAt(zl, eptr, ele, sdslen(ele), score);
                break;
            }
        }

        /* Move to next element. */
        eptr = lpNext(zl, sptr);
    }

    /* Push on tail of list when it was not yet inserted. */
    if (eptr == NULL) zl = zzlInsertAt(zl, NULL, ele, sdslen(ele), score);
    return zl;
}

/* Update the score of an element inside the sorted set listpack.
 * Note that the element must exist in the listpack. */
static unsigned char *zzlUpdateScore(unsigned char *zl, unsigned char *eptr, sds ele, double score, double curscore) {
    unsigned char *sptr = lpNext(zl, eptr);
    serverAssert(sptr != NULL);

    /* Fast path: if the new score keeps the element in its current slot under
     * (score, ele) ordering, replace the score entry in place instead of
     * delete + full re-scan.
     *
     * Only the neighbor in the direction of the score change needs to be
     * checked: if the score increases, the previous neighbor satisfies
     * (prev_score < score) strictly (since prev_score <= curscore < score), so
     * only the next neighbor can violate the order. The score-decrease case is
     * symmetric. */
    bool keep_position = true;
    if (score > curscore) {
        unsigned char *next_eptr = eptr, *next_sptr = sptr;
        zzlNext(zl, &next_eptr, &next_sptr);
        if (next_sptr != NULL) {
            double next_score = zzlGetScore(next_sptr);
            if (next_score < score ||
                (next_score == score && zzlCompareElements(next_eptr, (unsigned char *)ele, sdslen(ele)) < 0))
                keep_position = false;
        }
    } else { /* score < curscore */
        unsigned char *prev_eptr = eptr, *prev_sptr = sptr;
        zzlPrev(zl, &prev_eptr, &prev_sptr);
        if (prev_sptr != NULL) {
            double prev_score = zzlGetScore(prev_sptr);
            if (prev_score > score ||
                (prev_score == score && zzlCompareElements(prev_eptr, (unsigned char *)ele, sdslen(ele)) > 0))
                keep_position = false;
        }
    }

    if (keep_position) {
        long long lscore;
        if (double2ll(score, &lscore)) {
            zl = lpReplaceInteger(zl, &sptr, lscore);
        } else {
            char scorebuf[MAX_D2STRING_CHARS];
            int scorelen = d2string(scorebuf, sizeof(scorebuf), score);
            zl = lpReplace(zl, &sptr, (unsigned char *)scorebuf, scorelen);
        }
        return zl;
    }

    zl = zzlDelete(zl, eptr);
    return zzlInsert(zl, ele, score);
}

static unsigned char *zzlDeleteRangeByScore(unsigned char *zl, zrangespec *range, unsigned long *deleted) {
    unsigned char *eptr, *sptr;
    double score;
    unsigned long num = 0;

    if (deleted != NULL) *deleted = 0;

    eptr = zzlFirstInRange(zl, range);
    if (eptr == NULL) return zl;

    /* When the tail of the listpack is deleted, eptr will be NULL. */
    while (eptr && (sptr = lpNext(zl, eptr)) != NULL) {
        score = zzlGetScore(sptr);
        if (zsetScoreLteMax(score, range)) {
            /* Delete both the element and the score. */
            zl = lpDeleteRangeWithEntry(zl, &eptr, 2);
            num++;
        } else {
            /* No longer in range. */
            break;
        }
    }

    if (deleted != NULL) *deleted = num;
    return zl;
}

static unsigned char *zzlDeleteRangeByLex(unsigned char *zl, zlexrangespec *range, unsigned long *deleted) {
    unsigned char *eptr, *sptr;
    unsigned long num = 0;

    if (deleted != NULL) *deleted = 0;

    eptr = zzlFirstInLexRange(zl, range);
    if (eptr == NULL) return zl;

    /* When the tail of the listpack is deleted, eptr will be NULL. */
    while (eptr && (sptr = lpNext(zl, eptr)) != NULL) {
        if (zzlLexValueLteMax(eptr, range)) {
            /* Delete both the element and the score. */
            zl = lpDeleteRangeWithEntry(zl, &eptr, 2);
            num++;
        } else {
            /* No longer in range. */
            break;
        }
    }

    if (deleted != NULL) *deleted = num;
    return zl;
}

/* Delete all the elements with rank between start and end from the listpack.
 * Start and end are inclusive. Note that start and end need to be 1-based */
unsigned char *zzlDeleteRangeByRank(unsigned char *zl, unsigned int start, unsigned int end, unsigned long *deleted) {
    unsigned int num = (end - start) + 1;
    if (deleted) *deleted = num;
    zl = lpDeleteRange(zl, 2 * (start - 1), 2 * num);
    return zl;
}

/*-----------------------------------------------------------------------------
 * Common sorted set API
 *----------------------------------------------------------------------------*/

unsigned long zsetLength(const robj *zobj) {
    unsigned long length = 0;
    if (zobj->encoding == OBJ_ENCODING_LISTPACK) {
        length = zzlLength(objectGetVal(zobj));
    } else if (zobj->encoding == OBJ_ENCODING_BTREE) {
        length = orderedIndexLength(((const zset *)objectGetVal(zobj))->oi);
    } else {
        serverPanic("Unknown sorted set encoding");
    }
    return length;
}

/* Factory method to return a zset.
 *
 * The size hint indicates approximately how many items will be added,
 * and the value len hint indicates the approximate individual size of the added elements,
 * they are used to determine the initial representation.
 *
 * If the hints are not known, and underestimation or 0 is suitable.
 * We should never pass a negative value because it will convert to a very large unsigned number. */
robj *zsetTypeCreate(size_t size_hint, size_t val_len_hint) {
    if (size_hint <= server.zset_max_listpack_entries && val_len_hint <= server.zset_max_listpack_value) {
        return createZsetListpackObject();
    }

    robj *zobj = createZsetObject();
    zset *zs = objectGetVal(zobj);
    hashtableExpand(zs->ht, size_hint);
    return zobj;
}

/* Check if the existing zset should be converted to another encoding based off the
 * the size hint. */
void zsetTypeMaybeConvert(robj *zobj, size_t size_hint, size_t value_len_hint) {
    if (zobj->encoding == OBJ_ENCODING_LISTPACK &&
        (size_hint > server.zset_max_listpack_entries || value_len_hint > server.zset_max_listpack_value)) {
        zsetConvertAndExpand(zobj, OBJ_ENCODING_BTREE, size_hint);
    }
}

/* Convert the zset to specified encoding. The hashtable (when converting
 * to ordered index encoding) is presized to hold the number of elements in
 * the original zset. */
void zsetConvert(robj *zobj, int encoding) {
    zsetConvertAndExpand(zobj, encoding, zsetLength(zobj));
}

/* Converts a zset to the specified encoding, pre-sizing it for 'cap' elements. */
void zsetConvertAndExpand(robj *zobj, int encoding, unsigned long cap) {
    zset *zs;
    OrderedIndexItem *node;
    double score;

    if (zobj->encoding == encoding) return;
    if (zobj->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *zl = objectGetVal(zobj);
        unsigned char *eptr, *sptr;
        unsigned char *vstr;
        unsigned int vlen;
        long long vlong;

        if (encoding != OBJ_ENCODING_BTREE) serverPanic("Unknown target encoding");

        zs = zmalloc(sizeof(*zs));
        zs->ht = hashtableCreate(&zsetHashtableType);
        zs->oi = orderedIndexCreate();

        /* Presize the dict to avoid rehashing */
        hashtableExpand(zs->ht, cap);

        eptr = lpSeek(zl, 0);
        if (eptr != NULL) {
            sptr = lpNext(zl, eptr);
            serverAssertWithInfo(NULL, zobj, sptr != NULL);
        }

        while (eptr != NULL) {
            score = zzlGetScore(sptr);
            vstr = lpGetValue(eptr, &vlen, &vlong);
            char buf[LONG_STR_SIZE];
            const char *ele_ptr;
            size_t ele_len;
            if (vstr == NULL) {
                ele_len = ll2string(buf, sizeof(buf), vlong);
                ele_ptr = buf;
            } else {
                ele_ptr = (char *)vstr;
                ele_len = vlen;
            }

            node = orderedIndexInsert(zs->oi, score, ele_ptr, ele_len);
            serverAssert(hashtableAdd(zs->ht, node));
            zzlNext(zl, &eptr, &sptr);
        }

        zfree(objectGetVal(zobj));
        objectSetVal(zobj, zs);
        zobj->encoding = OBJ_ENCODING_BTREE;
    } else if (zobj->encoding == OBJ_ENCODING_BTREE) {
        unsigned char *zl = lpNew(0);

        if (encoding != OBJ_ENCODING_LISTPACK) serverPanic("Unknown target encoding");

        /* Free the ordered index by popping items one at a time into the listpack. */
        zs = objectGetVal(zobj);
        hashtableRelease(zs->ht);

        OrderedIndexItem *node;
        while ((node = orderedIndexPopFirst(zs->oi)) != NULL) {
            const char *ele_ptr;
            size_t ele_len;
            orderedIndexItemGetElement(node, &ele_ptr, &ele_len);
            zl = zzlInsertAt(zl, NULL, ele_ptr, ele_len, orderedIndexItemGetScore(node));
            orderedIndexItemFree(node);
        }
        orderedIndexFree(zs->oi);

        zfree(zs);
        objectSetVal(zobj, zl);
        zobj->encoding = OBJ_ENCODING_LISTPACK;
    } else {
        serverPanic("Unknown sorted set encoding");
    }
}

/* Convert the sorted set object into a listpack if it is not already a listpack
 * and if the number of elements and the maximum element size and total elements size
 * are within the expected ranges. */
void zsetConvertToListpackIfNeeded(robj *zobj, size_t maxelelen, size_t totelelen) {
    if (zobj->encoding == OBJ_ENCODING_LISTPACK) return;
    zset *zset = objectGetVal(zobj);

    if (orderedIndexLength(zset->oi) <= server.zset_max_listpack_entries &&
        maxelelen <= server.zset_max_listpack_value && lpSafeToAdd(NULL, totelelen)) {
        zsetConvert(zobj, OBJ_ENCODING_LISTPACK);
    }
}

/* Return (by reference) the score of the specified member of the sorted set
 * storing it into *score. If the element does not exist C_ERR is returned
 * otherwise C_OK is returned and *score is correctly populated.
 * If 'zobj' or 'member' is NULL, C_ERR is returned. */
int zsetScore(robj *zobj, sds member, double *score) {
    if (!zobj || !member) return C_ERR;

    if (zobj->encoding == OBJ_ENCODING_LISTPACK) {
        if (zzlFind(objectGetVal(zobj), member, score) == NULL) return C_ERR;
    } else if (zobj->encoding == OBJ_ENCODING_BTREE) {
        zset *zs = objectGetVal(zobj);
        void *entry;
        zsetMarkLookupKey(member);
        int found = hashtableFind(zs->ht, member, &entry);
        zsetUnmarkLookupKey(member);
        if (!found) return C_ERR;
        OrderedIndexItem *setElement = entry;
        *score = orderedIndexItemGetScore(setElement);
    } else {
        serverPanic("Unknown sorted set encoding");
    }
    return C_OK;
}

/* Add a new element or update the score of an existing element in a sorted
 * set, regardless of its encoding.
 *
 * The set of flags change the command behavior.
 *
 * The input flags are the following:
 *
 * ZADD_INCR: Increment the current element score by 'score' instead of updating
 *            the current element score. If the element does not exist, we
 *            assume 0 as previous score.
 * ZADD_NX:   Perform the operation only if the element does not exist.
 * ZADD_XX:   Perform the operation only if the element already exist.
 * ZADD_GT:   Perform the operation on existing elements only if the new score is
 *            greater than the current score.
 * ZADD_LT:   Perform the operation on existing elements only if the new score is
 *            less than the current score.
 *
 * When ZADD_INCR is used, the new score of the element is stored in
 * '*newscore' if 'newscore' is not NULL.
 *
 * The returned flags are the following:
 *
 * ZADD_NAN:     The resulting score is not a number.
 * ZADD_ADDED:   The element was added (not present before the call).
 * ZADD_UPDATED: The element score was updated.
 * ZADD_NOP:     No operation was performed because of NX or XX.
 *
 * Return value:
 *
 * The function returns 1 on success, and sets the appropriate flags
 * ADDED or UPDATED to signal what happened during the operation (note that
 * none could be set if we re-added an element using the same score it used
 * to have, or in the case a zero increment is used).
 *
 * The function returns 0 on error, currently only when the increment
 * produces a NAN condition, or when the 'score' value is NAN since the
 * start.
 *
 * The command as a side effect of adding a new element may convert the sorted
 * set internal encoding from listpack to hashtable+ordered index.
 *
 * Memory management of 'ele':
 *
 * The function does not take ownership of the 'ele' SDS string, but copies
 * it if needed. */
int zsetAdd(robj *zobj, double score, sds ele, int in_flags, int *out_flags, double *newscore) {
    /* Turn options into simple to check vars. */
    int incr = (in_flags & ZADD_IN_INCR) != 0;
    int nx = (in_flags & ZADD_IN_NX) != 0;
    int xx = (in_flags & ZADD_IN_XX) != 0;
    int gt = (in_flags & ZADD_IN_GT) != 0;
    int lt = (in_flags & ZADD_IN_LT) != 0;
    *out_flags = 0; /* We'll return our response flags. */
    double curscore;

    /* NaN as input is an error regardless of all the other parameters. */
    if (isnan(score)) {
        *out_flags = ZADD_OUT_NAN;
        return 0;
    }

    /* Update the sorted set according to its encoding. */
    if (zobj->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *eptr;

        if ((eptr = zzlFind(objectGetVal(zobj), ele, &curscore)) != NULL) {
            /* NX? Return, same element already exists. */
            if (nx) {
                *out_flags |= ZADD_OUT_NOP;
                return 1;
            }

            /* Prepare the score for the increment if needed. */
            if (incr) {
                score += curscore;
                if (isnan(score)) {
                    *out_flags |= ZADD_OUT_NAN;
                    return 0;
                }
            }

            /* GT/LT? Only update if score is greater/less than current. */
            if ((lt && score >= curscore) || (gt && score <= curscore)) {
                *out_flags |= ZADD_OUT_NOP;
                return 1;
            }

            if (newscore) *newscore = score;

            if (score != curscore) {
                objectSetVal(zobj, zzlUpdateScore(objectGetVal(zobj), eptr, ele, score, curscore));
                *out_flags |= ZADD_OUT_UPDATED;
            }
            return 1;
        } else if (!xx) {
            /* check if the element is too large or the list
             * becomes too long *before* executing zzlInsert. */
            if (zzlLength(objectGetVal(zobj)) + 1 > server.zset_max_listpack_entries ||
                sdslen(ele) > server.zset_max_listpack_value || !lpSafeToAdd(objectGetVal(zobj), sdslen(ele))) {
                zsetConvertAndExpand(zobj, OBJ_ENCODING_BTREE, zsetLength(zobj) + 1);
            } else {
                objectSetVal(zobj, zzlInsert(objectGetVal(zobj), ele, score));
                if (newscore) *newscore = score;
                *out_flags |= ZADD_OUT_ADDED;
                return 1;
            }
        } else {
            *out_flags |= ZADD_OUT_NOP;
            return 1;
        }
    }

    /* Note that the above block handling listpack would have either returned or
     * converted the key to ordered index encoding. */
    if (zobj->encoding == OBJ_ENCODING_BTREE) {
        zset *zs = objectGetVal(zobj);

        zsetMarkLookupKey(ele);
        void **node_ref_in_hashtable = hashtableFindRef(zs->ht, ele);
        zsetUnmarkLookupKey(ele);
        if (node_ref_in_hashtable != NULL) {
            /* NX? Return, same element already exists. */
            if (nx) {
                *out_flags |= ZADD_OUT_NOP;
                return 1;
            }

            OrderedIndexItem *old_node = *node_ref_in_hashtable;
            curscore = orderedIndexItemGetScore(old_node);

            /* Prepare the score for the increment if needed. */
            if (incr) {
                score += curscore;
                if (isnan(score)) {
                    *out_flags |= ZADD_OUT_NAN;
                    return 0;
                }
            }

            /* GT/LT? Only update if score is greater/less than current. */
            if ((lt && score >= curscore) || (gt && score <= curscore)) {
                *out_flags |= ZADD_OUT_NOP;
                return 1;
            }

            if (newscore) *newscore = score;

            /* Remove and re-insert when score changes. */
            if (score != curscore) {
                OrderedIndexItem *new_node = orderedIndexUpdateScore(zs->oi, old_node, score);
                /* Update the node pointer stored in the hashtable. */
                *node_ref_in_hashtable = new_node;
                *out_flags |= ZADD_OUT_UPDATED;
            }
            return 1;
        } else if (!xx) {
            OrderedIndexItem *new_node = orderedIndexInsert(zs->oi, score, ele, sdslen(ele));
            serverAssert(hashtableAdd(zs->ht, new_node));
            *out_flags |= ZADD_OUT_ADDED;
            if (newscore) *newscore = score;
            return 1;
        } else {
            *out_flags |= ZADD_OUT_NOP;
            return 1;
        }
    } else {
        serverPanic("Unknown sorted set encoding");
    }
    return 0; /* Never reached. */
}

/* Deletes the element 'ele' from the sorted set encoded as ordered index+hashtable,
 * returning 1 if the element existed and was deleted, 0 otherwise (the
 * element was not there). */
static int zsetRemoveFromIndex(zset *zs, sds ele) {
    void *entry;
    zsetMarkLookupKey(ele);
    int popped = hashtablePop(zs->ht, ele, &entry);
    zsetUnmarkLookupKey(ele);
    if (!popped) return 0;
    OrderedIndexItem *node = entry;

    /* hashtable only contains pointers to ordered index items. Nothing to free. */

    /* Delete from ordered index. */
    orderedIndexDelete(zs->oi, node);

    return 1;
}

/* Delete the element 'ele' from the sorted set, returning 1 if the element
 * existed and was deleted, 0 otherwise (the element was not there). */
int zsetDel(robj *zobj, sds ele) {
    if (zobj->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *eptr;

        if ((eptr = zzlFind(objectGetVal(zobj), ele, NULL)) != NULL) {
            objectSetVal(zobj, zzlDelete(objectGetVal(zobj), eptr));
            return 1;
        }
    } else if (zobj->encoding == OBJ_ENCODING_BTREE) {
        zset *zs = objectGetVal(zobj);
        if (zsetRemoveFromIndex(zs, ele)) {
            return 1;
        }
    } else {
        serverPanic("Unknown sorted set encoding");
    }
    return 0; /* No such element found. */
}

/* Given a sorted set object returns the 0-based rank of the object or
 * -1 if the object does not exist.
 *
 * For rank we mean the position of the element in the sorted collection
 * of elements. So the first element has rank 0, the second rank 1, and so
 * forth up to length-1 elements.
 *
 * If 'reverse' is false, the rank is returned considering as first element
 * the one with the lowest score. Otherwise, if 'reverse' is non-zero
 * the rank is computed considering as element with rank 0 the one with
 * the highest score. */
static long zsetRank(robj *zobj, sds ele, int reverse, double *output_score) {
    unsigned long llen;
    unsigned long rank;

    llen = zsetLength(zobj);

    if (zobj->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *zl = objectGetVal(zobj);
        unsigned char *eptr, *sptr;

        eptr = lpSeek(zl, 0);
        serverAssert(eptr != NULL);
        sptr = lpNext(zl, eptr);
        serverAssert(sptr != NULL);

        rank = 1;
        while (eptr != NULL) {
            if (lpCompare(eptr, (unsigned char *)ele, sdslen(ele))) break;
            rank++;
            zzlNext(zl, &eptr, &sptr);
        }

        if (eptr != NULL) {
            if (output_score) *output_score = zzlGetScore(sptr);
            if (reverse)
                return llen - rank;
            else
                return rank - 1;
        } else {
            return -1;
        }
    } else if (zobj->encoding == OBJ_ENCODING_BTREE) {
        zset *zs = objectGetVal(zobj);

        void *entry;
        zsetMarkLookupKey(ele);
        int found = hashtableFind(zs->ht, ele, &entry);
        zsetUnmarkLookupKey(ele);
        if (!found) return -1;
        OrderedIndexItem *node = entry;

        rank = orderedIndexGetIndex(zs->oi, node);
        if (output_score) *output_score = orderedIndexItemGetScore(node);
        if (reverse)
            return llen - rank - 1;
        else
            return rank;
    } else {
        serverPanic("Unknown sorted set encoding");
    }
}

/* This is a helper function for the COPY command.
 * Duplicate a sorted set object, with the guarantee that the returned object
 * has the same encoding as the original one.
 *
 * The resulting object always has refcount set to 1 */
robj *zsetDup(robj *o) {
    robj *zobj;
    zset *zs;
    zset *new_zs;

    serverAssert(objectGetType(o) == OBJ_ZSET);

    /* Create a new sorted set object that have the same encoding as the original object's encoding */
    if (objectGetEncoding(o) == OBJ_ENCODING_LISTPACK) {
        unsigned char *zl = objectGetVal(o);
        size_t sz = lpBytes(zl);
        unsigned char *new_zl = zmalloc(sz);
        memcpy(new_zl, zl, sz);
        zobj = createObject(OBJ_ZSET, new_zl);
        zobj->encoding = OBJ_ENCODING_LISTPACK;
    } else if (objectGetEncoding(o) == OBJ_ENCODING_BTREE) {
        zobj = createZsetObject();
        zs = objectGetVal(o);
        new_zs = objectGetVal(zobj);
        hashtableExpand(new_zs->ht, hashtableSize(zs->ht));
        OrderedIndex *oi = zs->oi;
        OrderedIndexItem *ln;

        /* We copy elements from the greatest to the smallest (that's trivial
         * since the elements are already ordered in the index): inserting in
         * descending order is optimal for the ordered index backend. */
        OrderedIndexIterator iter;
        orderedIndexInitIterator(&iter, oi);
        while ((ln = orderedIndexPrev(&iter)) != NULL) {
            const char *ele_ptr;
            size_t ele_len;
            orderedIndexItemGetElement(ln, &ele_ptr, &ele_len);
            OrderedIndexItem *znode = orderedIndexInsert(new_zs->oi, orderedIndexItemGetScore(ln), ele_ptr, ele_len);
            hashtableAdd(new_zs->ht, znode);
        }
    } else {
        serverPanic("Unknown sorted set encoding");
    }
    return zobj;
}

/* Create a new sds string from the listpack entry. */
sds zsetSdsFromListpackEntry(listpackEntry *e) {
    return e->sval ? sdsnewlen(e->sval, e->slen) : sdsfromlonglong(e->lval);
}

/* Reply with bulk string from the listpack entry. */
void zsetReplyFromListpackEntry(client *c, listpackEntry *e) {
    if (e->sval)
        addReplyBulkCBuffer(c, e->sval, e->slen);
    else
        addReplyBulkLongLong(c, e->lval);
}


/* Return random element from a non empty zset.
 * 'key' and 'val' will be set to hold the element.
 * The memory in `key` is not to be freed or modified by the caller.
 * 'score' can be NULL in which case it's not extracted. */
static void zsetTypeRandomElement(robj *zsetobj, unsigned long zsetsize, listpackEntry *key, double *score) {
    if (zsetobj->encoding == OBJ_ENCODING_BTREE) {
        zset *zs = objectGetVal(zsetobj);
        void *entry;
        hashtableFairRandomEntry(zs->ht, &entry);
        OrderedIndexItem *node = entry;
        const char *ele_ptr_tmp;
        size_t ele_len_tmp;
        orderedIndexItemGetElement(node, &ele_ptr_tmp, &ele_len_tmp);
        key->sval = (unsigned char *)ele_ptr_tmp;
        key->slen = ele_len_tmp;
        if (score) *score = orderedIndexItemGetScore(node);
    } else if (zsetobj->encoding == OBJ_ENCODING_LISTPACK) {
        listpackEntry val;
        lpRandomPair(objectGetVal(zsetobj), zsetsize, key, &val);
        if (score) {
            if (val.sval) {
                *score = zzlStrtod(val.sval, val.slen);
            } else {
                *score = (double)val.lval;
            }
        }
    } else {
        serverPanic("Unknown zset encoding");
    }
}

/*-----------------------------------------------------------------------------
 * Sorted set commands
 *----------------------------------------------------------------------------*/

/* This generic command implements both ZADD and ZINCRBY. */
static void zaddGenericCommand(client *c, int flags) {
    static char *nanerr = "resulting score is not a number (NaN)";
    robj *key = c->argv[1];
    robj *zobj;
    sds ele;
    double score = 0, *scores = NULL;
    int j, elements, ch = 0;
    size_t maxelelen = 0;
    int scoreidx = 0;
    int reply_err = 0;

    /* The following vars are used in order to track what the command actually
     * did during the execution, to reply to the client and to trigger the
     * notification of keyspace change. */
    int added = 0;     /* Number of new elements added. */
    int updated = 0;   /* Number of elements with updated score. */
    int processed = 0; /* Number of elements processed, may remain zero with
                          options like XX. */

    /* Parse options. At the end 'scoreidx' is set to the argument position
     * of the score of the first score-element pair. */
    scoreidx = 2;
    while (scoreidx < c->argc) {
        char *opt = objectGetVal(c->argv[scoreidx]);
        if (!strcasecmp(opt, "nx"))
            flags |= ZADD_IN_NX;
        else if (!strcasecmp(opt, "xx"))
            flags |= ZADD_IN_XX;
        else if (!strcasecmp(opt, "ch"))
            ch = 1; /* Return num of elements added or updated. */
        else if (!strcasecmp(opt, "incr"))
            flags |= ZADD_IN_INCR;
        else if (!strcasecmp(opt, "gt"))
            flags |= ZADD_IN_GT;
        else if (!strcasecmp(opt, "lt"))
            flags |= ZADD_IN_LT;
        else
            break;
        scoreidx++;
    }

    /* Turn options into simple to check vars. */
    int incr = (flags & ZADD_IN_INCR) != 0;
    int nx = (flags & ZADD_IN_NX) != 0;
    int xx = (flags & ZADD_IN_XX) != 0;
    int gt = (flags & ZADD_IN_GT) != 0;
    int lt = (flags & ZADD_IN_LT) != 0;

    /* After the options, we expect to have an even number of args, since
     * we expect any number of score-element pairs. */
    elements = c->argc - scoreidx;
    if (elements % 2 || !elements) {
        addReplyErrorObject(c, shared.syntaxerr);
        return;
    }
    elements /= 2; /* Now this holds the number of score-element pairs. */

    /* Check for incompatible options. */
    if (nx && xx) {
        addReplyError(c, "XX and NX options at the same time are not compatible");
        return;
    }

    if ((gt && nx) || (lt && nx) || (gt && lt)) {
        addReplyError(c, "GT, LT, and/or NX options at the same time are not compatible");
        return;
    }
    /* Note that XX is compatible with either GT or LT */

    if (incr && elements > 1) {
        addReplyError(c, "INCR option supports a single increment-element pair");
        return;
    }

    /* Start parsing all the scores, we need to emit any syntax error
     * before executing additions to the sorted set, as the command should
     * either execute fully or nothing at all. */
    scores = zmalloc(sizeof(double) * elements);
    for (j = 0; j < elements; j++) {
        if (getDoubleFromObjectOrReply(c, c->argv[scoreidx + j * 2], &scores[j], NULL) != C_OK) goto cleanup;
        ele = objectGetVal(c->argv[scoreidx + 1 + j * 2]);
        size_t elelen = sdslen(ele);
        if (elelen > maxelelen) maxelelen = elelen;
    }

    /* Lookup the key and create the sorted set if does not exist. */
    zobj = lookupKeyWrite(c->db, key);
    if (checkType(c, zobj, OBJ_ZSET)) goto cleanup;
    if (zobj == NULL) {
        if (xx) goto reply_to_client; /* No key + XX option: nothing to do. */
        zobj = zsetTypeCreate(elements, maxelelen);
        dbAdd(c->db, key, &zobj);
    } else {
        zsetTypeMaybeConvert(zobj, elements, maxelelen);
    }

    for (j = 0; j < elements; j++) {
        double newscore;
        score = scores[j];
        int retflags = 0;

        ele = objectGetVal(c->argv[scoreidx + 1 + j * 2]);
        int retval = zsetAdd(zobj, score, ele, flags, &retflags, &newscore);
        if (retval == 0) {
            reply_err = 1;
            break;
        }
        if (retflags & ZADD_OUT_ADDED) added++;
        if (retflags & ZADD_OUT_UPDATED) updated++;
        if (!(retflags & ZADD_OUT_NOP)) processed++;
        score = newscore;
    }
    if (!reply_err) {
        server.dirty += (added + updated);
    }
    if (added || updated) {
        signalModifiedKey(c, c->db, key);
        notifyKeyspaceEvent(NOTIFY_ZSET, incr ? "zincr" : "zadd", key, c->db->id);
    }

reply_to_client:
    if (reply_err) {
        addReplyError(c, nanerr);
    } else if (incr) { /* ZINCRBY or INCR option. */
        if (processed)
            addReplyDouble(c, score);
        else
            addReplyNull(c);
    } else { /* ZADD. */
        addReplyLongLong(c, ch ? added + updated : added);
    }
cleanup:
    zfree(scores);
}

void zaddCommand(client *c) {
    zaddGenericCommand(c, ZADD_IN_NONE);
}

void zincrbyCommand(client *c) {
    zaddGenericCommand(c, ZADD_IN_INCR);
}

void zremCommand(client *c) {
    robj *key = c->argv[1];
    robj *zobj;
    int deleted = 0, keyremoved = 0, j;

    if ((zobj = lookupKeyWriteOrReply(c, key, shared.czero)) == NULL || checkType(c, zobj, OBJ_ZSET)) return;

    if (zobj->encoding == OBJ_ENCODING_BTREE) hashtablePauseAutoShrink(((zset *)objectGetVal(zobj))->ht);
    for (j = 2; j < c->argc; j++) {
        if (zsetDel(zobj, objectGetVal(c->argv[j]))) deleted++;
        if (zsetLength(zobj) == 0) {
            dbDelete(c->db, key);
            keyremoved = 1;
            break;
        }
    }
    if (!keyremoved && zobj->encoding == OBJ_ENCODING_BTREE) hashtableResumeAutoShrink(((zset *)objectGetVal(zobj))->ht);

    if (deleted) {
        notifyKeyspaceEvent(NOTIFY_ZSET, "zrem", key, c->db->id);
        if (keyremoved) notifyKeyspaceEvent(NOTIFY_GENERIC, "del", key, c->db->id);
        signalModifiedKey(c, c->db, key);
        server.dirty += deleted;
    }
    addReplyLongLong(c, deleted);
}

typedef enum {
    ZRANGE_AUTO = 0,
    ZRANGE_RANK,
    ZRANGE_SCORE,
    ZRANGE_LEX,
} zrange_type;

/* Callback for orderedIndexDeleteRangeBy* — removes the item from the hashtable
 * and frees it. The callback receives ownership per the API contract. */
static void zsetIndexDeleteCallback(const OrderedIndexItem *item, void *privdata) {
    hashtable *ht = privdata;
    /* The packed item is the hashtable entry — pass it directly as the key.
     * zsetExtractElement sees it's unmarked and extracts element from offset 8. */
    hashtableDelete(ht, (sds)item);
}

/* Implements ZREMRANGEBYRANK, ZREMRANGEBYSCORE, ZREMRANGEBYLEX commands. */
void zremrangeGenericCommand(client *c, zrange_type rangetype) {
    robj *key = c->argv[1];
    robj *zobj;
    int keyremoved = 0;
    unsigned long deleted = 0;
    zrangespec range;
    zlexrangespec lexrange;
    long start, end, llen;
    char *notify_type = NULL;

    /* Step 1: Parse the range. */
    if (rangetype == ZRANGE_RANK) {
        notify_type = "zremrangebyrank";
        if ((getLongFromObjectOrReply(c, c->argv[2], &start, NULL) != C_OK) ||
            (getLongFromObjectOrReply(c, c->argv[3], &end, NULL) != C_OK))
            return;
    } else if (rangetype == ZRANGE_SCORE) {
        notify_type = "zremrangebyscore";
        if (zslParseRange(c->argv[2], c->argv[3], &range) != C_OK) {
            addReplyError(c, "min or max is not a float");
            return;
        }
    } else if (rangetype == ZRANGE_LEX) {
        notify_type = "zremrangebylex";
        if (zsetParseLexRange(c->argv[2], c->argv[3], &lexrange) != C_OK) {
            addReplyError(c, "min or max not valid string range item");
            return;
        }
    } else {
        serverPanic("unknown rangetype %d", (int)rangetype);
    }

    /* Step 2: Lookup & range sanity checks if needed. */
    if ((zobj = lookupKeyWriteOrReply(c, key, shared.czero)) == NULL || checkType(c, zobj, OBJ_ZSET)) goto cleanup;

    if (rangetype == ZRANGE_RANK) {
        /* Sanitize indexes. */
        llen = zsetLength(zobj);
        if (start < 0) start = llen + start;
        if (end < 0) end = llen + end;
        if (start < 0) start = 0;

        /* Invariant: start >= 0, so this test will be true when end < 0.
         * The range is empty when start > end or start >= length. */
        if (start > end || start >= llen) {
            addReply(c, shared.czero);
            goto cleanup;
        }
        if (end >= llen) end = llen - 1;
    }

    /* Step 3: Perform the range deletion operation. */
    if (zobj->encoding == OBJ_ENCODING_LISTPACK) {
        switch (rangetype) {
        case ZRANGE_AUTO:
        case ZRANGE_RANK: objectSetVal(zobj, zzlDeleteRangeByRank(objectGetVal(zobj), start + 1, end + 1, &deleted)); break;
        case ZRANGE_SCORE: objectSetVal(zobj, zzlDeleteRangeByScore(objectGetVal(zobj), &range, &deleted)); break;
        case ZRANGE_LEX: objectSetVal(zobj, zzlDeleteRangeByLex(objectGetVal(zobj), &lexrange, &deleted)); break;
        }
        if (zzlLength(objectGetVal(zobj)) == 0) {
            dbDelete(c->db, key);
            keyremoved = 1;
        }
    } else if (zobj->encoding == OBJ_ENCODING_BTREE) {
        zset *zs = objectGetVal(zobj);
        hashtablePauseAutoShrink(zs->ht);
        switch (rangetype) {
        case ZRANGE_AUTO:
        case ZRANGE_RANK: deleted = orderedIndexDeleteRangeByIndex(zs->oi, start, end, zsetIndexDeleteCallback, zs->ht); break;
        case ZRANGE_SCORE: deleted = orderedIndexDeleteRangeByScore(zs->oi, range.min, range.max, range.minex, range.maxex, zsetIndexDeleteCallback, zs->ht); break;
        case ZRANGE_LEX: deleted = orderedIndexDeleteRangeByLex(zs->oi, lexrange.min, lexrange.max, lexrange.minex, lexrange.maxex, zsetIndexDeleteCallback, zs->ht); break;
        }
        hashtableResumeAutoShrink(zs->ht);
        if (hashtableSize(zs->ht) == 0) {
            dbDelete(c->db, key);
            keyremoved = 1;
        }
    } else {
        serverPanic("Unknown sorted set encoding");
    }

    /* Step 4: Notifications and reply. */
    if (deleted) {
        signalModifiedKey(c, c->db, key);
        notifyKeyspaceEvent(NOTIFY_ZSET, notify_type, key, c->db->id);
        if (keyremoved) notifyKeyspaceEvent(NOTIFY_GENERIC, "del", key, c->db->id);
        server.dirty += deleted;
    }
    addReplyLongLong(c, deleted);

cleanup:
    if (rangetype == ZRANGE_LEX) zsetFreeLexRange(&lexrange);
}

void zremrangebyrankCommand(client *c) {
    zremrangeGenericCommand(c, ZRANGE_RANK);
}

void zremrangebyscoreCommand(client *c) {
    zremrangeGenericCommand(c, ZRANGE_SCORE);
}

void zremrangebylexCommand(client *c) {
    zremrangeGenericCommand(c, ZRANGE_LEX);
}

typedef struct {
    robj *subject;
    int type; /* Set, sorted set */
    int encoding;
    double weight;

    union {
        /* Set iterators. */
        union _iterset {
            struct {
                intset *is;
                int ii;
            } is;
            struct {
                hashtableIterator *iter;
            } ht;
            struct {
                unsigned char *lp;
                unsigned char *p;
            } lp;
        } set;

        /* Sorted set iterators. */
        union _iterzset {
            struct {
                unsigned char *zl;
                unsigned char *eptr, *sptr;
            } zl;
            struct {
                zset *zs;
                OrderedIndexIterator iter;
                OrderedIndexItem *node;
            } sl;
        } zset;
    } iter;
} zsetopsrc;


/* Use dirty flags for pointers that need to be cleaned up in the next
 * iteration over the zsetopval. The dirty flag for the long long value is
 * special, since long long values don't need cleanup. Instead, it means that
 * we already checked that "ell" holds a long long, or tried to convert another
 * representation into a long long value. When this was successful,
 * OPVAL_VALID_LL is set as well. */
#define OPVAL_DIRTY_SDS 1
#define OPVAL_DIRTY_LL 2
#define OPVAL_VALID_LL 4

/* Store value retrieved from the iterator. */
typedef struct {
    int flags;
    unsigned char _buf[32]; /* Private buffer. */
    sds ele;
    unsigned char *estr;
    unsigned int elen;
    long long ell;
    double score;
} zsetopval;

typedef union _iterset iterset;
typedef union _iterzset iterzset;

static void zuiInitIterator(zsetopsrc *op) {
    if (op->subject == NULL) return;

    if (op->type == OBJ_SET) {
        iterset *it = &op->iter.set;
        if (op->encoding == OBJ_ENCODING_INTSET) {
            it->is.is = objectGetVal(op->subject);
            it->is.ii = 0;
        } else if (op->encoding == OBJ_ENCODING_HASHTABLE) {
            it->ht.iter = hashtableCreateIterator(objectGetVal(op->subject), 0);
        } else if (op->encoding == OBJ_ENCODING_LISTPACK) {
            it->lp.lp = objectGetVal(op->subject);
            it->lp.p = lpFirst(it->lp.lp);
        } else {
            serverPanic("Unknown set encoding");
        }
    } else if (op->type == OBJ_ZSET) {
        /* Sorted sets are traversed in reverse order to optimize for
         * the insertion of the elements in a new list as in
         * ZDIFF/ZINTER/ZUNION */
        iterzset *it = &op->iter.zset;
        if (op->encoding == OBJ_ENCODING_LISTPACK) {
            it->zl.zl = objectGetVal(op->subject);
            it->zl.eptr = lpSeek(it->zl.zl, -2);
            if (it->zl.eptr != NULL) {
                it->zl.sptr = lpNext(it->zl.zl, it->zl.eptr);
                serverAssert(it->zl.sptr != NULL);
            }
        } else if (op->encoding == OBJ_ENCODING_BTREE) {
            it->sl.zs = objectGetVal(op->subject);
            orderedIndexInitIterator(&it->sl.iter, it->sl.zs->oi);
            it->sl.node = NULL;
        } else {
            serverPanic("Unknown sorted set encoding");
        }
    } else {
        serverPanic("Unsupported type");
    }
}

static void zuiClearIterator(zsetopsrc *op) {
    if (op->subject == NULL) return;

    if (op->type == OBJ_SET) {
        iterset *it = &op->iter.set;
        if (op->encoding == OBJ_ENCODING_INTSET) {
            UNUSED(it); /* skip */
        } else if (op->encoding == OBJ_ENCODING_HASHTABLE) {
            hashtableReleaseIterator(it->ht.iter);
        } else if (op->encoding == OBJ_ENCODING_LISTPACK) {
            UNUSED(it);
        } else {
            serverPanic("Unknown set encoding");
        }
    } else if (op->type == OBJ_ZSET) {
        iterzset *it = &op->iter.zset;
        if (op->encoding == OBJ_ENCODING_LISTPACK) {
            UNUSED(it); /* skip */
        } else if (op->encoding == OBJ_ENCODING_BTREE) {
            UNUSED(it); /* skip */
        } else {
            serverPanic("Unknown sorted set encoding");
        }
    } else {
        serverPanic("Unsupported type");
    }
}

static void zuiDiscardDirtyValue(zsetopval *val) {
    if (val->flags & OPVAL_DIRTY_SDS) {
        sdsfree(val->ele);
        val->ele = NULL;
        val->flags &= ~OPVAL_DIRTY_SDS;
    }
}

static unsigned long zuiLength(zsetopsrc *op) {
    if (op->subject == NULL) return 0;

    if (op->type == OBJ_SET) {
        return setTypeSize(op->subject);
    } else if (op->type == OBJ_ZSET) {
        if (op->encoding == OBJ_ENCODING_LISTPACK) {
            return zzlLength(objectGetVal(op->subject));
        } else if (op->encoding == OBJ_ENCODING_BTREE) {
            zset *zs = objectGetVal(op->subject);
            return orderedIndexLength(zs->oi);
        } else {
            serverPanic("Unknown sorted set encoding");
        }
    } else {
        serverPanic("Unsupported type");
    }
}

/* Check if the current value is valid. If so, store it in the passed structure
 * and move to the next element. If not valid, this means we have reached the
 * end of the structure and can abort. */
static int zuiNext(zsetopsrc *op, zsetopval *val) {
    if (op->subject == NULL) return 0;

    zuiDiscardDirtyValue(val);

    memset(val, 0, sizeof(zsetopval));

    if (op->type == OBJ_SET) {
        iterset *it = &op->iter.set;
        if (op->encoding == OBJ_ENCODING_INTSET) {
            int64_t ell;

            if (!intsetGet(it->is.is, it->is.ii, &ell)) return 0;
            val->ell = ell;
            val->score = 1.0;

            /* Move to next element. */
            it->is.ii++;
        } else if (op->encoding == OBJ_ENCODING_HASHTABLE) {
            void *next;
            if (!hashtableNext(it->ht.iter, &next)) return 0;
            val->ele = next;
            val->score = 1.0;
        } else if (op->encoding == OBJ_ENCODING_LISTPACK) {
            if (it->lp.p == NULL) return 0;
            val->estr = lpGetValue(it->lp.p, &val->elen, &val->ell);
            val->score = 1.0;

            /* Move to next element. */
            it->lp.p = lpNext(it->lp.lp, it->lp.p);
        } else {
            serverPanic("Unknown set encoding");
        }
    } else if (op->type == OBJ_ZSET) {
        iterzset *it = &op->iter.zset;
        if (op->encoding == OBJ_ENCODING_LISTPACK) {
            /* No need to check both, but better be explicit. */
            if (it->zl.eptr == NULL || it->zl.sptr == NULL) return 0;
            val->estr = lpGetValue(it->zl.eptr, &val->elen, &val->ell);
            val->score = zzlGetScore(it->zl.sptr);

            /* Move to next element (going backwards, see zuiInitIterator). */
            zzlPrev(it->zl.zl, &it->zl.eptr, &it->zl.sptr);
        } else if (op->encoding == OBJ_ENCODING_BTREE) {
            it->sl.node = orderedIndexPrev(&it->sl.iter);
            if (it->sl.node == NULL) return 0;
            const char *val_ele_ptr;
            size_t val_ele_len;
            orderedIndexItemGetElement(it->sl.node, &val_ele_ptr, &val_ele_len);
            val->estr = (unsigned char *)val_ele_ptr;
            val->elen = val_ele_len;
            val->score = orderedIndexItemGetScore(it->sl.node);
        } else {
            serverPanic("Unknown sorted set encoding");
        }
    } else {
        serverPanic("Unsupported type");
    }
    return 1;
}

static sds zuiSdsFromValue(zsetopval *val) {
    if (val->ele == NULL) {
        if (val->estr != NULL) {
            val->ele = sdsnewlen((char *)val->estr, val->elen);
        } else {
            val->ele = sdsfromlonglong(val->ell);
        }
        val->flags |= OPVAL_DIRTY_SDS;
    }
    return val->ele;
}

/* This is different from zuiSdsFromValue since returns a new SDS string
 * which is up to the caller to free. */
static sds zuiNewSdsFromValue(zsetopval *val) {
    if (val->flags & OPVAL_DIRTY_SDS) {
        /* We have already one to return! */
        sds ele = val->ele;
        val->flags &= ~OPVAL_DIRTY_SDS;
        val->ele = NULL;
        return ele;
    } else if (val->ele) {
        return sdsdup(val->ele);
    } else if (val->estr) {
        return sdsnewlen((char *)val->estr, val->elen);
    } else {
        return sdsfromlonglong(val->ell);
    }
}

/* Find value pointed to by val in the source pointer to by op. When found,
 * return 1 and store its score in target. Return 0 otherwise. */
static int zuiFind(zsetopsrc *op, zsetopval *val, double *score) {
    if (op->subject == NULL) return 0;

    if (op->type == OBJ_SET) {
        char *str = val->ele ? val->ele : (char *)val->estr;
        size_t len = val->ele ? sdslen(val->ele) : val->elen;
        if (setTypeIsMemberAux(op->subject, str, len, val->ell, val->ele != NULL)) {
            *score = 1.0;
            return 1;
        } else {
            return 0;
        }
    } else if (op->type == OBJ_ZSET) {
        zuiSdsFromValue(val);

        if (op->encoding == OBJ_ENCODING_LISTPACK) {
            if (zzlFind(objectGetVal(op->subject), val->ele, score) != NULL) {
                /* Score is already set by zzlFind. */
                return 1;
            } else {
                return 0;
            }
        } else if (op->encoding == OBJ_ENCODING_BTREE) {
            zset *zs = objectGetVal(op->subject);
            void *entry;
            zsetMarkLookupKey(val->ele);
            int found = hashtableFind(zs->ht, val->ele, &entry);
            zsetUnmarkLookupKey(val->ele);
            if (found) {
                OrderedIndexItem *node = entry;
                *score = orderedIndexItemGetScore(node);
                return 1;
            } else {
                return 0;
            }
        } else {
            serverPanic("Unknown sorted set encoding");
        }
    } else {
        serverPanic("Unsupported type");
    }
}

static int zuiCompareByCardinality(const void *s1, const void *s2) {
    unsigned long first = zuiLength((zsetopsrc *)s1);
    unsigned long second = zuiLength((zsetopsrc *)s2);
    if (first > second) return 1;
    if (first < second) return -1;
    return 0;
}

static int zuiCompareByRevCardinality(const void *s1, const void *s2) {
    return zuiCompareByCardinality(s1, s2) * -1;
}

#define REDIS_AGGR_SUM 1
#define REDIS_AGGR_MIN 2
#define REDIS_AGGR_MAX 3
#define zunionInterDictValue(_e) (dictGetVal(_e) == NULL ? 1.0 : *(double *)dictGetVal(_e))

inline static void zunionInterAggregate(double *target, double val, int aggregate) {
    if (aggregate == REDIS_AGGR_SUM) {
        *target = *target + val;
        /* The result of adding two doubles is NaN when one variable
         * is +inf and the other is -inf. When these numbers are added,
         * we maintain the convention of the result being 0.0. */
        if (isnan(*target)) *target = 0.0;
    } else if (aggregate == REDIS_AGGR_MIN) {
        *target = val < *target ? val : *target;
    } else if (aggregate == REDIS_AGGR_MAX) {
        *target = val > *target ? val : *target;
    } else {
        /* safety net */
        serverPanic("Unknown ZUNION/INTER aggregate type");
    }
}

static size_t zsetHashtableGetMaxElementLength(hashtable *ht, size_t *totallen) {
    size_t maxelelen = 0;

    hashtableIterator iter;
    hashtableInitIterator(&iter, ht, 0);
    void *next;
    while (hashtableNext(&iter, &next)) {
        OrderedIndexItem *node = next;
        const char *ele_ptr_tmp;
        size_t ele_len_tmp;
        orderedIndexItemGetElement(node, &ele_ptr_tmp, &ele_len_tmp);
        if (ele_len_tmp > maxelelen) maxelelen = ele_len_tmp;
        if (totallen) (*totallen) += ele_len_tmp;
    }
    hashtableCleanupIterator(&iter);

    return maxelelen;
}

static void zdiffAlgorithm1(zsetopsrc *src, long setnum, zset *dstzset, size_t *maxelelen, size_t *totelelen) {
    /* DIFF Algorithm 1:
     *
     * We perform the diff by iterating all the elements of the first set,
     * and only adding it to the target set if the element does not exist
     * into all the other sets.
     *
     * This way we perform at max N*M operations, where N is the size of
     * the first set, and M the number of sets.
     *
     * There is also a O(K*log(K)) cost for adding the resulting elements
     * to the target set, where K is the final size of the target set.
     *
     * The final complexity of this algorithm is O(N*M + K*log(K)). */
    int j;
    zsetopval zval;
    OrderedIndexItem *znode;
    sds tmp;

    /* With algorithm 1 it is better to order the sets to subtract
     * by decreasing size, so that we are more likely to find
     * duplicated elements ASAP. */
    qsort(src + 1, setnum - 1, sizeof(zsetopsrc), zuiCompareByRevCardinality);

    memset(&zval, 0, sizeof(zval));
    zuiInitIterator(&src[0]);
    while (zuiNext(&src[0], &zval)) {
        double value;
        int exists = 0;

        for (j = 1; j < setnum; j++) {
            /* It is not safe to access the zset we are
             * iterating, so explicitly check for equal object.
             * This check isn't really needed anymore since we already
             * check for a duplicate set in the zsetChooseDiffAlgorithm
             * function, but we're leaving it for future-proofing. */
            if (src[j].subject == src[0].subject || zuiFind(&src[j], &zval, &value)) {
                exists = 1;
                break;
            }
        }

        if (!exists) {
            tmp = zuiNewSdsFromValue(&zval);
            znode = orderedIndexInsert(dstzset->oi, zval.score, tmp, sdslen(tmp));
            hashtableAdd(dstzset->ht, znode);
            if (sdslen(tmp) > *maxelelen) *maxelelen = sdslen(tmp);
            (*totelelen) += sdslen(tmp);
            sdsfree(tmp);
        }
    }
    zuiClearIterator(&src[0]);
}


static void zdiffAlgorithm2(zsetopsrc *src, long setnum, zset *dstzset, size_t *maxelelen, size_t *totelelen) {
    /* DIFF Algorithm 2:
     *
     * Add all the elements of the first set to the auxiliary set.
     * Then remove all the elements of all the next sets from it.
     *

     * This is O(L + (N-K)log(N)) where L is the sum of all the elements in every
     * set, N is the size of the first set, and K is the size of the result set.
     *
     * Note that from the (L-N) dict searches, (N-K) got to the zsetRemoveFromIndex
     * which costs log(N)
     *
     * There is also a O(K) cost at the end for finding the largest element
     * size, but this doesn't change the algorithm complexity since K < L, and
     * O(2L) is the same as O(L). */
    int j;
    int cardinality = 0;
    zsetopval zval;
    OrderedIndexItem *znode;
    sds tmp;

    hashtablePauseAutoShrink(dstzset->ht);
    for (j = 0; j < setnum; j++) {
        if (zuiLength(&src[j]) == 0) continue;

        memset(&zval, 0, sizeof(zval));
        zuiInitIterator(&src[j]);
        while (zuiNext(&src[j], &zval)) {
            if (j == 0) {
                tmp = zuiNewSdsFromValue(&zval);
                znode = orderedIndexInsert(dstzset->oi, zval.score, tmp, sdslen(tmp));
                sdsfree(tmp);
                hashtableAdd(dstzset->ht, znode);
                cardinality++;
            } else {
                tmp = zuiSdsFromValue(&zval);
                if (zsetRemoveFromIndex(dstzset, tmp)) {
                    cardinality--;
                }
            }

            /* Exit if result set is empty as any additional removal
             * of elements will have no effect. */
            if (cardinality == 0) {
                zuiDiscardDirtyValue(&zval);
                break;
            }
        }
        zuiClearIterator(&src[j]);

        if (cardinality == 0) break;
    }

    /* Resize dict if needed after removing multiple elements */
    hashtableResumeAutoShrink(dstzset->ht);

    /* Using this algorithm, we can't calculate the max element as we go,
     * we have to iterate through all elements to find the max one after. */
    *maxelelen = zsetHashtableGetMaxElementLength(dstzset->ht, totelelen);
}

static int zsetChooseDiffAlgorithm(zsetopsrc *src, long setnum) {
    /* Select what DIFF algorithm to use.
     *
     * Algorithm 1 is O(N*M + K*log(K)) where N is the size of the
     * first set, M the total number of sets, and K is the size of the
     * result set.
     *
     * Algorithm 2 is O(L + (N-K)log(N)) where L is the total number of elements
     * in all the sets, N is the size of the first set, and K is the size of the
     * result set.
     *
     * We compute what is the best bet with the current input here. */
    long long algo_one_work = 0;
    long long algo_two_work = 0;

    for (int j = 0; j < setnum; j++) {
        /* If any other set is equal to the first set, there is nothing to be
         * done, since we would remove all elements anyway. */
        if (j > 0 && src[0].subject == src[j].subject) {
            return 0;
        }

        algo_one_work += zuiLength(&src[0]);
        algo_two_work += zuiLength(&src[j]);
    }

    /* Algorithm 1 has better constant times and performs less operations
     * if there are elements in common. Give it some advantage. */
    algo_one_work /= 2;
    return (algo_one_work <= algo_two_work) ? 1 : 2;
}

/* The zdiff() function is called to specifically handle ZDIFF, ZDIFFSTORE commands.
 * It computes the difference between the first and all successive input sorted sets.
 * Meaning, if the first key is empty, we cannot reduce further from an already empty collection,
 * and thus zdiff() becomes a no-op. */
static void zdiff(zsetopsrc *src, long setnum, zset *dstzset, size_t *maxelelen, size_t *totelelen) {
    /* Skip everything if the smallest input is empty. */
    if (zuiLength(&src[0]) > 0) {
        int diff_algo = zsetChooseDiffAlgorithm(src, setnum);
        if (diff_algo == 1) {
            zdiffAlgorithm1(src, setnum, dstzset, maxelelen, totelelen);
        } else if (diff_algo == 2) {
            zdiffAlgorithm2(src, setnum, dstzset, maxelelen, totelelen);
        } else if (diff_algo != 0) {
            serverPanic("Unknown algorithm");
        }
    }
}

/* The zunionInterDiffGenericCommand() function is called in order to implement the
 * following commands: ZUNION, ZINTER, ZDIFF, ZUNIONSTORE, ZINTERSTORE, ZDIFFSTORE,
 * ZINTERCARD.
 *
 * 'numkeysIndex' parameter position of key number. for ZUNION/ZINTER/ZDIFF command,
 * this value is 1, for ZUNIONSTORE/ZINTERSTORE/ZDIFFSTORE command, this value is 2.
 *
 * 'op' SET_OP_INTER, SET_OP_UNION or SET_OP_DIFF.
 *
 * 'cardinality_only' is currently only applicable when 'op' is SET_OP_INTER.
 * Work for SINTERCARD, only return the cardinality with minimum processing and memory overheads.
 */
static void zunionInterDiffGenericCommand(client *c, robj *dstkey, int numkeysIndex, int op, int cardinality_only) {
    int i, j;
    long setnum;
    int aggregate = REDIS_AGGR_SUM;
    zsetopsrc *src;
    zsetopval zval;
    sds tmp;
    size_t maxelelen = 0, totelelen = 0;
    robj *dstobj = NULL;
    zset *dstzset = NULL;
    int withscores = 0;
    unsigned long cardinality = 0;
    long limit = 0; /* Stop searching after reaching the limit. 0 means unlimited. */

    /* expect setnum input keys to be given */
    if ((getLongFromObjectOrReply(c, c->argv[numkeysIndex], &setnum, NULL) != C_OK)) return;

    if (setnum < 1) {
        addReplyErrorFormat(c, "at least 1 input key is needed for '%s' command", c->cmd->fullname);
        return;
    }

    /* test if the expected number of keys would overflow */
    if (setnum > (c->argc - (numkeysIndex + 1))) {
        addReplyErrorObject(c, shared.syntaxerr);
        return;
    }

    /* Try to allocate the src table, and abort on insufficient memory. */
    src = ztrycalloc(sizeof(zsetopsrc) * setnum);
    if (src == NULL) {
        addReplyError(c, "Insufficient memory, failed allocating transient memory, too many args.");
        return;
    }

    /* read keys to be used for input */
    for (i = 0, j = numkeysIndex + 1; i < setnum; i++, j++) {
        robj *obj = lookupKeyRead(c->db, c->argv[j]);
        if (obj != NULL) {
            if (obj->type != OBJ_ZSET && obj->type != OBJ_SET) {
                zfree(src);
                addReplyErrorObject(c, shared.wrongtypeerr);
                return;
            }

            src[i].subject = obj;
            src[i].type = obj->type;
            src[i].encoding = obj->encoding;
        } else {
            src[i].subject = NULL;
        }

        /* Default all weights to 1. */
        src[i].weight = 1.0;
    }

    /* parse optional extra arguments */
    if (j < c->argc) {
        int remaining = c->argc - j;

        while (remaining) {
            if (op != SET_OP_DIFF && !cardinality_only && remaining >= (setnum + 1) &&
                !strcasecmp(objectGetVal(c->argv[j]), "weights")) {
                j++;
                remaining--;
                for (i = 0; i < setnum; i++, j++, remaining--) {
                    if (getDoubleFromObjectOrReply(c, c->argv[j], &src[i].weight, "weight value is not a float") !=
                        C_OK) {
                        zfree(src);
                        return;
                    }
                }
            } else if (op != SET_OP_DIFF && !cardinality_only && remaining >= 2 &&
                       !strcasecmp(objectGetVal(c->argv[j]), "aggregate")) {
                j++;
                remaining--;
                if (!strcasecmp(objectGetVal(c->argv[j]), "sum")) {
                    aggregate = REDIS_AGGR_SUM;
                } else if (!strcasecmp(objectGetVal(c->argv[j]), "min")) {
                    aggregate = REDIS_AGGR_MIN;
                } else if (!strcasecmp(objectGetVal(c->argv[j]), "max")) {
                    aggregate = REDIS_AGGR_MAX;
                } else {
                    zfree(src);
                    addReplyErrorObject(c, shared.syntaxerr);
                    return;
                }
                j++;
                remaining--;
            } else if (remaining >= 1 && !dstkey && !cardinality_only && !strcasecmp(objectGetVal(c->argv[j]), "withscores")) {
                j++;
                remaining--;
                withscores = 1;
            } else if (cardinality_only && remaining >= 2 && !strcasecmp(objectGetVal(c->argv[j]), "limit")) {
                j++;
                remaining--;
                if (getPositiveLongFromObjectOrReply(c, c->argv[j], &limit, "LIMIT can't be negative") != C_OK) {
                    zfree(src);
                    return;
                }
                j++;
                remaining--;
            } else {
                zfree(src);
                addReplyErrorObject(c, shared.syntaxerr);
                return;
            }
        }
    }

    if (op != SET_OP_DIFF) {
        /* sort sets from the smallest to largest, this will improve our
         * algorithm's performance */
        qsort(src, setnum, sizeof(zsetopsrc), zuiCompareByCardinality);
    }

    /* We need a temp zset object to store our union/inter/diff. If the dstkey
     * is not NULL (that is, we are inside an ZUNIONSTORE/ZINTERSTORE/ZDIFFSTORE operation) then
     * this zset object will be the resulting object to zset into the target key.
     * In SINTERCARD case, we don't need the temp obj, so we can avoid creating it. */
    if (!cardinality_only) {
        dstobj = createZsetObject();
        dstzset = objectGetVal(dstobj);
    }
    memset(&zval, 0, sizeof(zval));

    if (op == SET_OP_INTER) {
        /* Skip everything if the smallest input is empty. */
        if (zuiLength(&src[0]) > 0) {
            /* Precondition: as src[0] is non-empty and the inputs are ordered
             * by size, all src[i > 0] are non-empty too. */
            zuiInitIterator(&src[0]);
            while (zuiNext(&src[0], &zval)) {
                double score, value;

                score = src[0].weight * zval.score;
                if (isnan(score)) score = 0;

                for (j = 1; j < setnum; j++) {
                    /* It is not safe to access the zset we are
                     * iterating, so explicitly check for equal object. */
                    if (src[j].subject == src[0].subject) {
                        value = zval.score * src[j].weight;
                        zunionInterAggregate(&score, value, aggregate);
                    } else if (zuiFind(&src[j], &zval, &value)) {
                        value *= src[j].weight;
                        zunionInterAggregate(&score, value, aggregate);
                    } else {
                        break;
                    }
                }

                /* Only continue when present in every input. */
                if (j == setnum && cardinality_only) {
                    cardinality++;

                    /* We stop the searching after reaching the limit. */
                    if (limit && cardinality >= (unsigned long)limit) {
                        /* Cleanup before we break the zuiNext loop. */
                        zuiDiscardDirtyValue(&zval);
                        break;
                    }
                } else if (j == setnum) {
                    tmp = zuiNewSdsFromValue(&zval);
                    OrderedIndexItem *znode = orderedIndexInsert(dstzset->oi, score, tmp, sdslen(tmp));
                    hashtableAdd(dstzset->ht, znode);
                    totelelen += sdslen(tmp);
                    if (sdslen(tmp) > maxelelen) maxelelen = sdslen(tmp);
                    sdsfree(tmp);
                }
            }
            zuiClearIterator(&src[0]);
        }
    } else if (op == SET_OP_UNION) {
        /* Step 1: Create the hash table first by iterating one sorted set after
         * the other. We wait to create the ordered index until scores/ordering
         * are finalized. */
        if (setnum) {
            /* Our union is at least as large as the largest set.
             * Resize the dictionary ASAP to avoid useless rehashing. */
            hashtableExpand(dstzset->ht, zuiLength(&src[setnum - 1]));
        }
        for (i = 0; i < setnum; i++) {
            if (zuiLength(&src[i]) == 0) continue;

            zuiInitIterator(&src[i]);
            while (zuiNext(&src[i], &zval)) {
                /* Initialize value */
                double score = src[i].weight * zval.score;
                if (isnan(score)) score = 0;

                /* Search for this element in the accumulating dictionary. */
                sds sdsval = zuiSdsFromValue(&zval);
                hashtablePosition position;
                /* If we don't have it, we need to create a new entry. */
                void *existing;
                zsetMarkLookupKey(sdsval);
                int is_new = hashtableFindPositionForInsert(dstzset->ht, sdsval, &position, &existing);
                zsetUnmarkLookupKey(sdsval);
                if (is_new) {
                    sds tmp_ele = zuiNewSdsFromValue(&zval);
                    OrderedIndexItem *new_node = orderedIndexItemCreate(score, tmp_ele, sdslen(tmp_ele));
                    sdsfree(tmp_ele);
                    hashtableInsertAtPosition(dstzset->ht, new_node, &position);
                    /* Remember the longest single element encountered,
                     * to understand if it's possible to convert to listpack
                     * at the end. */
                    const char *ele_ptr_tmp;
                    size_t ele_len_tmp;
                    orderedIndexItemGetElement(new_node, &ele_ptr_tmp, &ele_len_tmp);
                    totelelen += ele_len_tmp;
                    if (ele_len_tmp > maxelelen) {
                        maxelelen = ele_len_tmp;
                    }
                } else {
                    /* Update the score with the score of the new instance
                     * of the element found in the current sorted set. */
                    OrderedIndexItem *node = existing;
                    double cur = orderedIndexItemGetScore(node);
                    zunionInterAggregate(&cur, score, aggregate);
                    orderedIndexItemSetScore(node, cur);
                }
            }
            zuiClearIterator(&src[i]);
        }

        /* Step 2: Insert all detached items into the ordered index */
        hashtableIterator iter;
        hashtableInitIterator(&iter, dstzset->ht, 0);

        void *next;
        while (hashtableNext(&iter, &next)) {
            OrderedIndexItem *node = next;
            orderedIndexInsertItem(dstzset->oi, node);
        }
        hashtableCleanupIterator(&iter);
    } else if (op == SET_OP_DIFF) {
        zdiff(src, setnum, dstzset, &maxelelen, &totelelen);
    } else {
        serverPanic("Unknown operator");
    }

    if (dstkey) {
        if (orderedIndexLength(dstzset->oi)) {
            zsetConvertToListpackIfNeeded(dstobj, maxelelen, totelelen);
            setKey(c, c->db, dstkey, &dstobj, 0);
            notifyKeyspaceEvent(NOTIFY_ZSET, (op == SET_OP_UNION) ? "zunionstore" : (op == SET_OP_INTER ? "zinterstore" : "zdiffstore"),
                                dstkey, c->db->id);
            addReplyLongLong(c, zsetLength(dstobj));
            server.dirty++;
        } else {
            if (dbDelete(c->db, dstkey)) {
                signalModifiedKey(c, c->db, dstkey);
                notifyKeyspaceEvent(NOTIFY_GENERIC, "del", dstkey, c->db->id);
                server.dirty++;
            }
            addReply(c, shared.czero);
            decrRefCount(dstobj);
        }
    } else if (cardinality_only) {
        addReplyLongLong(c, cardinality);
    } else {
        unsigned long length = orderedIndexLength(dstzset->oi);
        OrderedIndex *oi = dstzset->oi;
        OrderedIndexIterator iter;
        orderedIndexInitIterator(&iter, oi);
        /* In case of WITHSCORES, respond with a single array in RESP2, and
         * nested arrays in RESP3. We can't use a map response type since the
         * client library needs to know to respect the order. */
        if (withscores && c->resp == 2)
            addReplyArrayLen(c, length * 2);
        else
            addReplyArrayLen(c, length);

        OrderedIndexItem *zn;
        while ((zn = orderedIndexNext(&iter)) != NULL) {
            if (withscores && c->resp > 2) addReplyArrayLen(c, 2);
            const char *ele_ptr;
            size_t ele_len;
            orderedIndexItemGetElement(zn, &ele_ptr, &ele_len);
            addReplyBulkCBuffer(c, ele_ptr, ele_len);
            if (withscores) addReplyDouble(c, orderedIndexItemGetScore(zn));
        }
        server.lazyfree_lazy_server_del ? freeObjAsync(NULL, dstobj, -1) : decrRefCount(dstobj);
    }
    zfree(src);
}

/* ZUNIONSTORE destination numkeys key [key ...] [WEIGHTS weight] [AGGREGATE SUM|MIN|MAX] */
void zunionstoreCommand(client *c) {
    zunionInterDiffGenericCommand(c, c->argv[1], 2, SET_OP_UNION, 0);
}

/* ZINTERSTORE destination numkeys key [key ...] [WEIGHTS weight] [AGGREGATE SUM|MIN|MAX] */
void zinterstoreCommand(client *c) {
    zunionInterDiffGenericCommand(c, c->argv[1], 2, SET_OP_INTER, 0);
}

/* ZDIFFSTORE destination numkeys key [key ...] */
void zdiffstoreCommand(client *c) {
    zunionInterDiffGenericCommand(c, c->argv[1], 2, SET_OP_DIFF, 0);
}

/* ZUNION numkeys key [key ...] [WEIGHTS weight] [AGGREGATE SUM|MIN|MAX] [WITHSCORES] */
void zunionCommand(client *c) {
    zunionInterDiffGenericCommand(c, NULL, 1, SET_OP_UNION, 0);
}

/* ZINTER numkeys key [key ...] [WEIGHTS weight] [AGGREGATE SUM|MIN|MAX] [WITHSCORES] */
void zinterCommand(client *c) {
    zunionInterDiffGenericCommand(c, NULL, 1, SET_OP_INTER, 0);
}

/* ZINTERCARD numkeys key [key ...] [LIMIT limit] */
void zinterCardCommand(client *c) {
    zunionInterDiffGenericCommand(c, NULL, 1, SET_OP_INTER, 1);
}

/* ZDIFF numkeys key [key ...] [WITHSCORES] */
void zdiffCommand(client *c) {
    zunionInterDiffGenericCommand(c, NULL, 1, SET_OP_DIFF, 0);
}

typedef enum {
    ZRANGE_DIRECTION_AUTO = 0,
    ZRANGE_DIRECTION_FORWARD,
    ZRANGE_DIRECTION_REVERSE
} zrange_direction;

typedef enum {
    ZRANGE_CONSUMER_TYPE_CLIENT = 0,
    ZRANGE_CONSUMER_TYPE_INTERNAL
} zrange_consumer_type;

typedef struct zrange_result_handler zrange_result_handler;

typedef void (*zrangeResultBeginFunction)(zrange_result_handler *c, long length);
typedef void (*zrangeResultFinalizeFunction)(zrange_result_handler *c, size_t result_count);
typedef void (*zrangeResultEmitCBufferFunction)(zrange_result_handler *c, const void *p, size_t len, double score);
typedef void (*zrangeResultEmitLongLongFunction)(zrange_result_handler *c, long long ll, double score);

void zrangeGenericCommand(zrange_result_handler *handler,
                          int argc_start,
                          int store,
                          zrange_type rangetype,
                          zrange_direction direction);

/* Interface struct for ZRANGE/ZRANGESTORE generic implementation.
 * There is one implementation of this interface that sends a RESP reply to clients.
 * and one implementation that stores the range result into a zset object. */
struct zrange_result_handler {
    zrange_consumer_type type;
    client *client;
    robj *dstkey;
    robj *dstobj;
    void *userdata;
    int withscores;
    int should_emit_array_length;
    zrangeResultBeginFunction beginResultEmission;
    zrangeResultFinalizeFunction finalizeResultEmission;
    zrangeResultEmitCBufferFunction emitResultFromCBuffer;
    zrangeResultEmitLongLongFunction emitResultFromLongLong;
};

/* Result handler methods for responding the ZRANGE to clients.
 * length can be used to provide the result length in advance (avoids deferred reply overhead).
 * length can be set to -1 if the result length is not know in advance.
 */
static void zrangeResultBeginClient(zrange_result_handler *handler, long length) {
    if (length > 0) {
        /* In case of WITHSCORES, respond with a single array in RESP2, and
         * nested arrays in RESP3. We can't use a map response type since the
         * client library needs to know to respect the order. */
        if (handler->withscores && (handler->client->resp == 2)) {
            length *= 2;
        }
        addReplyArrayLen(handler->client, length);
        handler->userdata = NULL;
        return;
    }
    handler->userdata = addReplyDeferredLen(handler->client);
}

static void zrangeResultEmitCBufferToClient(zrange_result_handler *handler,
                                            const void *value,
                                            size_t value_length_in_bytes,
                                            double score) {
    if (handler->should_emit_array_length) {
        addReplyArrayLen(handler->client, 2);
    }

    addReplyBulkCBuffer(handler->client, value, value_length_in_bytes);

    if (handler->withscores) {
        addReplyDouble(handler->client, score);
    }
}

static void zrangeResultEmitLongLongToClient(zrange_result_handler *handler, long long value, double score) {
    if (handler->should_emit_array_length) {
        addReplyArrayLen(handler->client, 2);
    }

    addReplyBulkLongLong(handler->client, value);

    if (handler->withscores) {
        addReplyDouble(handler->client, score);
    }
}

static void zrangeResultFinalizeClient(zrange_result_handler *handler, size_t result_count) {
    /* If the reply size was know at start there's nothing left to do */
    if (!handler->userdata) return;
    /* In case of WITHSCORES, respond with a single array in RESP2, and
     * nested arrays in RESP3. We can't use a map response type since the
     * client library needs to know to respect the order. */
    if (handler->withscores && (handler->client->resp == 2)) {
        result_count *= 2;
    }

    setDeferredArrayLen(handler->client, handler->userdata, result_count);
}

/* Result handler methods for storing the ZRANGESTORE to a zset. */
static void zrangeResultBeginStore(zrange_result_handler *handler, long length) {
    handler->dstobj = zsetTypeCreate(length >= 0 ? length : 0, 0);
}

static void zrangeResultEmitCBufferForStore(zrange_result_handler *handler,
                                            const void *value,
                                            size_t value_length_in_bytes,
                                            double score) {
    double newscore;
    int retflags = 0;
    sds ele = sdsnewlen(value, value_length_in_bytes);
    int retval = zsetAdd(handler->dstobj, score, ele, ZADD_IN_NONE, &retflags, &newscore);
    sdsfree(ele);
    serverAssert(retval);
}

static void zrangeResultEmitLongLongForStore(zrange_result_handler *handler, long long value, double score) {
    double newscore;
    int retflags = 0;
    sds ele = sdsfromlonglong(value);
    int retval = zsetAdd(handler->dstobj, score, ele, ZADD_IN_NONE, &retflags, &newscore);
    sdsfree(ele);
    serverAssert(retval);
}

static void zrangeResultFinalizeStore(zrange_result_handler *handler, size_t result_count) {
    if (result_count) {
        setKey(handler->client, handler->client->db, handler->dstkey, &handler->dstobj, 0);
        notifyKeyspaceEvent(NOTIFY_ZSET, "zrangestore", handler->dstkey, handler->client->db->id);
        server.dirty++;
        addReplyLongLong(handler->client, result_count);
    } else {
        if (dbDelete(handler->client->db, handler->dstkey)) {
            signalModifiedKey(handler->client, handler->client->db, handler->dstkey);
            notifyKeyspaceEvent(NOTIFY_GENERIC, "del", handler->dstkey, handler->client->db->id);
            server.dirty++;
        }
        addReply(handler->client, shared.czero);
        decrRefCount(handler->dstobj);
    }
}

/* Initialize the consumer interface type with the requested type. */
static void zrangeResultHandlerInit(zrange_result_handler *handler, client *client, zrange_consumer_type type) {
    memset(handler, 0, sizeof(*handler));

    handler->client = client;

    switch (type) {
    case ZRANGE_CONSUMER_TYPE_CLIENT:
        handler->beginResultEmission = zrangeResultBeginClient;
        handler->finalizeResultEmission = zrangeResultFinalizeClient;
        handler->emitResultFromCBuffer = zrangeResultEmitCBufferToClient;
        handler->emitResultFromLongLong = zrangeResultEmitLongLongToClient;
        break;

    case ZRANGE_CONSUMER_TYPE_INTERNAL:
        handler->beginResultEmission = zrangeResultBeginStore;
        handler->finalizeResultEmission = zrangeResultFinalizeStore;
        handler->emitResultFromCBuffer = zrangeResultEmitCBufferForStore;
        handler->emitResultFromLongLong = zrangeResultEmitLongLongForStore;
        break;
    }
}

static void zrangeResultHandlerScoreEmissionEnable(zrange_result_handler *handler) {
    handler->withscores = 1;
    handler->should_emit_array_length = (handler->client->resp > 2);
}

static void zrangeResultHandlerDestinationKeySet(zrange_result_handler *handler, robj *dstkey) {
    handler->dstkey = dstkey;
}

/* This command implements ZRANGE, ZREVRANGE. */
void genericZrangebyrankCommand(zrange_result_handler *handler,
                                robj *zobj,
                                long start,
                                long end,
                                int withscores,
                                int reverse) {
    client *c = handler->client;
    long llen;
    long rangelen;
    size_t result_cardinality;

    /* Sanitize indexes. */
    llen = zsetLength(zobj);
    if (start < 0) start = llen + start;
    if (end < 0) end = llen + end;
    if (start < 0) start = 0;


    /* Invariant: start >= 0, so this test will be true when end < 0.
     * The range is empty when start > end or start >= length. */
    if (start > end || start >= llen) {
        handler->beginResultEmission(handler, 0);
        handler->finalizeResultEmission(handler, 0);
        return;
    }
    if (end >= llen) end = llen - 1;
    rangelen = (end - start) + 1;
    result_cardinality = rangelen;

    handler->beginResultEmission(handler, rangelen);
    if (zobj->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *zl = objectGetVal(zobj);
        unsigned char *eptr, *sptr;
        unsigned char *vstr;
        unsigned int vlen;
        long long vlong;
        double score = 0.0;

        if (reverse)
            eptr = lpSeek(zl, -2 - (2 * start));
        else
            eptr = lpSeek(zl, 2 * start);

        serverAssertWithInfo(c, zobj, eptr != NULL);
        sptr = lpNext(zl, eptr);

        while (rangelen--) {
            serverAssertWithInfo(c, zobj, eptr != NULL && sptr != NULL);
            vstr = lpGetValue(eptr, &vlen, &vlong);

            if (withscores) /* don't bother to extract the score if it's gonna be ignored. */
                score = zzlGetScore(sptr);

            if (vstr == NULL) {
                handler->emitResultFromLongLong(handler, vlong, score);
            } else {
                handler->emitResultFromCBuffer(handler, vstr, vlen, score);
            }

            if (reverse)
                zzlPrev(zl, &eptr, &sptr);
            else
                zzlNext(zl, &eptr, &sptr);
        }

    } else if (zobj->encoding == OBJ_ENCODING_BTREE) {
        zset *zs = objectGetVal(zobj);
        OrderedIndex *oi = zs->oi;
        OrderedIndexItem *ln;
        OrderedIndexIterator iter;
        orderedIndexInitIterator(&iter, oi);

        /* Seek to starting position */
        if (reverse) {
            unsigned long seek_idx = (start > 0) ? (unsigned long)(llen - start) : orderedIndexLength(oi);
            orderedIndexSeekToIndex(&iter, seek_idx);
        } else {
            if (start > 0) orderedIndexSeekToIndex(&iter, (unsigned long)start);
        }

        while (rangelen--) {
            ln = reverse ? orderedIndexPrev(&iter) : orderedIndexNext(&iter);
            serverAssertWithInfo(c, zobj, ln != NULL);
            const char *ele_ptr;
            size_t ele_len;
            orderedIndexItemGetElement(ln, &ele_ptr, &ele_len);
            handler->emitResultFromCBuffer(handler, ele_ptr, ele_len, orderedIndexItemGetScore(ln));
        }
    } else {
        serverPanic("Unknown sorted set encoding");
    }

    handler->finalizeResultEmission(handler, result_cardinality);
}

/* ZRANGESTORE <dst> <src> <min> <max> [BYSCORE | BYLEX] [REV] [LIMIT offset count] */
void zrangestoreCommand(client *c) {
    robj *dstkey = c->argv[1];
    zrange_result_handler handler;
    zrangeResultHandlerInit(&handler, c, ZRANGE_CONSUMER_TYPE_INTERNAL);
    zrangeResultHandlerDestinationKeySet(&handler, dstkey);
    zrangeGenericCommand(&handler, 2, 1, ZRANGE_AUTO, ZRANGE_DIRECTION_AUTO);
}

/* ZRANGE <key> <min> <max> [BYSCORE | BYLEX] [REV] [WITHSCORES] [XX] [LIMIT offset count] */
void zrangeCommand(client *c) {
    zrange_result_handler handler;
    zrangeResultHandlerInit(&handler, c, ZRANGE_CONSUMER_TYPE_CLIENT);
    zrangeGenericCommand(&handler, 1, 0, ZRANGE_AUTO, ZRANGE_DIRECTION_AUTO);
}

/* ZREVRANGE <key> <start> <stop> [WITHSCORES] [XX] */
void zrevrangeCommand(client *c) {
    zrange_result_handler handler;
    zrangeResultHandlerInit(&handler, c, ZRANGE_CONSUMER_TYPE_CLIENT);
    zrangeGenericCommand(&handler, 1, 0, ZRANGE_RANK, ZRANGE_DIRECTION_REVERSE);
}

/* This command implements ZRANGEBYSCORE, ZREVRANGEBYSCORE. */
void genericZrangebyscoreCommand(zrange_result_handler *handler,
                                 zrangespec *range,
                                 robj *zobj,
                                 long offset,
                                 long limit,
                                 int reverse) {
    unsigned long rangelen = 0;

    handler->beginResultEmission(handler, -1);

    /* For invalid offset, return directly. */
    if (offset > 0 && offset >= (long)zsetLength(zobj)) {
        handler->finalizeResultEmission(handler, 0);
        return;
    }

    if (zobj->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *zl = objectGetVal(zobj);
        unsigned char *eptr, *sptr;
        unsigned char *vstr;
        unsigned int vlen;
        long long vlong;

        /* If reversed, get the last node in range as starting point. */
        if (reverse) {
            eptr = zzlLastInRange(zl, range);
        } else {
            eptr = zzlFirstInRange(zl, range);
        }

        /* Get score pointer for the first element. */
        if (eptr) sptr = lpNext(zl, eptr);

        /* If there is an offset, just traverse the number of elements without
         * checking the score because that is done in the next loop. */
        while (eptr && offset--) {
            if (reverse) {
                zzlPrev(zl, &eptr, &sptr);
            } else {
                zzlNext(zl, &eptr, &sptr);
            }
        }

        while (eptr && limit--) {
            double score = zzlGetScore(sptr);

            /* Abort when the node is no longer in range. */
            if (reverse) {
                if (!zsetScoreGteMin(score, range)) break;
            } else {
                if (!zsetScoreLteMax(score, range)) break;
            }

            vstr = lpGetValue(eptr, &vlen, &vlong);
            rangelen++;
            if (vstr == NULL) {
                handler->emitResultFromLongLong(handler, vlong, score);
            } else {
                handler->emitResultFromCBuffer(handler, vstr, vlen, score);
            }

            /* Move to next node */
            if (reverse) {
                zzlPrev(zl, &eptr, &sptr);
            } else {
                zzlNext(zl, &eptr, &sptr);
            }
        }
    } else if (zobj->encoding == OBJ_ENCODING_BTREE) {
        zset *zs = objectGetVal(zobj);
        OrderedIndex *oi = zs->oi;
        OrderedIndexItem *ln;
        OrderedIndexIterator iter;
        orderedIndexInitIterator(&iter, oi);

        /* Seek to position within score range */
        orderedIndexSeekToScoreRange(&iter, range->min, range->max, range->minex, range->maxex, reverse ? -offset - 1 : offset);

        while (limit--) {
            ln = reverse ? orderedIndexPrev(&iter) : orderedIndexNext(&iter);
            if (ln == NULL) break;
            /* Abort when the node is no longer in range. */
            if (reverse) {
                if (!zsetScoreGteMin(orderedIndexItemGetScore(ln), range)) break;
            } else {
                if (!zsetScoreLteMax(orderedIndexItemGetScore(ln), range)) break;
            }

            rangelen++;
            const char *ele_ptr;
            size_t ele_len;
            orderedIndexItemGetElement(ln, &ele_ptr, &ele_len);
            handler->emitResultFromCBuffer(handler, ele_ptr, ele_len, orderedIndexItemGetScore(ln));
        }
    } else {
        serverPanic("Unknown sorted set encoding");
    }

    handler->finalizeResultEmission(handler, rangelen);
}

/* ZRANGEBYSCORE <key> <min> <max> [WITHSCORES] [XX] [LIMIT offset count] */
void zrangebyscoreCommand(client *c) {
    zrange_result_handler handler;
    zrangeResultHandlerInit(&handler, c, ZRANGE_CONSUMER_TYPE_CLIENT);
    zrangeGenericCommand(&handler, 1, 0, ZRANGE_SCORE, ZRANGE_DIRECTION_FORWARD);
}

/* ZREVRANGEBYSCORE <key> <max> <min> [WITHSCORES] [XX] [LIMIT offset count] */
void zrevrangebyscoreCommand(client *c) {
    zrange_result_handler handler;
    zrangeResultHandlerInit(&handler, c, ZRANGE_CONSUMER_TYPE_CLIENT);
    zrangeGenericCommand(&handler, 1, 0, ZRANGE_SCORE, ZRANGE_DIRECTION_REVERSE);
}

void zcountCommand(client *c) {
    robj *key = c->argv[1];
    robj *zobj;
    zrangespec range;
    unsigned long count = 0;

    /* Parse the range arguments */
    if (zslParseRange(c->argv[2], c->argv[3], &range) != C_OK) {
        addReplyError(c, "min or max is not a float");
        return;
    }

    /* Lookup the sorted set */
    if ((zobj = lookupKeyReadOrReply(c, key, shared.czero)) == NULL || checkType(c, zobj, OBJ_ZSET)) return;

    if (zobj->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *zl = objectGetVal(zobj);
        unsigned char *eptr, *sptr;
        double score;

        /* Use the first element in range as the starting point */
        eptr = zzlFirstInRange(zl, &range);

        /* No "first" element */
        if (eptr == NULL) {
            addReply(c, shared.czero);
            return;
        }

        /* First element is in range */
        sptr = lpNext(zl, eptr);
        score = zzlGetScore(sptr);
        serverAssertWithInfo(c, zobj, zsetScoreLteMax(score, &range));

        /* Iterate over elements in range */
        while (eptr) {
            score = zzlGetScore(sptr);

            /* Abort when the node is no longer in range. */
            if (!zsetScoreLteMax(score, &range)) {
                break;
            } else {
                count++;
                zzlNext(zl, &eptr, &sptr);
            }
        }
    } else if (zobj->encoding == OBJ_ENCODING_BTREE) {
        zset *zs = objectGetVal(zobj);
        OrderedIndex *oi = zs->oi;

        count = orderedIndexCountScoreRange(oi, range.min, range.max, range.minex, range.maxex);
    } else {
        serverPanic("Unknown sorted set encoding");
    }

    addReplyLongLong(c, count);
}

void zlexcountCommand(client *c) {
    robj *key = c->argv[1];
    robj *zobj;
    zlexrangespec range;
    unsigned long count = 0;

    /* Parse the range arguments */
    if (zsetParseLexRange(c->argv[2], c->argv[3], &range) != C_OK) {
        addReplyError(c, "min or max not valid string range item");
        return;
    }

    /* Lookup the sorted set */
    if ((zobj = lookupKeyReadOrReply(c, key, shared.czero)) == NULL || checkType(c, zobj, OBJ_ZSET)) {
        zsetFreeLexRange(&range);
        return;
    }

    if (zobj->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *zl = objectGetVal(zobj);
        unsigned char *eptr, *sptr;

        /* Use the first element in range as the starting point */
        eptr = zzlFirstInLexRange(zl, &range);

        /* No "first" element */
        if (eptr == NULL) {
            zsetFreeLexRange(&range);
            addReply(c, shared.czero);
            return;
        }

        /* First element is in range */
        sptr = lpNext(zl, eptr);
        serverAssertWithInfo(c, zobj, zzlLexValueLteMax(eptr, &range));

        /* Iterate over elements in range */
        while (eptr) {
            /* Abort when the node is no longer in range. */
            if (!zzlLexValueLteMax(eptr, &range)) {
                break;
            } else {
                count++;
                zzlNext(zl, &eptr, &sptr);
            }
        }
    } else if (zobj->encoding == OBJ_ENCODING_BTREE) {
        zset *zs = objectGetVal(zobj);
        OrderedIndex *oi = zs->oi;

        count = orderedIndexCountLexRange(oi, range.min, range.max, range.minex, range.maxex);
    } else {
        serverPanic("Unknown sorted set encoding");
    }

    zsetFreeLexRange(&range);
    addReplyLongLong(c, count);
}

/* This command implements ZRANGEBYLEX, ZREVRANGEBYLEX. */
void genericZrangebylexCommand(zrange_result_handler *handler,
                               zlexrangespec *range,
                               robj *zobj,
                               int withscores,
                               long offset,
                               long limit,
                               int reverse) {
    unsigned long rangelen = 0;

    handler->beginResultEmission(handler, -1);

    if (zobj->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *zl = objectGetVal(zobj);
        unsigned char *eptr, *sptr;
        unsigned char *vstr;
        unsigned int vlen;
        long long vlong;

        /* If reversed, get the last node in range as starting point. */
        if (reverse) {
            eptr = zzlLastInLexRange(zl, range);
        } else {
            eptr = zzlFirstInLexRange(zl, range);
        }

        /* Get score pointer for the first element. */
        if (eptr) sptr = lpNext(zl, eptr);

        /* If there is an offset, just traverse the number of elements without
         * checking the score because that is done in the next loop. */
        while (eptr && offset--) {
            if (reverse) {
                zzlPrev(zl, &eptr, &sptr);
            } else {
                zzlNext(zl, &eptr, &sptr);
            }
        }

        while (eptr && limit--) {
            double score = 0;
            if (withscores) /* don't bother to extract the score if it's gonna be ignored. */
                score = zzlGetScore(sptr);

            /* Abort when the node is no longer in range. */
            if (reverse) {
                if (!zzlLexValueGteMin(eptr, range)) break;
            } else {
                if (!zzlLexValueLteMax(eptr, range)) break;
            }

            vstr = lpGetValue(eptr, &vlen, &vlong);
            rangelen++;
            if (vstr == NULL) {
                handler->emitResultFromLongLong(handler, vlong, score);
            } else {
                handler->emitResultFromCBuffer(handler, vstr, vlen, score);
            }

            /* Move to next node */
            if (reverse) {
                zzlPrev(zl, &eptr, &sptr);
            } else {
                zzlNext(zl, &eptr, &sptr);
            }
        }
    } else if (zobj->encoding == OBJ_ENCODING_BTREE) {
        zset *zs = objectGetVal(zobj);
        OrderedIndex *oi = zs->oi;
        OrderedIndexItem *ln;
        OrderedIndexIterator iter;
        orderedIndexInitIterator(&iter, oi);

        /* Seek to position within lex range */
        orderedIndexSeekToLexRange(&iter, range->min, range->max, range->minex, range->maxex, reverse ? -offset - 1 : offset);

        while (limit--) {
            ln = reverse ? orderedIndexPrev(&iter) : orderedIndexNext(&iter);
            if (ln == NULL) break;
            /* Abort when the node is no longer in range. */
            const char *ele_ptr;
            size_t ele_len;
            orderedIndexItemGetElement(ln, &ele_ptr, &ele_len);
            if (reverse) {
                if (!zsetLexGteMin(ele_ptr, ele_len, range)) break;
            } else {
                if (!zsetLexLteMax(ele_ptr, ele_len, range)) break;
            }

            rangelen++;
            handler->emitResultFromCBuffer(handler, ele_ptr, ele_len, orderedIndexItemGetScore(ln));
        }
    } else {
        serverPanic("Unknown sorted set encoding");
    }

    handler->finalizeResultEmission(handler, rangelen);
}

/* ZRANGEBYLEX <key> <min> <max> [LIMIT offset count] [XX] */
void zrangebylexCommand(client *c) {
    zrange_result_handler handler;
    zrangeResultHandlerInit(&handler, c, ZRANGE_CONSUMER_TYPE_CLIENT);
    zrangeGenericCommand(&handler, 1, 0, ZRANGE_LEX, ZRANGE_DIRECTION_FORWARD);
}

/* ZREVRANGEBYLEX <key> <max> <min> [LIMIT offset count] [XX] */
void zrevrangebylexCommand(client *c) {
    zrange_result_handler handler;
    zrangeResultHandlerInit(&handler, c, ZRANGE_CONSUMER_TYPE_CLIENT);
    zrangeGenericCommand(&handler, 1, 0, ZRANGE_LEX, ZRANGE_DIRECTION_REVERSE);
}

/**
 * This function handles ZRANGE and ZRANGESTORE, and also the deprecated
 * Z[REV]RANGE[BYSCORE|BYLEX] commands.
 *
 * The simple ZRANGE and ZRANGESTORE can take _AUTO in rangetype and direction,
 * other command pass explicit value.
 *
 * The argc_start points to the src key argument, so following syntax is like:
 * <src> <min> <max> [BYSCORE | BYLEX] [REV] [WITHSCORES] [XX] [LIMIT offset count]
 *
 * Note: XX is not supported by ZRANGESTORE.
 */
void zrangeGenericCommand(zrange_result_handler *handler,
                          int argc_start,
                          int store,
                          zrange_type rangetype,
                          zrange_direction direction) {
    client *c = handler->client;
    robj *key = c->argv[argc_start];
    robj *zobj;
    zrangespec range;
    zlexrangespec lexrange;
    int minidx = argc_start + 1;
    int maxidx = argc_start + 2;

    /* Options common to all */
    long opt_start = 0;
    long opt_end = 0;
    int opt_withscores = 0;
    int opt_keyexist = 0;
    long opt_offset = 0;
    long opt_limit = -1;

    /* Step 1: Skip the <src> <min> <max> args and parse remaining optional arguments. */
    for (int j = argc_start + 3; j < c->argc; j++) {
        int leftargs = c->argc - j - 1;
        if (!store && !strcasecmp(objectGetVal(c->argv[j]), "withscores")) {
            opt_withscores = 1;
        } else if (!store && !strcasecmp(objectGetVal(c->argv[j]), "xx")) {
            opt_keyexist = 1;
        } else if (!strcasecmp(objectGetVal(c->argv[j]), "limit") && leftargs >= 2) {
            if ((getLongFromObjectOrReply(c, c->argv[j + 1], &opt_offset, NULL) != C_OK) ||
                (getLongFromObjectOrReply(c, c->argv[j + 2], &opt_limit, NULL) != C_OK)) {
                return;
            }
            j += 2;
        } else if (direction == ZRANGE_DIRECTION_AUTO && !strcasecmp(objectGetVal(c->argv[j]), "rev")) {
            direction = ZRANGE_DIRECTION_REVERSE;
        } else if (rangetype == ZRANGE_AUTO && !strcasecmp(objectGetVal(c->argv[j]), "bylex")) {
            rangetype = ZRANGE_LEX;
        } else if (rangetype == ZRANGE_AUTO && !strcasecmp(objectGetVal(c->argv[j]), "byscore")) {
            rangetype = ZRANGE_SCORE;
        } else {
            addReplyErrorObject(c, shared.syntaxerr);
            return;
        }
    }

    /* Use defaults if not overridden by arguments. */
    if (direction == ZRANGE_DIRECTION_AUTO) direction = ZRANGE_DIRECTION_FORWARD;
    if (rangetype == ZRANGE_AUTO) rangetype = ZRANGE_RANK;

    /* Check for conflicting arguments. */
    if (opt_limit != -1 && rangetype == ZRANGE_RANK) {
        addReplyError(c, "syntax error, LIMIT is only supported in combination with either BYSCORE or BYLEX");
        return;
    }
    if (opt_withscores && rangetype == ZRANGE_LEX) {
        addReplyError(c, "syntax error, WITHSCORES not supported in combination with BYLEX");
        return;
    }

    if (direction == ZRANGE_DIRECTION_REVERSE && ((ZRANGE_SCORE == rangetype) || (ZRANGE_LEX == rangetype))) {
        /* Range is given as [max,min] */
        int tmp = maxidx;
        maxidx = minidx;
        minidx = tmp;
    }

    /* Step 2: Parse the range. */
    switch (rangetype) {
    case ZRANGE_AUTO:
    case ZRANGE_RANK:
        /* Z[REV]RANGE, ZRANGESTORE [REV]RANGE */
        if ((getLongFromObjectOrReply(c, c->argv[minidx], &opt_start, NULL) != C_OK) ||
            (getLongFromObjectOrReply(c, c->argv[maxidx], &opt_end, NULL) != C_OK)) {
            return;
        }
        break;

    case ZRANGE_SCORE:
        /* Z[REV]RANGEBYSCORE, ZRANGESTORE [REV]RANGEBYSCORE */
        if (zslParseRange(c->argv[minidx], c->argv[maxidx], &range) != C_OK) {
            addReplyError(c, "min or max is not a float");
            return;
        }
        break;

    case ZRANGE_LEX:
        /* Z[REV]RANGEBYLEX, ZRANGESTORE [REV]RANGEBYLEX */
        if (zsetParseLexRange(c->argv[minidx], c->argv[maxidx], &lexrange) != C_OK) {
            addReplyError(c, "min or max not valid string range item");
            return;
        }
        break;
    }

    if (opt_withscores || store) {
        zrangeResultHandlerScoreEmissionEnable(handler);
    }

    /* Step 3: Lookup the key and get the range. */
    zobj = lookupKeyRead(c->db, key);
    if (zobj == NULL) {
        if (store) {
            handler->beginResultEmission(handler, -1);
            handler->finalizeResultEmission(handler, 0);
        } else if (opt_keyexist) {
            addReplyNullArray(c);
        } else {
            addReply(c, shared.emptyarray);
        }
        goto cleanup;
    }

    if (checkType(c, zobj, OBJ_ZSET)) goto cleanup;

    /* Step 4: Pass this to the command-specific handler. */
    switch (rangetype) {
    case ZRANGE_AUTO:
    case ZRANGE_RANK:
        genericZrangebyrankCommand(handler, zobj, opt_start, opt_end, opt_withscores || store,
                                   direction == ZRANGE_DIRECTION_REVERSE);
        break;

    case ZRANGE_SCORE:
        genericZrangebyscoreCommand(handler, &range, zobj, opt_offset, opt_limit,
                                    direction == ZRANGE_DIRECTION_REVERSE);
        break;

    case ZRANGE_LEX:
        genericZrangebylexCommand(handler, &lexrange, zobj, opt_withscores || store, opt_offset, opt_limit,
                                  direction == ZRANGE_DIRECTION_REVERSE);
        break;
    }

    /* Instead of returning here, we'll just fall-through the clean-up. */

cleanup:

    if (rangetype == ZRANGE_LEX) {
        zsetFreeLexRange(&lexrange);
    }
}

void zcardCommand(client *c) {
    robj *key = c->argv[1];
    robj *zobj;

    if ((zobj = lookupKeyReadOrReply(c, key, shared.czero)) == NULL || checkType(c, zobj, OBJ_ZSET)) return;

    addReplyLongLong(c, zsetLength(zobj));
}

/* Adds the member's score as a reply to the client. */
static void zscoreReply(client *c, robj *zobj, robj *member) {
    double score;

    if (zsetScore(zobj, objectGetVal(member), &score) == C_ERR) {
        addReplyNull(c);
    } else {
        addReplyDouble(c, score);
    }
}

void zscoreCommand(client *c) {
    robj *key = c->argv[1];
    robj *zobj;

    if ((zobj = lookupKeyReadOrReply(c, key, shared.null[c->resp])) == NULL || checkType(c, zobj, OBJ_ZSET)) return;

    zscoreReply(c, zobj, c->argv[2]);
}

#define ZMSCORE_FIND_BATCH_SIZE 16
static_assert(ZMSCORE_FIND_BATCH_SIZE <= HASHTABLE_FIND_BATCH_MAX_SIZE,
              "ZMSCORE batch size exceeds hashtable batch lookup limit");

static void zmscoreReplyWithHashtable(client *c, hashtable *ht, robj **members, size_t count) {
    const void *keys[ZMSCORE_FIND_BATCH_SIZE];
    void *found_entries[ZMSCORE_FIND_BATCH_SIZE];
    while (count) {
        size_t batch = count > ZMSCORE_FIND_BATCH_SIZE ? ZMSCORE_FIND_BATCH_SIZE : count;

        /* The same SDS may appear more than once, so only mark it once. */
        for (size_t i = 0; i < batch; i++) {
            sds member = objectGetVal(members[i]);
            if (!zsetIsLookupKey(member)) zsetMarkLookupKey(member);
            keys[i] = member;
        }

        uint32_t result = hashtableFindBatch(ht, (int)batch, keys, found_entries);

        /* Unmark each SDS once; later duplicates are already unmarked. */
        for (size_t i = 0; i < batch; i++) {
            sds member = objectGetVal(members[i]);
            if (zsetIsLookupKey(member)) zsetUnmarkLookupKey(member);
        }

        for (size_t i = 0; i < batch; i++) {
            if ((result >> i) & 1) {
                OrderedIndexItem *node = found_entries[i];
                addReplyDouble(c, orderedIndexItemGetScore(node));
            } else {
                addReplyNull(c);
            }
        }

        members += batch;
        count -= batch;
    }
}

void zmscoreCommand(client *c) {
    robj *key = c->argv[1];
    robj *zobj;

    zobj = lookupKeyRead(c->db, key);
    if (zobj == NULL) {
        addReplyArrayLen(c, c->argc - 2);
        for (int j = 2; j < c->argc; j++) {
            addReplyNull(c);
        }
        return;
    }
    if (checkType(c, zobj, OBJ_ZSET)) return;

    size_t count = c->argc - 2;
    addReplyArrayLen(c, count);

    /* Prefer hashtable batch lookup to improve performance. */
    if (zobj->encoding == OBJ_ENCODING_BTREE && count > 1) {
        zset *zs = objectGetVal(zobj);
        zmscoreReplyWithHashtable(c, zs->ht, c->argv + 2, count);
        return;
    }

    for (size_t i = 0; i < count; i++) {
        zscoreReply(c, zobj, c->argv[i + 2]);
    }
}

void zrankGenericCommand(client *c, int reverse) {
    robj *key = c->argv[1];
    robj *ele = c->argv[2];
    robj *zobj;
    robj *reply;
    long rank;
    int opt_withscore = 0;
    double score;

    if (c->argc > 4) {
        addReplyErrorArity(c);
        return;
    }
    if (c->argc > 3) {
        if (!strcasecmp(objectGetVal(c->argv[3]), "withscore")) {
            opt_withscore = 1;
        } else {
            addReplyErrorObject(c, shared.syntaxerr);
            return;
        }
    }
    reply = opt_withscore ? shared.nullarray[c->resp] : shared.null[c->resp];
    if ((zobj = lookupKeyReadOrReply(c, key, reply)) == NULL || checkType(c, zobj, OBJ_ZSET)) {
        return;
    }
    serverAssertWithInfo(c, ele, sdsEncodedObject(ele));
    rank = zsetRank(zobj, objectGetVal(ele), reverse, opt_withscore ? &score : NULL);
    if (rank >= 0) {
        if (opt_withscore) {
            addReplyArrayLen(c, 2);
        }
        addReplyLongLong(c, rank);
        if (opt_withscore) {
            addReplyDouble(c, score);
        }
    } else {
        if (opt_withscore) {
            addReplyNullArray(c);
        } else {
            addReplyNull(c);
        }
    }
}

void zrankCommand(client *c) {
    zrankGenericCommand(c, 0);
}

void zrevrankCommand(client *c) {
    zrankGenericCommand(c, 1);
}

void zscanCommand(client *c) {
    robj *o;
    unsigned long long cursor;

    if (parseScanCursorOrReply(c, objectGetVal(c->argv[2]), &cursor) == C_ERR) return;
    if ((o = lookupKeyReadOrReply(c, c->argv[1], shared.emptyscan)) == NULL || checkType(c, o, OBJ_ZSET)) return;
    scanGenericCommand(c, o, cursor);
}

void addZpopInitialReply(client *c, int emitkey, int use_nested_array, long rangelen, robj *key) {
    if (!use_nested_array && !emitkey) {
        /* ZPOPMIN/ZPOPMAX with or without COUNT option in RESP2. */
        addReplyArrayLen(c, rangelen * 2);
    } else if (use_nested_array && !emitkey) {
        /* ZPOPMIN/ZPOPMAX with COUNT option in RESP3. */
        addReplyArrayLen(c, rangelen);
    } else if (!use_nested_array && emitkey) {
        /* BZPOPMIN/BZPOPMAX in RESP2 and RESP3. */
        addReplyArrayLen(c, rangelen * 2 + 1);
        addReplyBulk(c, key);
    } else if (use_nested_array && emitkey) {
        /* ZMPOP/BZMPOP in RESP2 and RESP3. */
        addReplyArrayLen(c, 2);
        addReplyBulk(c, key);
        addReplyArrayLen(c, rangelen);
    }
}
/* This command implements the generic zpop operation, used by:
 * ZPOPMIN, ZPOPMAX, BZPOPMIN, BZPOPMAX and ZMPOP. This function is also used
 * inside blocked.c in the unblocking stage of BZPOPMIN, BZPOPMAX and BZMPOP.
 *
 * If 'emitkey' is true also the key name is emitted, useful for the blocking
 * behavior of BZPOP[MIN|MAX], since we can block into multiple keys.
 * Or in ZMPOP/BZMPOP, because we also can take multiple keys.
 *
 * 'count' is the number of elements requested to pop, or -1 for plain single pop.
 *
 * 'use_nested_array' when false it generates a flat array (with or without key name).
 * When true, it generates a nested 2 level array of field + score pairs, or 3 level when emitkey is set.
 *
 * 'reply_nil_when_empty' when true we reply a NIL if we are not able to pop up any elements.
 * Like in ZMPOP/BZMPOP we reply with a structured nested array containing key name
 * and member + score pairs. In these commands, we reply with null when we have no result.
 * Otherwise, in ZPOPMIN/ZPOPMAX we reply an empty array by default.
 *
 * 'deleted' is an optional output argument to get an indication
 * if the key got deleted by this function.
 * */
void genericZpopCommand(client *c,
                        robj **keyv,
                        int keyc,
                        int where,
                        int emitkey,
                        long count,
                        int use_nested_array,
                        int reply_nil_when_empty,
                        int *deleted) {
    int idx;
    robj *key = NULL;
    robj *zobj = NULL;
    sds ele;
    double score;

    if (deleted) *deleted = 0;

    /* Check type and break on the first error, otherwise identify candidate. */
    idx = 0;
    while (idx < keyc) {
        key = keyv[idx++];
        zobj = lookupKeyWrite(c->db, key);
        if (!zobj) continue;
        if (checkType(c, zobj, OBJ_ZSET)) return;
        break;
    }

    /* No candidate for zpopping, return empty. */
    if (!zobj) {
        if (reply_nil_when_empty) {
            addReplyNullArray(c);
        } else {
            addReply(c, shared.emptyarray);
        }
        return;
    }

    if (count == 0) {
        /* ZPOPMIN/ZPOPMAX with count 0. */
        addReply(c, shared.emptyarray);
        return;
    }

    long result_count = 0;

    /* When count is -1, we need to correct it to 1 for plain single pop. */
    if (count == -1) count = 1;

    long llen = zsetLength(zobj);
    long rangelen = (count > llen) ? llen : count;

    /* Remove the element. */
    do {
        if (zobj->encoding == OBJ_ENCODING_LISTPACK) {
            unsigned char *zl = objectGetVal(zobj);
            unsigned char *eptr, *sptr;
            unsigned char *vstr;
            unsigned int vlen;
            long long vlong;

            /* Get the first or last element in the sorted set. */
            eptr = lpSeek(zl, where == ZSET_MAX ? -2 : 0);
            serverAssertWithInfo(c, zobj, eptr != NULL);
            vstr = lpGetValue(eptr, &vlen, &vlong);
            if (vstr == NULL)
                ele = sdsfromlonglong(vlong);
            else
                ele = sdsnewlen(vstr, vlen);

            /* Get the score. */
            sptr = lpNext(zl, eptr);
            serverAssertWithInfo(c, zobj, sptr != NULL);
            score = zzlGetScore(sptr);
        } else if (zobj->encoding == OBJ_ENCODING_BTREE) {
            zset *zs = objectGetVal(zobj);
            OrderedIndex *oi = zs->oi;
            OrderedIndexItem *zln;

            /* Get the first or last element in the sorted set. */
            zln = (where == ZSET_MAX ? orderedIndexGetLast(oi) : orderedIndexGetFirst(oi));

            /* There must be an element in the sorted set. */
            serverAssertWithInfo(c, zobj, zln != NULL);
            const char *ele_ptr;
            size_t ele_len;
            orderedIndexItemGetElement(zln, &ele_ptr, &ele_len);
            ele = sdsnewlen(ele_ptr, ele_len);
            score = orderedIndexItemGetScore(zln);
        } else {
            serverPanic("Unknown sorted set encoding");
        }

        serverAssertWithInfo(c, zobj, zsetDel(zobj, ele));
        server.dirty++;

        if (result_count == 0) { /* Do this only for the first iteration. */
            char *events[2] = {"zpopmin", "zpopmax"};
            notifyKeyspaceEvent(NOTIFY_ZSET, events[where], key, c->db->id);
            addZpopInitialReply(c, emitkey, use_nested_array, rangelen, key);
        }

        if (use_nested_array) {
            addReplyArrayLen(c, 2);
        }
        addReplyBulkCBuffer(c, ele, sdslen(ele));
        addReplyDouble(c, score);
        sdsfree(ele);
        ++result_count;
    } while (--rangelen);

    /* Remove the key, if indeed needed. */
    if (zsetLength(zobj) == 0) {
        if (deleted) *deleted = 1;

        dbDelete(c->db, key);
        notifyKeyspaceEvent(NOTIFY_GENERIC, "del", key, c->db->id);
    }
    signalModifiedKey(c, c->db, key);

    if (c->cmd->proc == zmpopCommand) {
        /* Always replicate it as ZPOP[MIN|MAX] with COUNT option instead of ZMPOP. */
        robj *count_obj = createStringObjectFromLongLong((count > llen) ? llen : count);
        rewriteClientCommandVector(c, 3, (where == ZSET_MAX) ? shared.zpopmax : shared.zpopmin, key, count_obj);
        decrRefCount(count_obj);
    }
}

/* ZPOPMIN/ZPOPMAX key [<count>] */
void zpopMinMaxCommand(client *c, int where) {
    if (c->argc > 3) {
        addReplyErrorObject(c, shared.syntaxerr);
        return;
    }

    long count = -1; /* -1 for plain single pop. */
    if (c->argc == 3 && getPositiveLongFromObjectOrReply(c, c->argv[2], &count, NULL) != C_OK) return;

    /* Respond with a single (flat) array in RESP2 or if count is -1
     * (returning a single element). In RESP3, when count > 0 use nested array. */
    int use_nested_array = (c->resp > 2 && count != -1);

    genericZpopCommand(c, &c->argv[1], 1, where, 0, count, use_nested_array, 0, NULL);
}

/* ZPOPMIN key [<count>] */
void zpopminCommand(client *c) {
    zpopMinMaxCommand(c, ZSET_MIN);
}

/* ZPOPMAX key [<count>] */
void zpopmaxCommand(client *c) {
    zpopMinMaxCommand(c, ZSET_MAX);
}

/* BZPOPMIN, BZPOPMAX, BZMPOP actual implementation.
 *
 * 'numkeys' is the number of keys.
 *
 * 'timeout_idx' parameter position of block timeout.
 *
 * 'where' ZSET_MIN or ZSET_MAX.
 *
 * 'count' is the number of elements requested to pop, or -1 for plain single pop.
 *
 * 'use_nested_array' when false it generates a flat array (with or without key name).
 * When true, it generates a nested 3 level array of keyname, field + score pairs.
 * */
void blockingGenericZpopCommand(client *c,
                                robj **keys,
                                int numkeys,
                                int where,
                                int timeout_idx,
                                long count,
                                int use_nested_array,
                                int reply_nil_when_empty) {
    robj *o;
    robj *key;
    mstime_t timeout;
    int j;

    if (getTimeoutFromObjectOrReply(c, c->argv[timeout_idx], &timeout, UNIT_SECONDS) != C_OK) return;

    for (j = 0; j < numkeys; j++) {
        key = keys[j];
        o = lookupKeyWrite(c->db, key);
        /* Non-existing key, move to next key. */
        if (o == NULL) continue;

        if (checkType(c, o, OBJ_ZSET)) return;

        long llen = zsetLength(o);
        /* Empty zset, move to next key. */
        if (llen == 0) continue;

        /* Non empty zset, this is like a normal ZPOP[MIN|MAX]. */
        genericZpopCommand(c, &key, 1, where, 1, count, use_nested_array, reply_nil_when_empty, NULL);

        if (count == -1) {
            /* Replicate it as ZPOP[MIN|MAX] instead of BZPOP[MIN|MAX]. */
            rewriteClientCommandVector(c, 2, (where == ZSET_MAX) ? shared.zpopmax : shared.zpopmin, key);
        } else {
            /* Replicate it as ZPOP[MIN|MAX] with COUNT option. */
            robj *count_obj = createStringObjectFromLongLong((count > llen) ? llen : count);
            rewriteClientCommandVector(c, 3, (where == ZSET_MAX) ? shared.zpopmax : shared.zpopmin, key, count_obj);
            decrRefCount(count_obj);
        }

        return;
    }

    /* If we are not allowed to block the client and the zset is empty the only thing
     * we can do is treating it as a timeout (even with timeout 0). */
    if (c->flag.deny_blocking) {
        addReplyNullArray(c);
        return;
    }

    /* If the keys do not exist we must block */
    blockForKeys(c, BLOCKED_ZSET, keys, numkeys, timeout, 0);
}

// BZPOPMIN key [key ...] timeout
void bzpopminCommand(client *c) {
    blockingGenericZpopCommand(c, c->argv + 1, c->argc - 2, ZSET_MIN, c->argc - 1, -1, 0, 0);
}

// BZPOPMAX key [key ...] timeout
void bzpopmaxCommand(client *c) {
    blockingGenericZpopCommand(c, c->argv + 1, c->argc - 2, ZSET_MAX, c->argc - 1, -1, 0, 0);
}

static void zrandmemberReplyWithListpack(client *c, unsigned int count, listpackEntry *keys, listpackEntry *vals) {
    for (unsigned long i = 0; i < count; i++) {
        if (vals && c->resp > 2) addReplyArrayLen(c, 2);
        if (keys[i].sval)
            addReplyBulkCBuffer(c, keys[i].sval, keys[i].slen);
        else
            addReplyBulkLongLong(c, keys[i].lval);
        if (vals) {
            if (vals[i].sval) {
                addReplyDouble(c, zzlStrtod(vals[i].sval, vals[i].slen));
            } else
                addReplyDouble(c, vals[i].lval);
        }
    }
}

/* How many times bigger should be the zset compared to the requested size
 * for us to not use the "remove elements" strategy? Read later in the
 * implementation for more info. */
#define ZRANDMEMBER_SUB_STRATEGY_MUL 3

/* If client is trying to ask for a very large number of random elements,
 * queuing may consume an unlimited amount of memory, so we want to limit
 * the number of randoms per time. */
#define ZRANDMEMBER_RANDOM_SAMPLE_LIMIT 1000

void zrandmemberWithCountCommand(client *c, long l, int withscores) {
    unsigned long count, size;
    int uniq = 1;
    robj *zsetobj;

    if ((zsetobj = lookupKeyReadOrReply(c, c->argv[1], shared.emptyarray)) == NULL || checkType(c, zsetobj, OBJ_ZSET))
        return;
    size = zsetLength(zsetobj);

    if (l >= 0) {
        count = (unsigned long)l;
    } else {
        count = -l;
        uniq = 0;
    }

    /* If count is zero, serve it ASAP to avoid special cases later. */
    if (count == 0) {
        addReply(c, shared.emptyarray);
        return;
    }

    /* CASE 1: The count was negative, so the extraction method is just:
     * "return N random elements" sampling the whole set every time.
     * This case is trivial and can be served without auxiliary data
     * structures. This case is the only one that also needs to return the
     * elements in random order. */
    if (!uniq || count == 1) {
        if (withscores && c->resp == 2)
            addReplyArrayLen(c, count * 2);
        else
            addReplyArrayLen(c, count);
        if (zsetobj->encoding == OBJ_ENCODING_BTREE) {
            zset *zs = objectGetVal(zsetobj);
            while (count--) {
                void *entry;
                serverAssert(hashtableFairRandomEntry(zs->ht, &entry));
                OrderedIndexItem *node = entry;
                if (withscores && c->resp > 2) addReplyArrayLen(c, 2);
                const char *ele_ptr_tmp;
                size_t ele_len_tmp;
                orderedIndexItemGetElement(node, &ele_ptr_tmp, &ele_len_tmp);
                addReplyBulkCBuffer(c, ele_ptr_tmp, ele_len_tmp);
                if (withscores) addReplyDouble(c, orderedIndexItemGetScore(node));
                if (c->flag.close_asap) break;
            }
        } else if (zsetobj->encoding == OBJ_ENCODING_LISTPACK) {
            listpackEntry *keys, *vals = NULL;
            unsigned long limit, sample_count;
            limit = count > ZRANDMEMBER_RANDOM_SAMPLE_LIMIT ? ZRANDMEMBER_RANDOM_SAMPLE_LIMIT : count;
            keys = zmalloc(sizeof(listpackEntry) * limit);
            if (withscores) vals = zmalloc(sizeof(listpackEntry) * limit);
            while (count) {
                sample_count = count > limit ? limit : count;
                count -= sample_count;
                lpRandomPairs(objectGetVal(zsetobj), sample_count, keys, vals);
                zrandmemberReplyWithListpack(c, sample_count, keys, vals);
                if (c->flag.close_asap) break;
            }
            zfree(keys);
            zfree(vals);
        }
        return;
    }

    zsetopsrc src;
    zsetopval zval;
    src.subject = zsetobj;
    src.type = zsetobj->type;
    src.encoding = zsetobj->encoding;
    zuiInitIterator(&src);
    memset(&zval, 0, sizeof(zval));

    /* Initiate reply count, RESP3 responds with nested array, RESP2 with flat one. */
    long reply_size = count < size ? count : size;
    if (withscores && c->resp == 2)
        addReplyArrayLen(c, reply_size * 2);
    else
        addReplyArrayLen(c, reply_size);

    /* CASE 2:
     * The number of requested elements is greater than the number of
     * elements inside the zset: simply return the whole zset. */
    if (count >= size) {
        while (zuiNext(&src, &zval)) {
            if (withscores && c->resp > 2) addReplyArrayLen(c, 2);
            addReplyBulkSds(c, zuiNewSdsFromValue(&zval));
            if (withscores) addReplyDouble(c, zval.score);
        }
        zuiClearIterator(&src);
        return;
    }

    /* CASE 2.5 listpack only. Sampling unique elements, in non-random order.
     * Listpack encoded zsets are meant to be relatively small, so
     * ZRANDMEMBER_SUB_STRATEGY_MUL isn't necessary and we rather not make
     * copies of the entries. Instead, we emit them directly to the output
     * buffer.
     *
     * And it is inefficient to repeatedly pick one random element from a
     * listpack in CASE 4. So we use this instead. */
    if (zsetobj->encoding == OBJ_ENCODING_LISTPACK) {
        listpackEntry *keys, *vals = NULL;
        keys = zmalloc(sizeof(listpackEntry) * count);
        if (withscores) vals = zmalloc(sizeof(listpackEntry) * count);
        serverAssert(lpRandomPairsUnique(objectGetVal(zsetobj), count, keys, vals) == count);
        zrandmemberReplyWithListpack(c, count, keys, vals);
        zfree(keys);
        zfree(vals);
        zuiClearIterator(&src);
        return;
    }

    /* CASE 3:
     * The number of elements inside the zset is not greater than
     * ZRANDMEMBER_SUB_STRATEGY_MUL times the number of requested elements.
     * In this case we create a hashtable from scratch with all the elements, and
     * subtract random elements to reach the requested number of elements.
     *
     * This is done because if the number of requested elements is just
     * a bit less than the number of elements in the set, the natural approach
     * used into CASE 4 is highly inefficient. */
    if (count * ZRANDMEMBER_SUB_STRATEGY_MUL > size) {
        /* Hashtable encoding (generic implementation) */
        hashtable *ht = hashtableCreate(&zsetHashtableType);
        hashtableExpand(ht, size);
        zset *zs = objectGetVal(src.subject);
        hashtableIterator iter;
        hashtableInitIterator(&iter, zs->ht, 0);
        void *entry;
        /* Add all the elements into the temporary hashtable. */
        while (hashtableNext(&iter, &entry)) {
            bool res = hashtableAdd(ht, entry);
            serverAssert(res);
        }
        serverAssert(hashtableSize(ht) == size);

        /* Remove random elements to reach the right count. */
        while (size > count) {
            void *element;
            hashtableFairRandomEntry(ht, &element);
            hashtableDelete(ht, element);
            size--;
        }
        hashtableCleanupIterator(&iter);

        /* Reply with what's in the temporary hashtable and release memory */
        hashtableInitIterator(&iter, ht, 0);
        void *next;
        while (hashtableNext(&iter, &next)) {
            OrderedIndexItem *node = (OrderedIndexItem *)next;
            const char *key_ptr_tmp;
            size_t key_len_tmp;
            orderedIndexItemGetElement(node, &key_ptr_tmp, &key_len_tmp);
            if (withscores && c->resp > 2) addReplyArrayLen(c, 2);
            addReplyBulkCBuffer(c, key_ptr_tmp, key_len_tmp);
            if (withscores) addReplyDouble(c, orderedIndexItemGetScore(node));
        }

        hashtableCleanupIterator(&iter);
        hashtableRelease(ht);
    }

    /* CASE 4: We have a big zset compared to the requested number of elements.
     * In this case we can simply get random elements from the zset and add
     * to the temporary set, trying to eventually get enough unique elements
     * to reach the specified count. */
    else {
        /* Hashtable encoding (generic implementation) */
        unsigned long added = 0;
        hashtable *ht = hashtableCreate(&setHashtableType);
        hashtableExpand(ht, count);

        while (added < count) {
            listpackEntry key;
            double score = 0;
            zsetTypeRandomElement(zsetobj, size, &key, withscores ? &score : NULL);

            /* Try to add the object to the hashtable. If it already exists
             * free it, otherwise increment the number of objects we have
             * in the result hashtable. */
            sds skey = zsetSdsFromListpackEntry(&key);
            if (!hashtableAdd(ht, skey)) {
                sdsfree(skey);
                continue;
            }
            added++;

            if (withscores && c->resp > 2) addReplyArrayLen(c, 2);
            zsetReplyFromListpackEntry(c, &key);
            if (withscores) addReplyDouble(c, score);
        }

        /* Release memory */
        hashtableRelease(ht);
    }
    zuiClearIterator(&src);
}

/* ZRANDMEMBER key [<count> [WITHSCORES]] */
void zrandmemberCommand(client *c) {
    long l;
    int withscores = 0;
    robj *zset;
    listpackEntry ele;

    if (c->argc >= 3) {
        if (getRangeLongFromObjectOrReply(c, c->argv[2], -LONG_MAX, LONG_MAX, &l, NULL) != C_OK) return;
        if (c->argc > 4 || (c->argc == 4 && strcasecmp(objectGetVal(c->argv[3]), "withscores"))) {
            addReplyErrorObject(c, shared.syntaxerr);
            return;
        } else if (c->argc == 4) {
            withscores = 1;
            if (l < -LONG_MAX / 2 || l > LONG_MAX / 2) {
                addReplyError(c, "value is out of range");
                return;
            }
        }
        zrandmemberWithCountCommand(c, l, withscores);
        return;
    }

    /* Handle variant without <count> argument. Reply with simple bulk string */
    if ((zset = lookupKeyReadOrReply(c, c->argv[1], shared.null[c->resp])) == NULL || checkType(c, zset, OBJ_ZSET)) {
        return;
    }

    zsetTypeRandomElement(zset, zsetLength(zset), &ele, NULL);
    zsetReplyFromListpackEntry(c, &ele);
}

/* ZMPOP/BZMPOP
 *
 * 'numkeys_idx' parameter position of key number.
 * 'is_block' this indicates whether it is a blocking variant. */
void zmpopGenericCommand(client *c, int numkeys_idx, int is_block) {
    long j;
    long numkeys = 0; /* Number of keys. */
    int where = 0;    /* ZSET_MIN or ZSET_MAX. */
    long count = -1;  /* Reply will consist of up to count elements, depending on the zset's length. */

    /* Parse the numkeys. */
    if (getRangeLongFromObjectOrReply(c, c->argv[numkeys_idx], 1, LONG_MAX, &numkeys,
                                      "numkeys should be greater than 0") != C_OK)
        return;

    /* Parse the where. where_idx: the index of where in the c->argv. */
    long where_idx = numkeys_idx + numkeys + 1;
    if (where_idx >= c->argc) {
        addReplyErrorObject(c, shared.syntaxerr);
        return;
    }
    if (!strcasecmp(objectGetVal(c->argv[where_idx]), "MIN")) {
        where = ZSET_MIN;
    } else if (!strcasecmp(objectGetVal(c->argv[where_idx]), "MAX")) {
        where = ZSET_MAX;
    } else {
        addReplyErrorObject(c, shared.syntaxerr);
        return;
    }

    /* Parse the optional arguments. */
    for (j = where_idx + 1; j < c->argc; j++) {
        char *opt = objectGetVal(c->argv[j]);
        int moreargs = (c->argc - 1) - j;

        if (count == -1 && !strcasecmp(opt, "COUNT") && moreargs) {
            j++;
            if (getRangeLongFromObjectOrReply(c, c->argv[j], 1, LONG_MAX, &count, "count should be greater than 0") !=
                C_OK)
                return;
        } else {
            addReplyErrorObject(c, shared.syntaxerr);
            return;
        }
    }

    if (count == -1) count = 1;

    if (is_block) {
        /* BLOCK. We will handle CLIENT_DENY_BLOCKING flag in blockingGenericZpopCommand. */
        blockingGenericZpopCommand(c, c->argv + numkeys_idx + 1, numkeys, where, 1, count, 1, 1);
    } else {
        /* NON-BLOCK */
        genericZpopCommand(c, c->argv + numkeys_idx + 1, numkeys, where, 1, count, 1, 1, NULL);
    }
}

/* ZMPOP numkeys key [<key> ...] MIN|MAX [COUNT count] */
void zmpopCommand(client *c) {
    zmpopGenericCommand(c, 1, 0);
}

/* BZMPOP timeout numkeys key [<key> ...] MIN|MAX [COUNT count] */
void bzmpopCommand(client *c) {
    zmpopGenericCommand(c, 2, 1);
}
