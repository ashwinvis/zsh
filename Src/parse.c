/*
 * parse.c - parser
 *
 * This file is part of zsh, the Z shell.
 *
 * Copyright (c) 1992-1997 Paul Falstad
 * All rights reserved.
 *
 * Permission is hereby granted, without written agreement and without
 * license or royalty fees, to use, copy, modify, and distribute this
 * software and to distribute modified versions of this software for any
 * purpose, provided that the above copyright notice and the following
 * two paragraphs appear in all copies of this software.
 *
 * In no event shall Paul Falstad or the Zsh Development Group be liable
 * to any party for direct, indirect, special, incidental, or consequential
 * damages arising out of the use of this software and its documentation,
 * even if Paul Falstad and the Zsh Development Group have been advised of
 * the possibility of such damage.
 *
 * Paul Falstad and the Zsh Development Group specifically disclaim any
 * warranties, including, but not limited to, the implied warranties of
 * merchantability and fitness for a particular purpose.  The software
 * provided hereunder is on an "as is" basis, and Paul Falstad and the
 * Zsh Development Group have no obligation to provide maintenance,
 * support, updates, enhancements, or modifications.
 *
 */

#include "zsh.mdh"
#include "parse.pro"

/* != 0 if we are about to read a command word */
 
/**/
mod_export int incmdpos;
 
/* != 0 if we are in the middle of a [[ ... ]] */
 
/**/
mod_export int incond;
 
/* != 0 if we are after a redirection (for ctxtlex only) */
 
/**/
mod_export int inredir;
 
/* != 0 if we are about to read a case pattern */
 
/**/
int incasepat;
 
/* != 0 if we just read a newline */
 
/**/
int isnewlin;

/* != 0 if we are after a for keyword */

/**/
int infor;

/* list of here-documents */

/**/
struct heredocs *hdocs;
 

#define YYERROR(O)  { tok = LEXERR; ecused = (O); return 0; }
#define YYERRORV(O) { tok = LEXERR; ecused = (O); return; }
#define COND_ERROR(X,Y) do { \
  zwarn(X,Y,0); \
  herrflush(); \
  if (noerrs != 2) \
    errflag = 1; \
  YYERROR(ecused) \
} while(0)


/* 
 * Word code.
 *
 * For now we simply post-process the syntax tree produced by the
 * parser. We compile it into a struct eprog. Some day the parser
 * above should be changed to emit the word code directly.
 *
 * Word code layout:
 *
 *   WC_END
 *     - end of program code
 *
 *   WC_LIST
 *     - data contains type (sync, ...)
 *     - follwed by code for this list
 *     - if not (type & Z_END), followed by next WC_LIST
 *
 *   WC_SUBLIST
 *     - data contains type (&&, ||, END) and flags (coprog, not)
 *     - followed by code for sublist
 *     - if not (type == END), followed by next WC_SUBLIST
 *
 *   WC_PIPE
 *     - data contains type (end, mid) and LINENO
 *     - if not (type == END), followed by offset to next WC_PIPE
 *     - followed by command
 *     - if not (type == END), followed by next WC_PIPE
 *
 *   WC_REDIR
 *     - must precede command-code (or WC_ASSIGN)
 *     - data contains type (<, >, ...)
 *     - followed by fd1 and name from struct redir
 *
 *   WC_ASSIGN
 *     - data contains type (scalar, array) and number of array-elements
 *     - followed by name and value
 *
 *   WC_SIMPLE
 *     - data contains the number of arguments (plus command)
 *     - followed by strings
 *
 *   WC_SUBSH
 *     - data unused
 *     - followed by list
 *
 *   WC_CURSH
 *     - data unused
 *     - followed by list
 *
 *   WC_TIMED
 *     - data contains type (followed by pipe or not)
 *     - if (type == PIPE), followed by pipe
 *
 *   WC_FUNCDEF
 *     - data contains offset to after body
 *     - followed by number of names
 *     - followed by names
 *     - followed by offset to first string
 *     - followed by length of string table
 *     - followed by number of patterns for body
 *     - follwoed by codes for body
 *     - followed by strings for body
 *
 *   WC_FOR
 *     - data contains type (list, ...) and offset to after body
 *     - if (type == COND), followed by init, cond, advance expressions
 *     - else if (type == PPARAM), followed by param name
 *     - else if (type == LIST), followed by param name, num strings, strings
 *     - followed by body
 *
 *   WC_SELECT
 *     - data contains type (list, ...) and offset to after body
 *     - if (type == PPARAM), followed by param name
 *     - else if (type == LIST), followed by param name, num strings, strings
 *     - followed by body
 *
 *   WC_WHILE
 *     - data contains type (while, until) and ofsset to after body
 *     - followed by condition
 *     - followed by body
 *
 *   WC_REPEAT
 *     - data contains offset to after body
 *     - followed by number-string
 *     - followed by body
 *
 *   WC_CASE
 *     - first CASE is always of type HEAD, data contains offset to esac
 *     - after that CASEs of type OR (;;) and AND (;&), data is offset to
 *       next case
 *     - each OR/AND case is followed by pattern, pattern-number, list
 *
 *   WC_IF
 *     - first IF is of type HEAD, data contains offset to fi
 *     - after that IFs of type IF, ELIF, ELSE, data is offset to next
 *     - each non-HEAD is followed by condition (only IF, ELIF) and body
 *
 *   WC_COND
 *     - data contains type
 *     - if (type == AND/OR), data contains offset to after this one,
 *       followed by two CONDs
 *     - else if (type == NOT), followed by COND
 *     - else if (type == MOD), followed by name and strings
 *     - else if (type == MODI), followed by name, left, right
 *     - else if (type == STR[N]EQ), followed by left, right, pattern-number
 *     - else if (has two args) followed by left, right
 *     - else followed by string
 *
 *   WC_ARITH
 *     - followed by string (there's only one)
 *
 *   WC_AUTOFN
 *     - only used by the autoload builtin
 *
 * Lists and sublists may also be simplified, indicated by the presence
 * of the Z_SIMPLE or WC_SUBLIST_SIMPLE flags. In this case they are only
 * followed by a slot containing the line number, not by a WC_SUBLIST or
 * WC_PIPE, respectively. The real advantage of simplified lists and
 * sublists is that they can be executed faster, see exec.c. In the
 * parser, the test if a list can be simplified is done quite simply
 * by passing a int* around which gets set to non-zero if the thing
 * just parsed is `complex', i.e. may need to be run by forking or 
 * some such.
 *
 * In each of the above, strings are encoded as one word code. For empty
 * strings this is the bit pattern 11x, the lowest bit is non-zero if the
 * string contains tokens and zero otherwise (this is true for the other
 * ways to encode strings, too). For short strings (one to three
 * characters), this is the marker 01x with the 24 bits above that
 * containing the characters. Longer strings are encoded as the offset
 * into the strs character array stored in the eprog struct shifted by
 * two and ored with the bit pattern 0x.
 * The ecstr() function that adds the code for a string uses a simple
 * list of strings already added so that long strings are encoded only
 * once.
 *
 * Note also that in the eprog struct the pattern, code, and string
 * arrays all point to the same memory block.
 *
 *
 * To make things even faster in future versions, we could not only 
 * test if the strings contain tokens, but instead what kind of
 * expansions need to be done on strings. In the execution code we
 * could then use these flags for a specialized version of prefork()
 * to avoid a lot of string parsing and some more string duplication.
 */

/**/
int eclen, ecused, ecfree, ecnpats;
/**/
Wordcode ecbuf;
/**/
Eccstr ecstrs;
/**/
int ecsoffs, ecssub, ecnfunc;

/* Insert n free code-slots at position p. */

static void
ecispace(int p, int n)
{
    int m;

    if (ecfree < n) {
	int a = (n > 256 ? n : 256);

	ecbuf = (Wordcode) hrealloc((char *) ecbuf, eclen * sizeof(wordcode),
				    (eclen + a) * sizeof(wordcode));
	eclen += a;
	ecfree += a;
    }
    if ((m = ecused - p) > 0)
	memmove(ecbuf + p + n, ecbuf + p, m * sizeof(wordcode));
    ecused += n;
}

/* Add one wordcode. */

static int
ecadd(wordcode c)
{
    if (ecfree < 1) {
	ecbuf = (Wordcode) hrealloc((char *) ecbuf, eclen * sizeof(wordcode),
				    (eclen + 256) * sizeof(wordcode));
	eclen += 256;
	ecfree += 256;
    }
    ecbuf[ecused] = c;
    ecused++;
    ecfree--;

    return ecused - 1;
}

/* Delete a wordcode. */

static void
ecdel(int p)
{
    int n = ecused - p - 1;

    if (n > 0)
	memmove(ecbuf + p, ecbuf + p + 1, n * sizeof(wordcode));
    ecused--;
}

/* Build the wordcode for a string. */

static wordcode
ecstrcode(char *s)
{
    int l, t = has_token(s);

    if ((l = strlen(s) + 1) && l <= 4) {
	wordcode c = (t ? 3 : 2);
	switch (l) {
	case 4: c |= ((wordcode) STOUC(s[2])) << 19;
	case 3: c |= ((wordcode) STOUC(s[1])) << 11;
	case 2: c |= ((wordcode) STOUC(s[0])) <<  3; break;
	case 1: c = (t ? 7 : 6); break;
	}
	return c;
    } else {
	Eccstr p, q = NULL;

	for (p = ecstrs; p; q = p, p = p->next)
	    if (p->nfunc == ecnfunc && !strcmp(s, p->str))
		return p->offs;

	p = (Eccstr) zhalloc(sizeof(*p));
	p->next = NULL;
	if (q)
	    q->next = p;
	else
	    ecstrs = p;
	p->offs = ((ecsoffs - ecssub) << 2) | (t ? 1 : 0);
	p->str = s;
	p->nfunc = ecnfunc;
	ecsoffs += l;

	return p->offs;
    }
}

static int
ecstr(char *s)
{
    return ecadd(ecstrcode(s));
}


#define par_save_list(C) \
    do { \
        int eu = ecused; \
        par_list(C); \
        if (eu == ecused) ecadd(WCB_END()); \
    } while (0)
#define par_save_list1(C) \
    do { \
        int eu = ecused; \
        par_list1(C); \
        if (eu == ecused) ecadd(WCB_END()); \
    } while (0)


/* Initialise wordcode buffer. */

static void
init_parse(void)
{
    ecbuf = (Wordcode) zhalloc((eclen = ecfree = 256) * sizeof(wordcode));
    ecused = 0;
    ecstrs = NULL;
    ecsoffs = ecnpats = 0;
    ecssub = 0;
    ecnfunc = 0;
}

/* Build eprog. */

static Eprog
bld_eprog(void)
{
    Eprog ret;
    Eccstr p;
    char *q;
    int l;

    ecadd(WCB_END());

    ret = (Eprog) zhalloc(sizeof(*ret));
    ret->len = ((ecnpats * sizeof(Patprog)) +
		(ecused * sizeof(wordcode)) +
		ecsoffs);
    ret->npats = ecnpats;
    ret->pats = (Patprog *) zhalloc(ret->len);
    ret->prog = (Wordcode) (ret->pats + ecnpats);
    ret->strs = (char *) (ret->prog + ecused);
    ret->shf = NULL;
    ret->alloc = EA_HEAP;
    ret->dump = NULL;
    for (l = 0; l < ecnpats; l++)
	ret->pats[l] = dummy_patprog1;
    memcpy(ret->prog, ecbuf, ecused * sizeof(wordcode));
    for (p = ecstrs, q = ret->strs; p; p = p->next, q += l) {
	l = strlen(p->str) + 1;
	memcpy(q, p->str, l);
    }
    return ret;
}

/*
 * event	: ENDINPUT
 *			| SEPER
 *			| sublist [ SEPER | AMPER | AMPERBANG ]
 */

/**/
Eprog
parse_event(void)
{
    tok = ENDINPUT;
    incmdpos = 1;
    yylex();
    init_parse();
    return ((par_event()) ? bld_eprog() : NULL);
}

/**/
static int
par_event(void)
{
    int r = 0, p, c = 0;

    while (tok == SEPER) {
	if (isnewlin > 0)
	    return 0;
	yylex();
    }
    if (tok == ENDINPUT)
	return 0;

    p = ecadd(0);

    if (par_sublist(&c)) {
	if (tok == ENDINPUT) {
	    set_list_code(p, Z_SYNC, c);
	    r = 1;
	} else if (tok == SEPER) {
	    set_list_code(p, Z_SYNC, c);
	    if (isnewlin <= 0)
		yylex();
	    r = 1;
	} else if (tok == AMPER) {
	    set_list_code(p, Z_ASYNC, c);
	    yylex();
	    r = 1;
	} else if (tok == AMPERBANG) {
	    set_list_code(p, (Z_ASYNC | Z_DISOWN), c);
	    yylex();
	    r = 1;
	}
    }
    if (!r) {
	if (errflag) {
	    yyerror(0);
	    ecused--;
	    return 0;
	}
	yyerror(1);
	herrflush();
	if (noerrs != 2)
	    errflag = 1;
	ecused--;
	return 0;
    } else {
	int oec = ecused;

	par_event();
	if (ecused == oec)
	    ecbuf[p] |= wc_bdata(Z_END);
    }
    return 1;
}

/**/
mod_export Eprog
parse_list(void)
{
    int c = 0;

    tok = ENDINPUT;
    incmdpos = 1;
    yylex();
    init_parse();
    par_list(&c);
#if 0 
   if (tok == LEXERR)
#endif
   if (tok != ENDINPUT) {
	yyerror(0);
	return NULL;
    }
    return bld_eprog();
}

/**/
mod_export Eprog
parse_cond(void)
{
    init_parse();

    if (!par_cond())
	return NULL;

    return bld_eprog();
}

/* This adds a list wordcode. The important bit about this is that it also
 * tries to optimise this to a Z_SIMPLE list code. */

/**/
static void
set_list_code(int p, int type, int complex)
{
    if (!complex && (type == Z_SYNC || type == (Z_SYNC | Z_END)) &&
	WC_SUBLIST_TYPE(ecbuf[p + 1]) == WC_SUBLIST_END) {
	int ispipe = !(WC_SUBLIST_FLAGS(ecbuf[p + 1]) & WC_SUBLIST_SIMPLE);
	ecbuf[p] = WCB_LIST((type | Z_SIMPLE), ecused - 2 - p);
	ecdel(p + 1);
	if (ispipe)
	    ecbuf[p + 1] = WC_PIPE_LINENO(ecbuf[p + 1]);
    } else
	ecbuf[p] = WCB_LIST(type, 0);
}

/* The same for sublists. */

/**/
static void
set_sublist_code(int p, int type, int flags, int skip, int complex)
{
    if (complex)
	ecbuf[p] = WCB_SUBLIST(type, flags, skip);
    else {
	ecbuf[p] = WCB_SUBLIST(type, (flags | WC_SUBLIST_SIMPLE), skip);
	ecbuf[p + 1] = WC_PIPE_LINENO(ecbuf[p + 1]);
    }
}

/*
 * list	: { SEPER } [ sublist [ { SEPER | AMPER | AMPERBANG } list ] ]
 */

/**/
static int
par_list(int *complex)
{
    int p, lp = -1, c;

 rec:

    while (tok == SEPER)
	yylex();

    p = ecadd(0);
    c = 0;

    if (par_sublist(&c)) {
	*complex |= c;
	if (tok == SEPER || tok == AMPER || tok == AMPERBANG) {
	    if (tok != SEPER)
		*complex = 1;
	    set_list_code(p, ((tok == SEPER) ? Z_SYNC :
			      (tok == AMPER) ? Z_ASYNC :
			      (Z_ASYNC | Z_DISOWN)), c);
	    incmdpos = 1;
	    do {
		yylex();
	    } while (tok == SEPER);
	    lp = p;
	    goto rec;
	} else
	    set_list_code(p, (Z_SYNC | Z_END), c);
	return 1;
    } else {
	ecused--;
	if (lp >= 0) {
	    ecbuf[lp] |= wc_bdata(Z_END);
	    return 1;
	}
	return 0;
    }
}

/**/
static int
par_list1(int *complex)
{
    int p = ecadd(0), c = 0;

    if (par_sublist(&c)) {
	set_list_code(p, (Z_SYNC | Z_END), c);
	*complex |= c;
	return 1;
    } else {
	ecused--;
	return 0;
    }
}

/*
 * sublist	: sublist2 [ ( DBAR | DAMPER ) { SEPER } sublist ]
 */

/**/
static int
par_sublist(int *complex)
{
    int f, p, c = 0;

    p = ecadd(0);

    if ((f = par_sublist2(&c)) != -1) {
	int e = ecused;

	*complex |= c;
	if (tok == DBAR || tok == DAMPER) {
	    int qtok = tok, sl;

	    cmdpush(tok == DBAR ? CS_CMDOR : CS_CMDAND);
	    yylex();
	    while (tok == SEPER)
		yylex();
	    sl = par_sublist(complex);
	    set_sublist_code(p, (sl ? (qtok == DBAR ?
				       WC_SUBLIST_OR : WC_SUBLIST_AND) :
				 WC_SUBLIST_END),
			     f, (e - 1 - p), c);
	    cmdpop();
	} else
	    set_sublist_code(p, WC_SUBLIST_END, f, (e - 1 - p), c);
	return 1;
    } else {
	ecused--;
	return 0;
    }
}

/*
 * sublist2	: [ COPROC | BANG ] pline
 */

/**/
static int
par_sublist2(int *complex)
{
    int f = 0;

    if (tok == COPROC) {
	*complex = 1;
	f |= WC_SUBLIST_COPROC;
	yylex();
    } else if (tok == BANG) {
	*complex = 1;
	f |= WC_SUBLIST_NOT;
	yylex();
    }
    if (!par_pline(complex) && !f)
	return -1;

    return f;
}

/*
 * pline	: cmd [ ( BAR | BARAMP ) { SEPER } pline ]
 */

/**/
static int
par_pline(int *complex)
{
    int p, line = lineno;

    p = ecadd(0);

    if (!par_cmd(complex)) {
	ecused--;
	return 0;
    }
    if (tok == BAR) {
	*complex = 1;
	cmdpush(CS_PIPE);
	yylex();
	while (tok == SEPER)
	    yylex();
	ecbuf[p] = WCB_PIPE(WC_PIPE_MID, (line >= 0 ? line + 1 : 0));
	ecispace(p + 1, 1);
	ecbuf[p + 1] = ecused - 1 - p;
	par_pline(complex);
	cmdpop();
	return 1;
    } else if (tok == BARAMP) {
	int r;

	for (r = p + 1; wc_code(ecbuf[r]) == WC_REDIR; r += 3);

	ecispace(r, 3);
	p += 3;
	ecbuf[r] = WCB_REDIR(MERGEOUT);
	ecbuf[r + 1] = 2;
	ecbuf[r + 2] = ecstrcode("1");

	*complex = 1;
	cmdpush(CS_ERRPIPE);
	yylex();
	ecbuf[p] = WCB_PIPE(WC_PIPE_MID, (line >= 0 ? line + 1 : 0));
	ecispace(p + 1, 1);
	ecbuf[p + 1] = ecused - 1 - p;
	par_pline(complex);
	cmdpop();
	return 1;
    } else {
	ecbuf[p] = WCB_PIPE(WC_PIPE_END, (line >= 0 ? line + 1 : 0));
	return 1;
    }
}

/*
 * cmd	: { redir } ( for | case | if | while | repeat |
 *				subsh | funcdef | time | dinbrack | dinpar | simple ) { redir }
 */

/**/
static int
par_cmd(int *complex)
{
    int r, nr = 0;

    r = ecused;

    if (IS_REDIROP(tok)) {
	*complex = 1;
	while (IS_REDIROP(tok)) {
	    nr++;
	    par_redir(&r);
	}
    }
    switch (tok) {
    case FOR:
	cmdpush(CS_FOR);
	par_for(complex);
	cmdpop();
	break;
    case FOREACH:
	cmdpush(CS_FOREACH);
	par_for(complex);
	cmdpop();
	break;
    case SELECT:
	*complex = 1;
	cmdpush(CS_SELECT);
	par_for(complex);
	cmdpop();
	break;
    case CASE:
	cmdpush(CS_CASE);
	par_case(complex);
	cmdpop();
	break;
    case IF:
	par_if(complex);
	break;
    case WHILE:
	cmdpush(CS_WHILE);
	par_while(complex);
	cmdpop();
	break;
    case UNTIL:
	cmdpush(CS_UNTIL);
	par_while(complex);
	cmdpop();
	break;
    case REPEAT:
	cmdpush(CS_REPEAT);
	par_repeat(complex);
	cmdpop();
	break;
    case INPAR:
	*complex = 1;
	cmdpush(CS_SUBSH);
	par_subsh(complex);
	cmdpop();
	break;
    case INBRACE:
	cmdpush(CS_CURSH);
	par_subsh(complex);
	cmdpop();
	break;
    case FUNC:
	cmdpush(CS_FUNCDEF);
	par_funcdef();
	cmdpop();
	break;
    case TIME:
	*complex = 1;
	par_time();
	break;
    case DINBRACK:
	cmdpush(CS_COND);
	par_dinbrack();
	cmdpop();
	break;
    case DINPAR:
	ecadd(WCB_ARITH());
	ecstr(tokstr);
	yylex();
	break;
    default:
	{
	    int sr;

	    if (!(sr = par_simple(complex, nr))) {
		if (!nr)
		    return 0;
	    } else {
		/* Three codes per redirection. */
		if (sr > 1) {
		    *complex = 1;
		    r += (sr - 1) * 3;
		}
	    }
	}
	break;
    }
    if (IS_REDIROP(tok)) {
	*complex = 1;
	while (IS_REDIROP(tok))
	    par_redir(&r);
    }
    incmdpos = 1;
    incasepat = 0;
    incond = 0;
    return 1;
}

/*
 * for  : ( FOR DINPAR expr SEMI expr SEMI expr DOUTPAR |
 *    ( FOR[EACH] | SELECT ) name ( "in" wordlist | INPAR wordlist OUTPAR ) )
 *	{ SEPER } ( DO list DONE | INBRACE list OUTBRACE | list ZEND | list1 )
 */

/**/
static void
par_for(int *complex)
{
    int oecused = ecused, csh = (tok == FOREACH), p, sel = (tok == SELECT);
    int type;

    p = ecadd(0);

    incmdpos = 0;
    infor = tok == FOR ? 2 : 0;
    yylex();
    if (tok == DINPAR) {
	yylex();
	if (tok != DINPAR)
	    YYERRORV(oecused);
	ecstr(tokstr);
	yylex();
	if (tok != DINPAR)
	    YYERRORV(oecused);
	ecstr(tokstr);
	yylex();
	if (tok != DOUTPAR)
	    YYERRORV(oecused);
	ecstr(tokstr);
	infor = 0;
	incmdpos = 1;
	yylex();
	type = WC_FOR_COND;
    } else {
	infor = 0;
	if (tok != STRING || !isident(tokstr))
	    YYERRORV(oecused);
	ecstr(tokstr);
	incmdpos = 1;
	yylex();
	if (tok == STRING && !strcmp(tokstr, "in")) {
	    int np, n;

	    incmdpos = 0;
	    yylex();
	    np = ecadd(0);
	    n = par_wordlist();
	    if (tok != SEPER)
		YYERRORV(oecused);
	    ecbuf[np] = n;
	    type = (sel ? WC_SELECT_LIST : WC_FOR_LIST);
	} else if (tok == INPAR) {
	    int np, n;

	    incmdpos = 0;
	    yylex();
	    np = ecadd(0);
	    n = par_nl_wordlist();
	    if (tok != OUTPAR)
		YYERRORV(oecused);
	    ecbuf[np] = n;
	    incmdpos = 1;
	    yylex();
	    type = (sel ? WC_SELECT_LIST : WC_FOR_LIST);
	} else
	    type = (sel ? WC_SELECT_PPARAM : WC_FOR_PPARAM);
    }
    incmdpos = 1;
    while (tok == SEPER)
	yylex();
    if (tok == DO) {
	yylex();
	par_save_list(complex);
	if (tok != DONE)
	    YYERRORV(oecused);
	yylex();
    } else if (tok == INBRACE) {
	yylex();
	par_save_list(complex);
	if (tok != OUTBRACE)
	    YYERRORV(oecused);
	yylex();
    } else if (csh || isset(CSHJUNKIELOOPS)) {
	par_save_list(complex);
	if (tok != ZEND)
	    YYERRORV(oecused);
	yylex();
    } else if (unset(SHORTLOOPS)) {
	YYERRORV(oecused);
    } else
	par_save_list1(complex);

    ecbuf[p] = (sel ?
		WCB_SELECT(type, ecused - 1 - p) :
		WCB_FOR(type, ecused - 1 - p));
}

/*
 * case	: CASE STRING { SEPER } ( "in" | INBRACE )
				{ { SEPER } STRING { BAR STRING } OUTPAR
					list [ DSEMI | SEMIAMP ] }
				{ SEPER } ( "esac" | OUTBRACE )
 */

/**/
static void
par_case(int *complex)
{
    int oecused = ecused, brflag, p, pp, n = 1, type;

    p = ecadd(0);

    incmdpos = 0;
    yylex();
    if (tok != STRING)
	YYERRORV(oecused);
    ecstr(tokstr);

    incmdpos = 1;
    yylex();
    while (tok == SEPER)
	yylex();
    if (!(tok == STRING && !strcmp(tokstr, "in")) && tok != INBRACE)
	YYERRORV(oecused);
    brflag = (tok == INBRACE);
    incasepat = 1;
    incmdpos = 0;
    yylex();

    for (;;) {
	char *str;

	while (tok == SEPER)
	    yylex();
	if (tok == OUTBRACE)
	    break;
	if (tok != STRING)
	    YYERRORV(oecused);
	if (!strcmp(tokstr, "esac"))
	    break;
	str = dupstring(tokstr);
	incasepat = 0;
	incmdpos = 1;
	type = WC_CASE_OR;
	for (;;) {
	    yylex();
	    if (tok == OUTPAR) {
		incasepat = 0;
		incmdpos = 1;
		yylex();
		break;
	    } else if (tok == BAR) {
		char *str2;
		int sl = strlen(str);

		incasepat = 1;
		incmdpos = 0;
		str2 = hcalloc(sl + 2);
		strcpy(str2, str);
		str2[sl] = Bar;
		str2[sl+1] = '\0';
		str = str2;
	    } else {
		int sl = strlen(str);

		if (!sl || str[sl - 1] != Bar) {
		    /* POSIX allows (foo*) patterns */
		    int pct;
		    char *s;

		    for (s = str, pct = 0; *s; s++) {
			if (*s == Inpar)
			    pct++;
			if (!pct)
			    break;
			if (pct == 1) {
			    if (*s == Bar || *s == Inpar)
				while (iblank(s[1]))
				    chuck(s+1);
			    if (*s == Bar || *s == Outpar)
				while (iblank(s[-1]) &&
				       (s < str + 1 || s[-2] != Meta))
				    chuck(--s);
			}
			if (*s == Outpar)
			    pct--;
		    }
		    if (*s || pct || s == str)
			YYERRORV(oecused);
		    /* Simplify pattern by removing surrounding (...) */
		    sl = strlen(str);
		    DPUTS(*str != Inpar || str[sl - 1] != Outpar,
			  "BUG: strange case pattern");
		    str[sl - 1] = '\0';
		    chuck(str);
		    break;
		} else {
		    char *str2;

		    if (tok != STRING)
			YYERRORV(oecused);
		    str2 = hcalloc(sl + strlen(tokstr) + 1);
		    strcpy(str2, str);
		    strcpy(str2 + sl, tokstr);
		    str = str2;
		}
	    }
	}
	pp = ecadd(0);
	ecstr(str);
	ecadd(ecnpats++);
	par_save_list(complex);
	n++;
	if (tok == SEMIAMP)
	    type = WC_CASE_AND;
	ecbuf[pp] = WCB_CASE(type, ecused - 1 - pp);
	if ((tok == ESAC && !brflag) || (tok == OUTBRACE && brflag))
	    break;
	if (tok != DSEMI && tok != SEMIAMP)
	    YYERRORV(oecused);
	incasepat = 1;
	incmdpos = 0;
	yylex();
    }
    incmdpos = 1;
    yylex();

    ecbuf[p] = WCB_CASE(WC_CASE_HEAD, ecused - 1 - p);
}

/*
 * if	: { ( IF | ELIF ) { SEPER } ( INPAR list OUTPAR | list )
			{ SEPER } ( THEN list | INBRACE list OUTBRACE | list1 ) }
			[ FI | ELSE list FI | ELSE { SEPER } INBRACE list OUTBRACE ]
			(you get the idea...?)
 */

/**/
static void
par_if(int *complex)
{
    int oecused = ecused, xtok, p, pp, type, usebrace = 0;
    unsigned char nc;

    p = ecadd(0);

    for (;;) {
	xtok = tok;
	cmdpush(xtok == IF ? CS_IF : CS_ELIF);
	yylex();
	if (xtok == FI)
	    break;
	if (xtok == ELSE)
	    break;
	while (tok == SEPER)
	    yylex();
	if (!(xtok == IF || xtok == ELIF)) {
	    cmdpop();
	    YYERRORV(oecused);
	}
	pp = ecadd(0);
	type = (xtok == IF ? WC_IF_IF : WC_IF_ELIF);
	par_save_list(complex);
	incmdpos = 1;
	while (tok == SEPER)
	    yylex();
	xtok = FI;
	nc = cmdstack[cmdsp - 1] == CS_IF ? CS_IFTHEN : CS_ELIFTHEN;
	if (tok == THEN) {
	    usebrace = 0;
	    cmdpop();
	    cmdpush(nc);
	    yylex();
	    par_save_list(complex);
	    ecbuf[pp] = WCB_IF(type, ecused - 1 - pp);
	    incmdpos = 1;
	    cmdpop();
	} else if (tok == INBRACE) {
	    usebrace = 1;
	    cmdpop();
	    cmdpush(nc);
	    yylex();
	    par_save_list(complex);
	    if (tok != OUTBRACE) {
		cmdpop();
		YYERRORV(oecused);
	    }
	    ecbuf[pp] = WCB_IF(type, ecused - 1 - pp);
	    yylex();
	    incmdpos = 1;
	    if (tok == SEPER)
		break;
	    cmdpop();
	} else if (unset(SHORTLOOPS)) {
	    cmdpop();
	    YYERRORV(oecused);
	} else {
	    cmdpop();
	    cmdpush(nc);
	    par_save_list1(complex);
	    ecbuf[pp] = WCB_IF(type, ecused - 1 - pp);
	    incmdpos = 1;
	    break;
	}
    }
    cmdpop();
    if (xtok == ELSE) {
	pp = ecadd(0);
	cmdpush(CS_ELSE);
	while (tok == SEPER)
	    yylex();
	if (tok == INBRACE && usebrace) {
	    yylex();
	    par_save_list(complex);
	    if (tok != OUTBRACE) {
		cmdpop();
		YYERRORV(oecused);
	    }
	} else {
	    par_save_list(complex);
	    if (tok != FI) {
		cmdpop();
		YYERRORV(oecused);
	    }
	}
	ecbuf[pp] = WCB_IF(WC_IF_ELSE, ecused - 1 - pp);
	yylex();
	cmdpop();
    }
    ecbuf[p] = WCB_IF(WC_IF_HEAD, ecused - 1 - p);
}

/*
 * while	: ( WHILE | UNTIL ) ( INPAR list OUTPAR | list ) { SEPER }
				( DO list DONE | INBRACE list OUTBRACE | list ZEND )
 */

/**/
static void
par_while(int *complex)
{
    int oecused = ecused, p;
    int type = (tok == UNTIL ? WC_WHILE_UNTIL : WC_WHILE_WHILE);

    p = ecadd(0);
    yylex();
    par_save_list(complex);
    incmdpos = 1;
    while (tok == SEPER)
	yylex();
    if (tok == DO) {
	yylex();
	par_save_list(complex);
	if (tok != DONE)
	    YYERRORV(oecused);
	yylex();
    } else if (tok == INBRACE) {
	yylex();
	par_save_list(complex);
	if (tok != OUTBRACE)
	    YYERRORV(oecused);
	yylex();
    } else if (isset(CSHJUNKIELOOPS)) {
	par_save_list(complex);
	if (tok != ZEND)
	    YYERRORV(oecused);
	yylex();
    } else
	YYERRORV(oecused);

    ecbuf[p] = WCB_WHILE(type, ecused - 1 - p);
}

/*
 * repeat	: REPEAT STRING { SEPER } ( DO list DONE | list1 )
 */

/**/
static void
par_repeat(int *complex)
{
    int oecused = ecused, p;

    p = ecadd(0);

    incmdpos = 0;
    yylex();
    if (tok != STRING)
	YYERRORV(oecused);
    ecstr(tokstr);
    incmdpos = 1;
    yylex();
    while (tok == SEPER)
	yylex();
    if (tok == DO) {
	yylex();
	par_save_list(complex);
	if (tok != DONE)
	    YYERRORV(oecused);
	yylex();
    } else if (tok == INBRACE) {
	yylex();
	par_save_list(complex);
	if (tok != OUTBRACE)
	    YYERRORV(oecused);
	yylex();
    } else if (isset(CSHJUNKIELOOPS)) {
	par_save_list(complex);
	if (tok != ZEND)
	    YYERRORV(oecused);
	yylex();
    } else if (unset(SHORTLOOPS)) {
	YYERRORV(oecused);
    } else
	par_save_list1(complex);

    ecbuf[p] = WCB_REPEAT(ecused - 1 - p);
}

/*
 * subsh	: ( INPAR | INBRACE ) list ( OUTPAR | OUTBRACE )
 */

/**/
static void
par_subsh(int *complex)
{
    int oecused = ecused, otok = tok;

    ecadd(tok == INPAR ? WCB_SUBSH() : WCB_CURSH());
    yylex();
    par_save_list(complex);
    if (tok != ((otok == INPAR) ? OUTPAR : OUTBRACE))
	YYERRORV(oecused);
    incmdpos = 1;
    yylex();
}

/*
 * funcdef	: FUNCTION wordlist [ INOUTPAR ] { SEPER }
 *					( list1 | INBRACE list OUTBRACE )
 */

/**/
static void
par_funcdef(void)
{
    int oecused = ecused, oldlineno = lineno, num = 0, onp, p, c = 0;
    int so, oecssub = ecssub;

    lineno = 0;
    nocorrect = 1;
    incmdpos = 0;
    yylex();

    p = ecadd(0);
    ecadd(0);

    incmdpos = 1;
    while (tok == STRING) {
	if (*tokstr == Inbrace && !tokstr[1]) {
	    tok = INBRACE;
	    break;
	}
	ecstr(tokstr);
	num++;
	yylex();
    }
    ecadd(0);
    ecadd(0);
    ecadd(0);

    nocorrect = 0;
    if (tok == INOUTPAR)
	yylex();
    while (tok == SEPER)
	yylex();

    ecnfunc++;
    ecssub = so = ecsoffs;
    onp = ecnpats;
    ecnpats = 0;

    if (tok == INBRACE) {
	yylex();
	par_list(&c);
	if (tok != OUTBRACE) {
	    lineno += oldlineno;
	    ecnpats = onp;
	    ecssub = oecssub;
	    YYERRORV(oecused);
	}
	yylex();
    } else if (unset(SHORTLOOPS)) {
	lineno += oldlineno;
	ecnpats = onp;
	ecssub = oecssub;
	YYERRORV(oecused);
    } else
	par_list1(&c);

    ecadd(WCB_END());
    ecbuf[p + num + 2] = so - oecssub;
    ecbuf[p + num + 3] = ecsoffs - so;
    ecbuf[p + num + 4] = ecnpats;
    ecbuf[p + 1] = num;

    lineno += oldlineno;
    ecnpats = onp;
    ecssub = oecssub;

    ecbuf[p] = WCB_FUNCDEF(ecused - 1 - p);
}

/*
 * time	: TIME sublist2
 */

/**/
static void
par_time(void)
{
    int p, f, c = 0;

    yylex();

    p = ecadd(0);
    ecadd(0);
    f = par_sublist2(&c);
    ecbuf[p] = WCB_TIMED((p + 1 == ecused) ? WC_TIMED_EMPTY : WC_TIMED_PIPE);
    set_sublist_code(p + 1, WC_SUBLIST_END, f, ecused - 2 - p, c);
}

/*
 * dinbrack	: DINBRACK cond DOUTBRACK
 */

/**/
static void
par_dinbrack(void)
{
    int oecused = ecused;

    incond = 1;
    incmdpos = 0;
    yylex();
    par_cond();
    if (tok != DOUTBRACK)
	YYERRORV(oecused);
    incond = 0;
    incmdpos = 1;
    yylex();
}

/*
 * simple	: { COMMAND | EXEC | NOGLOB | NOCORRECT | DASH }
					{ STRING | ENVSTRING | ENVARRAY wordlist OUTPAR | redir }
					[ INOUTPAR { SEPER } ( list1 | INBRACE list OUTBRACE ) ]
 */

/**/
static int
par_simple(int *complex, int nr)
{
    int oecused = ecused, isnull = 1, r, argc = 0, p, isfunc = 0, sr = 0;
    int c = *complex;

    r = ecused;
    for (;;) {
	if (tok == NOCORRECT) {
	    *complex = c = 1;
	    nocorrect = 1;
	} else if (tok == ENVSTRING) {
	    char *p, *name, *str;

	    ecadd(WCB_ASSIGN(WC_ASSIGN_SCALAR, 0));
	    name = tokstr;
	    for (p = tokstr; *p && *p != Inbrack && *p != '='; p++);
	    if (*p == Inbrack && !skipparens(Inbrack, Outbrack, &p) &&
		*p == '=') {
		*p = '\0';
		str = p + 1;
	    } else
		equalsplit(tokstr, &str);
	    ecstr(name);
	    ecstr(str);
	    isnull = 0;
	} else if (tok == ENVARRAY) {
	    int oldcmdpos = incmdpos, n;

	    p = ecadd(0);
	    incmdpos = 0;
	    ecstr(tokstr);
	    cmdpush(CS_ARRAY);
	    yylex();
	    n = par_nl_wordlist();
	    ecbuf[p] = WCB_ASSIGN(WC_ASSIGN_ARRAY, n);
	    cmdpop();
	    if (tok != OUTPAR)
		YYERROR(oecused);
	    incmdpos = oldcmdpos;
	    isnull = 0;
	} else
	    break;
	yylex();
    }
    if (tok == AMPER || tok == AMPERBANG)
	YYERROR(oecused);

    p = ecadd(WCB_SIMPLE(0));

    for (;;) {
	if (tok == STRING) {
	    *complex = 1;
	    incmdpos = 0;
	    ecstr(tokstr);
	    argc++;
	    yylex();
	} else if (IS_REDIROP(tok)) {
	    *complex = c = 1;
	    par_redir(&r);
	    p += 3;		/* 3 codes per redirection */
	    sr++;
	} else if (tok == INOUTPAR) {
	    int oldlineno = lineno, onp, so, oecssub = ecssub;

	    *complex = c;
	    lineno = 0;
	    incmdpos = 1;
	    cmdpush(CS_FUNCDEF);
	    yylex();
	    while (tok == SEPER)
		yylex();

	    ecispace(p + 1, 1);
	    ecbuf[p + 1] = argc;
	    ecadd(0);
	    ecadd(0);
	    ecadd(0);

	    ecnfunc++;
	    ecssub = so = ecsoffs;
	    onp = ecnpats;
	    ecnpats = 0;

	    if (tok == INBRACE) {
		int c = 0;

		yylex();
		par_list(&c);
		if (tok != OUTBRACE) {
		    cmdpop();
		    lineno += oldlineno;
		    ecnpats = onp;
		    ecssub = oecssub;
		    YYERROR(oecused);
		}
		yylex();
	    } else {
		int ll, sl, c = 0;

		ll = ecadd(0);
		sl = ecadd(0);

		par_cmd(&c);

		set_sublist_code(sl, WC_SUBLIST_END, 0, ecused - 1 - sl, c);
		set_list_code(ll, (Z_SYNC | Z_END), c);
	    }
	    cmdpop();

	    ecadd(WCB_END());
	    ecbuf[p + argc + 2] = so - oecssub;
	    ecbuf[p + argc + 3] = ecsoffs - so;
	    ecbuf[p + argc + 4] = ecnpats;

	    lineno += oldlineno;
	    ecnpats = onp;
	    ecssub = oecssub;

	    ecbuf[p] = WCB_FUNCDEF(ecused - 1 - p);

	    isfunc = 1;
	} else
	    break;
	isnull = 0;
    }
    if (isnull && !(sr + nr)) {
	ecused = p;
	return 0;
    }
    incmdpos = 1;

    if (!isfunc)
	ecbuf[p] = WCB_SIMPLE(argc);

    return sr + 1;
}

/*
 * redir	: ( OUTANG | ... | TRINANG ) STRING
 */

static int redirtab[TRINANG - OUTANG + 1] = {
    WRITE,
    WRITENOW,
    APP,
    APPNOW,
    READ,
    READWRITE,
    HEREDOC,
    HEREDOCDASH,
    MERGEIN,
    MERGEOUT,
    ERRWRITE,
    ERRWRITENOW,
    ERRAPP,
    ERRAPPNOW,
    HERESTR,
};

/**/
static void
par_redir(int *rp)
{
    int r = *rp, type, fd1, oldcmdpos, oldnc;
    char *name;

    oldcmdpos = incmdpos;
    incmdpos = 0;
    oldnc = nocorrect;
    if (tok != INANG && tok != INOUTANG)
	nocorrect = 1;
    type = redirtab[tok - OUTANG];
    fd1 = tokfd;
    yylex();
    if (tok != STRING && tok != ENVSTRING)
	YYERRORV(ecused);
    incmdpos = oldcmdpos;
    nocorrect = oldnc;

    /* assign default fd */
    if (fd1 == -1)
	fd1 = IS_READFD(type) ? 0 : 1;

    name = tokstr;

    switch (type) {
    case HEREDOC:
    case HEREDOCDASH: {
	/* <<[-] name */
	struct heredocs **hd;

	for (hd = &hdocs; *hd; hd = &(*hd)->next);
	*hd = zalloc(sizeof(struct heredocs));
	(*hd)->next = NULL;
	(*hd)->pc = ecbuf + r;
	(*hd)->str = tokstr;

	/* If we ever need more than three codes (or less), we have to change
	 * the factors in par_cmd() and par_simple(), too. */
	ecispace(r, 3);
	*rp = r + 3;
	ecbuf[r] = WCB_REDIR(type);
	ecbuf[r + 1] = fd1;

	yylex();
	return;
    }
    case WRITE:
    case WRITENOW:
	if (tokstr[0] == Outang && tokstr[1] == Inpar)
	    /* > >(...) */
	    type = OUTPIPE;
	else if (tokstr[0] == Inang && tokstr[1] == Inpar)
	    YYERRORV(ecused);
	break;
    case READ:
	if (tokstr[0] == Inang && tokstr[1] == Inpar)
	    /* < <(...) */
	    type = INPIPE;
	else if (tokstr[0] == Outang && tokstr[1] == Inpar)
	    YYERRORV(ecused);
	break;
    case READWRITE:
	if ((tokstr[0] == Inang || tokstr[0] == Outang) && tokstr[1] == Inpar)
	    type = tokstr[0] == Inang ? INPIPE : OUTPIPE;
	break;
    }
    yylex();

    /* If we ever need more than three codes (or less), we have to change
     * the factors in par_cmd() and par_simple(), too. */
    ecispace(r, 3);
    *rp = r + 3;
    ecbuf[r] = WCB_REDIR(type);
    ecbuf[r + 1] = fd1;
    ecbuf[r + 2] = ecstrcode(name);
}

/**/
void
setheredoc(Wordcode pc, int type, char *str)
{
    pc[0] = WCB_REDIR(type);
    pc[2] = ecstrcode(str);
}

/*
 * wordlist	: { STRING }
 */

/**/
static int
par_wordlist(void)
{
    int num = 0;
    while (tok == STRING) {
	ecstr(tokstr);
	num++;
	yylex();
    }
    return num;
}

/*
 * nl_wordlist	: { STRING | SEPER }
 */

/**/
static int
par_nl_wordlist(void)
{
    int num = 0;

    while (tok == STRING || tok == SEPER) {
	if (tok != SEPER) {
	    ecstr(tokstr);
	    num++;
	}
	yylex();
    }
    return num;
}

/*
 * condlex is yylex for normal parsing, but is altered to allow
 * the test builtin to use par_cond.
 */

/**/
void (*condlex) _((void)) = yylex;

/*
 * cond	: cond_1 { SEPER } [ DBAR { SEPER } cond ]
 */

/**/
static int
par_cond(void)
{
    int p = ecused, r;

    r = par_cond_1();
    while (tok == SEPER)
	condlex();
    if (tok == DBAR) {
	condlex();
	while (tok == SEPER)
	    condlex();
	ecispace(p, 1);
	par_cond();
	ecbuf[p] = WCB_COND(COND_OR, ecused - 1 - p);
	return 1;
    }
    return r;
}

/*
 * cond_1 : cond_2 { SEPER } [ DAMPER { SEPER } cond_1 ]
 */

/**/
static int
par_cond_1(void)
{
    int r, p = ecused;

    r = par_cond_2();
    while (tok == SEPER)
	condlex();
    if (tok == DAMPER) {
	condlex();
	while (tok == SEPER)
	    condlex();
	ecispace(p, 1);
	par_cond_1();
	ecbuf[p] = WCB_COND(COND_AND, ecused - 1 - p);
	return 1;
    }
    return r;
}

/*
 * cond_2	: BANG cond_2
				| INPAR { SEPER } cond_2 { SEPER } OUTPAR
				| STRING STRING STRING
				| STRING STRING
				| STRING ( INANG | OUTANG ) STRING
 */

/**/
static int
par_cond_2(void)
{
    char *s1, *s2, *s3;
    int dble = 0;

    if (condlex == testlex) {
	/* See the description of test in POSIX 1003.2 */
	if (tok == NULLTOK)
	    /* no arguments: false */
	    return par_cond_double(dupstring("-n"), dupstring(""));
	if (!*testargs) {
	    /* one argument: [ foo ] is equivalent to [ -n foo ] */
	    s1 = tokstr;
	    condlex();
	    return par_cond_double(dupstring("-n"), s1);
	}
	if (testargs[1] && !testargs[2]) {
	    /* three arguments: if the second argument is a binary operator, *
	     * perform that binary test on the first and the trird argument  */
	    if (!strcmp(*testargs, "=")  ||
		!strcmp(*testargs, "==") ||
		!strcmp(*testargs, "!=") ||
		(**testargs == '-' && get_cond_num(*testargs + 1) >= 0)) {
		s1 = tokstr;
		condlex();
		s2 = tokstr;
		condlex();
		s3 = tokstr;
		condlex();
		return par_cond_triple(s1, s2, s3);
	    }
	}
    }
    if (tok == BANG) {
	condlex();
	ecadd(WCB_COND(COND_NOT, 0));
	return par_cond_2();
    }
    if (tok == INPAR) {
	int r;

	condlex();
	while (tok == SEPER)
	    condlex();
	r = par_cond();
	while (tok == SEPER)
	    condlex();
	if (tok != OUTPAR)
	    YYERROR(ecused);
	condlex();
	return r;
    }
    if (tok != STRING) {
	if (tok && tok != LEXERR && condlex == testlex) {
	    s1 = tokstr;
	    condlex();
	    return par_cond_double("-n", s1);
	} else
	    YYERROR(ecused);
    }
    s1 = tokstr;
    if (condlex == testlex)
	dble = (*s1 == '-' && strspn(s1+1, "abcdefghknoprstuwxzLONGS") == 1
		  && !s1[2]);
    condlex();
    if (tok == INANG || tok == OUTANG) {
	int xtok = tok;
	condlex();
	if (tok != STRING)
	    YYERROR(ecused);
	s3 = tokstr;
	condlex();
	ecadd(WCB_COND((xtok == INANG ? COND_STRLT : COND_STRGTR), 0));
	ecstr(s1);
	ecstr(s3);
	return 1;
    }
    if (tok != STRING) {
	if (tok != LEXERR && condlex == testlex) {
	    if (!dble)
		return par_cond_double("-n", s1);
	    else if (!strcmp(s1, "-t"))
		return par_cond_double(s1, "1");
	} else
	    YYERROR(ecused);
    }
    s2 = tokstr;
    incond++;			/* parentheses do globbing */
    condlex();
    incond--;			/* parentheses do grouping */
    if (tok == STRING && !dble) {
	s3 = tokstr;
	condlex();
	if (tok == STRING) {
	    LinkList l = newlinklist();

	    addlinknode(l, s2);
	    addlinknode(l, s3);

	    while (tok == STRING) {
		addlinknode(l, tokstr);
		condlex();
	    }
	    return par_cond_multi(s1, l);
	} else
	    return par_cond_triple(s1, s2, s3);
    } else
	return par_cond_double(s1, s2);
}

/**/
static int
par_cond_double(char *a, char *b)
{
    if (a[0] != '-' || !a[1])
	COND_ERROR("parse error: condition expected: %s", a);
    else if (!a[2] && strspn(a+1, "abcdefgknoprstuwxzhLONGS") == 1) {
	ecadd(WCB_COND(a[1], 0));
	ecstr(b);
    } else {
	ecadd(WCB_COND(COND_MOD, 1));
	ecstr(a);
	ecstr(b);
    }
    return 1;
}

/**/
static int
get_cond_num(char *tst)
{
    static char *condstrs[] =
    {
	"nt", "ot", "ef", "eq", "ne", "lt", "gt", "le", "ge", NULL
    };
    int t0;

    for (t0 = 0; condstrs[t0]; t0++)
	if (!strcmp(condstrs[t0], tst))
	    return t0;
    return -1;
}

/**/
static int
par_cond_triple(char *a, char *b, char *c)
{
    int t0;

    if ((b[0] == Equals || b[0] == '=') &&
	(!b[1] || ((b[1] == Equals || b[1] == '=') && !b[2]))) {
	ecadd(WCB_COND(COND_STREQ, 0));
	ecstr(a);
	ecstr(c);
	ecadd(ecnpats++);
    } else if (b[0] == '!' && (b[1] == Equals || b[1] == '=') && !b[2]) {
	ecadd(WCB_COND(COND_STRNEQ, 0));
	ecstr(a);
	ecstr(c);
	ecadd(ecnpats++);
    } else if (b[0] == '-') {
	if ((t0 = get_cond_num(b + 1)) > -1) {
	    ecadd(WCB_COND(t0 + COND_NT, 0));
	    ecstr(a);
	    ecstr(c);
	} else {
	    ecadd(WCB_COND(COND_MODI, 0));
	    ecstr(b);
	    ecstr(a);
	    ecstr(c);
	}
    } else if (a[0] == '-' && a[1]) {
	ecadd(WCB_COND(COND_MOD, 2));
	ecstr(a);
	ecstr(b);
	ecstr(c);
    } else
	COND_ERROR("condition expected: %s", b);

    return 1;
}

/**/
static int
par_cond_multi(char *a, LinkList l)
{
    if (a[0] != '-' || !a[1])
	COND_ERROR("condition expected: %s", a);
    else {
	LinkNode n;

	ecadd(WCB_COND(COND_MOD, countlinknodes(l)));
	ecstr(a);
	for (n = firstnode(l); n; incnode(n))
	    ecstr((char *) getdata(n));
    }
    return 1;
}

/**/
static void
yyerror(int noerr)
{
    int t0;
    char *t;

    if ((t = dupstring(yytext)))
	untokenize(t);

    for (t0 = 0; t0 != 20; t0++)
	if (!t || !t[t0] || t[t0] == '\n')
	    break;
    if (t0 == 20)
	zwarn("parse error near `%l...'", t, 20);
    else if (t0)
	zwarn("parse error near `%l'", t, t0);
    else
	zwarn("parse error", NULL, 0);
    if (!noerr && noerrs != 2)
	errflag = 1;
}

/**/
mod_export Eprog
zdupeprog(Eprog p)
{
    Eprog r;
    int i;
    Patprog *pp;

    if (p == &dummy_eprog)
	return p;

    r = (Eprog) zalloc(sizeof(*r));
    r->alloc = EA_REAL;
    r->dump = NULL;
    r->len = p->len;
    r->npats = p->npats;
    pp = r->pats = (Patprog *) zcalloc(r->len);
    r->prog = (Wordcode) (r->pats + r->npats);
    r->strs = ((char *) r->prog) + (p->strs - ((char *) p->prog));
    memcpy(r->prog, p->prog, r->len - (p->npats * sizeof(Patprog)));
    r->shf = NULL;

    for (i = r->npats; i--; pp++)
	*pp = dummy_patprog1;

    return r;
}

static LinkList eprog_free;

/**/
mod_export void
freeeprog(Eprog p)
{
    if (p && p != &dummy_eprog)
	zaddlinknode(eprog_free, p);
}

/**/
void
freeeprogs(void)
{
    Eprog p;
    int i;
    Patprog *pp;

    while ((p = (Eprog) getlinknode(eprog_free))) {
	for (i = p->npats, pp = p->pats; i--; pp++)
	    freepatprog(*pp);
	if (p->dump) {
	    decrdumpcount(p->dump);
	    zfree(p->pats, p->npats * sizeof(Patprog));
	} else
	    zfree(p->pats, p->len);
	zfree(p, sizeof(*p));
    }
}

/**/
char *
ecgetstr(Estate s, int dup, int *tok)
{
    static char buf[4];
    wordcode c = *s->pc++;
    char *r;

    if (c == 6 || c == 7)
	r = "";
    else if (c & 2) {
	buf[0] = (char) ((c >>  3) & 0xff);
	buf[1] = (char) ((c >> 11) & 0xff);
	buf[2] = (char) ((c >> 19) & 0xff);
	buf[3] = '\0';
	r = dupstring(buf);
	dup = EC_NODUP;
    } else {
	r = s->strs + (c >> 2);
    }
    if (tok)
	*tok = (c & 1);

    /*** Since function dump files are mapped read-only, avoiding to
     *   to duplicate strings when they don't contain tokens may fail
     *   when one of the many utility functions happens to write to
     *   one of the strings (without really modifying it).
     *   If that happens to you and you don't feel like debugging it,
     *   just change the line below to:
     *
     *     return (dup ? dupstring(r) : r);
     */

    return ((dup == EC_DUP || (dup && (c & 1)))  ? dupstring(r) : r);
}

/**/
char *
ecrawstr(Eprog p, Wordcode pc, int *tok)
{
    static char buf[4];
    wordcode c = *pc;

    if (c == 6 || c == 7) {
	if (tok)
	    *tok = (c & 1);
	return "";
    } else if (c & 2) {
	buf[0] = (char) ((c >>  3) & 0xff);
	buf[1] = (char) ((c >> 11) & 0xff);
	buf[2] = (char) ((c >> 19) & 0xff);
	buf[3] = '\0';
	if (tok)
	    *tok = (c & 1);
	return buf;
    } else {
	if (tok)
	    *tok = (c & 1);
	return p->strs + (c >> 2);
    }
}

/**/
char **
ecgetarr(Estate s, int num, int dup, int *tok)
{
    char **ret, **rp;
    int tf = 0, tmp = 0;

    ret = rp = (char **) zhalloc((num + 1) * sizeof(char *));

    while (num--) {
	*rp++ = ecgetstr(s, dup, &tmp);
	tf |=  tmp;
    }
    *rp = NULL;
    if (tok)
	*tok = tf;

    return ret;
}

/**/
LinkList
ecgetlist(Estate s, int num, int dup, int *tok)
{
    if (num) {
	LinkList ret;
	int i, tf = 0, tmp = 0;

	ret = newsizedlist(num);
	for (i = 0; i < num; i++) {
	    setsizednode(ret, i, ecgetstr(s, dup, &tmp));
	    tf |= tmp;
	}
	if (tok)
	    *tok = tf;
	return ret;
    }
    if (tok)
	*tok = 0;
    return NULL;
}

/**/
LinkList
ecgetredirs(Estate s)
{
    LinkList ret = newlinklist();
    wordcode code = *s->pc++;

    while (wc_code(code) == WC_REDIR) {
	Redir r = (Redir) zhalloc(sizeof(*r));

	r->type = WC_REDIR_TYPE(code);
	r->fd1 = *s->pc++;
	r->name = ecgetstr(s, EC_DUP, NULL);

	addlinknode(ret, r);

	code = *s->pc++;
    }
    s->pc--;

    return ret;
}

/**/
mod_export struct eprog dummy_eprog;

static wordcode dummy_eprog_code;

/**/
void
init_eprog(void)
{
    dummy_eprog_code = WCB_END();
    dummy_eprog.len = sizeof(wordcode);
    dummy_eprog.prog = &dummy_eprog_code;
    dummy_eprog.strs = NULL;

    eprog_free = znewlinklist();
}

/* Code for function dump files.
 *
 * Dump files consist of a header and the function bodies (the wordcode
 * plus the string table) and that twice: once for the byte-order of the
 * host the file was created on and once for the other byte-order. The
 * header describes where the beginning of the `other' version is and it
 * is up to the shell reading the file to decide which version it needs.
 * This is done by checking if the first word is FD_MAGIC (then the 
 * shell reading the file has the same byte order as the one that created
 * the file) or if it is FD_OMAGIC, then the `other' version has to be
 * read.
 * The header is the magic number, a word containing the flags (if the
 * file should be mapped or read and if this header is the `other' one),
 * the version string in a field of 40 characters and the descriptions
 * for the functions in the dump file.
 * Each description consists of a struct fdhead followed by the name,
 * aligned to sizeof(wordcode) (i.e. 4 bytes).
 */

#include "version.h"

#define FD_EXT ".zwc"
#define FD_MINMAP 4096

#define FD_PRELEN 12
#define FD_MAGIC  0x01020304
#define FD_OMAGIC 0x04030201

#define FDF_MAP   1
#define FDF_OTHER 2

typedef struct fdhead *FDHead;

struct fdhead {
    wordcode start;		/* offset to function definition */
    wordcode len;		/* length of wordcode/strings */
    wordcode npats;		/* number of patterns needed */
    wordcode strs;		/* offset to strings */
    wordcode hlen;		/* header length (incl. name) */
    wordcode tail;		/* offset to name tail */
};

#define fdheaderlen(f) (((Wordcode) (f))[FD_PRELEN])

#define fdmagic(f)       (((Wordcode) (f))[0])
#define fdbyte(f, i)     ((wordcode) (((unsigned char *) (((Wordcode) (f)) + 1))[i]))
#define fdflags(f)       fdbyte(f, 0)
#define fdother(f)       (fdbyte(f, 1) + (fdbyte(f, 2) << 8) + (fdbyte(f, 3) << 16))
#define fdsetother(f, o) \
    do { \
        fdbyte(f, 1) = (o & 0xff); \
        fdbyte(f, 2) = (o >> 8) & 0xff; \
        fdbyte(f, 3) = (o >> 16) & 0xff; \
    } while (0)
#define fdversion(f)     ((char *) ((f) + 2))

#define firstfdhead(f) ((FDHead) (((Wordcode) (f)) + FD_PRELEN))
#define nextfdhead(f)  ((FDHead) (((Wordcode) (f)) + (f)->hlen))

#define fdname(f)      ((char *) (((FDHead) (f)) + 1))

/* Try to find the description for the given function name. */

static FDHead
dump_find_func(Wordcode h, char *name)
{
    FDHead n, e = (FDHead) (h + fdheaderlen(h));

    for (n = firstfdhead(h); n < e; n = nextfdhead(n))
	if (!strcmp(name, fdname(n) + n->tail))
	    return n;

    return NULL;
}

/**/
int
bin_zcompile(char *nam, char **args, char *ops, int func)
{
    int map;

    if (ops['t']) {
	Wordcode f;

	if (!*args) {
	    zerrnam(nam, "too few arguments", NULL, 0);
	    return 1;
	}
	if (!(f = load_dump_header(*args))) {
	    zerrnam(nam, "invalid dump file: %s", *args, 0);
	    return 1;
	}
	if (args[1]) {
	    for (args++; *args; args++)
		if (!dump_find_func(f, *args))
		    return 1;
	    return 0;
	} else {
	    FDHead h, e = (FDHead) (f + fdheaderlen(f));

	    printf("function dump file (%s) for zsh-%s\n",
		   ((fdflags(f) & FDF_MAP) ? "mapped" : "read"), fdversion(f));
	    for (h = firstfdhead(f); h < e; h = nextfdhead(h))
		printf("%s\n", fdname(h));
	    return 0;
	}
    }
    if (!*args) {
	zerrnam(nam, "too few arguments", NULL, 0);
	return 1;
    }
    map = (ops['m'] ? 2 : (ops['r'] ? 0 : 1));

    if (!args[1])
	return build_dump(nam, dyncat(*args, FD_EXT), args, ops['U'], map);

    return build_dump(nam, *args, args + 1, ops['U'], map);
}

/* Load the header of a dump file. Returns NULL if the file isn't a
 * valid dump file. */

/**/
static Wordcode
load_dump_header(char *name)
{
    int fd;
    wordcode buf[FD_PRELEN + 1];

    if ((fd = open(name, O_RDONLY)) < 0)
	return NULL;

    if (read(fd, buf, (FD_PRELEN + 1) * sizeof(wordcode)) !=
	((FD_PRELEN + 1) * sizeof(wordcode)) ||
	strcmp(ZSH_VERSION, fdversion(buf))) {
	close(fd);
	return NULL;
    } else {
	int len;
	Wordcode head;

	if (fdmagic(buf) == FD_MAGIC) {
	    len = fdheaderlen(buf) * sizeof(wordcode);
	    head = (Wordcode) zhalloc(len);
	}
	else {
	    int o = fdother(buf);

	    if (lseek(fd, o, 0) == -1 ||
		read(fd, buf, (FD_PRELEN + 1) * sizeof(wordcode)) !=
		((FD_PRELEN + 1) * sizeof(wordcode))) {
		close(fd);
		return NULL;
	    }
	    len = fdheaderlen(buf) * sizeof(wordcode);
	    head = (Wordcode) zhalloc(len);
	}
	memcpy(head, buf, (FD_PRELEN + 1) * sizeof(wordcode));

	if (read(fd, head + (FD_PRELEN + 1),
		 len - ((FD_PRELEN + 1) * sizeof(wordcode))) !=
	    len - ((FD_PRELEN + 1) * sizeof(wordcode))) {
	    close(fd);
	    return NULL;
	}
	close(fd);
	return head;
    }
}

/* Swap the bytes in a wordcode. */

static void
fdswap(Wordcode p, int n)
{
    wordcode c;

    for (; n--; p++) {
	c = *p;
	*p = (((c & 0xff) << 24) |
	      ((c & 0xff00) << 8) |
	      ((c & 0xff0000) >> 8) |
	      ((c & 0xff000000) >> 24));
    }
}

/* Write a dump file. */

/**/
static int
build_dump(char *nam, char *dump, char **files, int ali, int map)
{
    int dfd, fd, hlen, tlen, flen, tmp, ona = noaliases, other = 0, ohlen;
    LinkList progs;
    LinkNode node;
    struct fdhead head;
    wordcode pre[FD_PRELEN];
    char *file, **ofiles = files, **oofiles = files, *name, *tail;
    Eprog prog;

    if ((dfd = open(dump, O_WRONLY|O_CREAT, 0600)) < 0) {
	zerrnam(nam, "can't write dump file: %s", dump, 0);
	return 1;
    }
    progs = newlinklist();
    noaliases = ali;

    for (hlen = FD_PRELEN, tlen = 0; *files; files++) {
	if ((fd = open(*files, O_RDONLY)) < 0 ||
	    (flen = lseek(fd, 0, 2)) == -1) {
	    if (fd >= 0)
		close(fd);
	    close(dfd);
	    zerrnam(nam, "can't open file: %s", *files, 0);
	    noaliases = ona;
	    return 1;
	}
	file = (char *) zalloc(flen + 1);
	file[flen] = '\0';
	lseek(fd, 0, 0);
	if (read(fd, file, flen) != flen) {
	    close(fd);
	    close(dfd);
	    zfree(file, flen);
	    zerrnam(nam, "can't read file: %s", *files, 0);
	    noaliases = ona;
	    return 1;
	}
	close(fd);
	file = metafy(file, flen, META_REALLOC);

	if (!(prog = parse_string(file, 1)) || errflag) {
	    close(dfd);
	    zfree(file, flen);
	    zerrnam(nam, "can't read file: %s", *files, 0);
	    noaliases = ona;
	    return 1;
	}
	zfree(file, flen);

	addlinknode(progs, prog);

	flen = (strlen(*files) + sizeof(wordcode)) / sizeof(wordcode);
	hlen += (sizeof(head) / sizeof(wordcode)) + flen;

	tlen += (prog->len - (prog->npats * sizeof(Patprog)) +
		 sizeof(wordcode) - 1) / sizeof(wordcode);
    }
    noaliases = ona;

    tlen = (tlen + hlen) * sizeof(wordcode);
    if (map == 1)
	map = (tlen >= FD_MINMAP);

    for (ohlen = hlen; ; hlen = ohlen) {
	fdmagic(pre) = (other ? FD_OMAGIC : FD_MAGIC);
	fdflags(pre) = (map ? FDF_MAP : 0) | other;
	fdsetother(pre, tlen);
	strcpy(fdversion(pre), ZSH_VERSION);
	write(dfd, pre, FD_PRELEN * sizeof(wordcode));

	for (node = firstnode(progs), ofiles = oofiles; node;
	     ofiles++, incnode(node)) {
	    prog = (Eprog) getdata(node);
	    head.start = hlen;
	    hlen += (prog->len - (prog->npats * sizeof(Patprog)) +
		     sizeof(wordcode) - 1) / sizeof(wordcode);
	    head.len = prog->len - (prog->npats * sizeof(Patprog));
	    head.npats = prog->npats;
	    head.strs = prog->strs - ((char *) prog->prog);
	    head.hlen = (sizeof(struct fdhead) / sizeof(wordcode)) +
		(strlen(*ofiles) + sizeof(wordcode)) / sizeof(wordcode);
	    for (name = tail = *ofiles; *name; name++)
		if (*name == '/')
		    tail = name + 1;
	    head.tail = tail - *ofiles;
	    if (other)
		fdswap((Wordcode) &head, sizeof(head) / sizeof(wordcode));
	    write(dfd, &head, sizeof(head));
	    tmp = strlen(*ofiles) + 1;
	    write(dfd, *ofiles, tmp);
	    if ((tmp &= (sizeof(wordcode) - 1)))
		write(dfd, &head, sizeof(wordcode) - tmp);
	}
	for (node = firstnode(progs); node; incnode(node)) {
	    prog = (Eprog) getdata(node);
	    tmp = (prog->len - (prog->npats * sizeof(Patprog)) +
		   sizeof(wordcode) - 1) / sizeof(wordcode);
	    if (other)
		fdswap(prog->prog, (((Wordcode) prog->strs) - prog->prog));
	    write(dfd, prog->prog, tmp * sizeof(wordcode));
	}
	if (other)
	    break;
	other = FDF_OTHER;
    }
    close(dfd);
    return 0;
}

#if defined(HAVE_SYS_MMAN_H) && defined(HAVE_MMAP) && defined(HAVE_MUNMAP)

#include <sys/mman.h>

#if defined(MAP_SHARED) && defined(PROT_READ)

#define USE_MMAP 1

#endif
#endif

#ifdef USE_MMAP

/* List of dump files mapped. */

static FuncDump dumps;

/* Load a dump file (i.e. map it). */

static void
load_dump_file(char *dump, int other, int len)
{
    FuncDump d;
    Wordcode addr;
    int fd, off;

    if (other) {
	static size_t pgsz = 0;

	if (!pgsz) {

#ifdef _SC_PAGESIZE
	    pgsz = sysconf(_SC_PAGESIZE);     /* SVR4 */
#else
# ifdef _SC_PAGE_SIZE
	    pgsz = sysconf(_SC_PAGE_SIZE);    /* HPUX */
# else
	    pgsz = getpagesize();
# endif
#endif

	    pgsz--;
	}
	off = len & ~pgsz;
    } else
	off = 0;

    if ((fd = open(dump, O_RDONLY)) < 0)
	return;

    fd = movefd(fd);

    if ((addr = (Wordcode) mmap(NULL, len, PROT_READ, MAP_SHARED, fd, off)) ==
	((Wordcode) -1)) {
	close(fd);
	return;
    }
    d = (FuncDump) zalloc(sizeof(*d));
    d->next = dumps;
    dumps = d;
    d->name = ztrdup(dump);
    d->fd = fd;
    d->map = addr + (other ? (len - off) / sizeof(wordcode) : 0);
    d->addr = addr;
    d->len = len;
    d->count = 0;
}

/* See if `dump' is the name of a dump file and it has the definition
 * for the function `name'. If so, return an eprog for it. */

/**/
Eprog
try_dump_file(char *dump, char *name, char *func)
{
    int isrec = 0;
    Wordcode d;
    FDHead h;
    FuncDump f;

 rec:

    d = NULL;
    for (f = dumps; f; f = f->next)
	if (!strcmp(dump, f->name)) {
	    d = f->map;
	    break;
	}
    if (!f && (isrec || !(d = load_dump_header(dump)))) {
	if (!isrec) {
	    struct stat stc, stn;
	    char *p = (char *) zhalloc(strlen(dump) + strlen(name) +
				       strlen(FD_EXT) + 2);

	    sprintf(p, "%s/%s%s", dump, name, FD_EXT);

	    /* Ignore the dump file if it is older than the normal one. */
	    if (stat(p, &stc) || stat(func, &stn) || stn.st_mtime > stc.st_mtime)
		return NULL;

	    if (!(d = load_dump_header(dump = p)))
		return NULL;

	} else
	    return NULL;
    }
    if ((h = dump_find_func(d, name))) {
	/* Found the name. If the file is already mapped, return the eprog,
	 * otherwise map it and just go up. */
	if (f) {
	    Eprog prog = (Eprog) zalloc(sizeof(*prog));
	    Patprog *pp;
	    int np;

	    prog->alloc = EA_MAP;
	    prog->len = h->len;
	    prog->npats = np = h->npats;
	    prog->pats = pp = (Patprog *) zalloc(np * sizeof(Patprog));
	    prog->prog = f->map + h->start;
	    prog->strs = ((char *) prog->prog) + h->strs;
	    prog->shf = NULL;
	    prog->dump = f;

	    incrdumpcount(f);

	    while (np--)
		*pp++ = dummy_patprog1;

	    return prog;
	} else if (fdflags(d) & FDF_MAP) {
	    load_dump_file(dump, (fdflags(d) & FDF_OTHER), fdother(d));
	    isrec = 1;
	    goto rec;
	} else {
	    Eprog prog;
	    Patprog *pp;
	    int np, fd, po = h->npats * sizeof(Patprog);

	    if ((fd = open(dump, O_RDONLY)) < 0 ||
		lseek(fd, ((h->start * sizeof(wordcode)) +
			   ((fdflags(d) & FDF_OTHER) ? fdother(d) : 0)), 0) < 0) {
		if (fd >= 0)
		    close(fd);
		return NULL;
	    }
	    d = (Wordcode) zalloc(h->len + po);

	    if (read(fd, ((char *) d) + po, h->len) != h->len) {
		close(fd);
		zfree(d, h->len);

		return NULL;
	    }
	    close(fd);

	    prog = (Eprog) zalloc(sizeof(*prog));

	    prog->alloc = EA_REAL;
	    prog->len = h->len + po;
	    prog->npats = np = h->npats;
	    prog->pats = pp = (Patprog *) d;
	    prog->prog = (Wordcode) (((char *) d) + po);
	    prog->strs = ((char *) prog->prog) + h->strs;
	    prog->shf = NULL;
	    prog->dump = f;

	    while (np--)
		*pp++ = dummy_patprog1;

	    return prog;
	}
    }
    return NULL;
}

/* Increment the reference counter for a dump file. */

/**/
void
incrdumpcount(FuncDump f)
{
    f->count++;
}

/* Decrement the reference counter for a dump file. If zero, unmap the file. */

/**/
void
decrdumpcount(FuncDump f)
{
    f->count--;
    if (!f->count) {
	FuncDump p, q;

	for (q = NULL, p = dumps; p && p != f; q = p, p = p->next);
	if (p) {
	    if (q)
		q->next = p->next;
	    else
		dumps = p->next;
	    munmap((void *) f->addr, f->len);
	    zclose(f->fd);
	    zfree(f, sizeof(*f));
	}
    }
}

#else

Eprog
try_dump_file(char *dump, char *name, char *func)
{
    return NULL;
}

void
incrdumpcount(FuncDump f)
{
}

void
decrdumpcount(FuncDump f)
{
}

#endif
