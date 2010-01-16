/* vi: set sw=4 ts=4: */
/*
 * Mini find implementation for busybox
 *
 * Copyright (C) 1999-2004 by Erik Andersen <andersen@codepoet.org>
 *
 * Reworked by David Douthitt <n9ubh@callsign.net> and
 *  Matt Kraai <kraai@alumni.carnegiemellon.edu>.
 *
 * Licensed under the GPL version 2, see the file LICENSE in this tarball.
 */

/* findutils-4.1.20:
 *
 * # find file.txt -exec 'echo {}' '{}  {}' ';'
 * find: echo file.txt: No such file or directory
 * # find file.txt -exec 'echo' '{}  {}' '; '
 * find: missing argument to `-exec'
 * # find file.txt -exec 'echo {}' '{}  {}' ';' junk
 * find: paths must precede expression
 * # find file.txt -exec 'echo {}' '{}  {}' ';' junk ';'
 * find: paths must precede expression
 * # find file.txt -exec 'echo' '{}  {}' ';'
 * file.txt  file.txt
 * (strace: execve("/bin/echo", ["echo", "file.txt  file.txt"], [ 30 vars ]))
 * # find file.txt -exec 'echo' '{}  {}' ';' -print -exec pwd ';'
 * file.txt  file.txt
 * file.txt
 * /tmp
 * # find -name '*.c' -o -name '*.h'
 * [shows files, *.c and *.h intermixed]
 * # find file.txt -name '*f*' -o -name '*t*'
 * file.txt
 * # find file.txt -name '*z*' -o -name '*t*'
 * file.txt
 * # find file.txt -name '*f*' -o -name '*z*'
 * file.txt
 *
 * # find t z -name '*t*' -print -o -name '*z*'
 * t
 * # find t z t z -name '*t*' -o -name '*z*' -print
 * z
 * z
 * # find t z t z '(' -name '*t*' -o -name '*z*' ')' -o -print
 * (no output)
 */

/* Testing script
 * ./busybox find "$@" | tee /tmp/bb_find
 * echo ==================
 * /path/to/gnu/find "$@" | tee /tmp/std_find
 * echo ==================
 * diff -u /tmp/std_find /tmp/bb_find && echo Identical
 */

#include <fnmatch.h>
#include "libbb.h"
#if ENABLE_FEATURE_FIND_REGEX
#include "xregex.h"
#endif

/* This is a NOEXEC applet. Be very careful! */


typedef int (*action_fp)(const char *fileName, const struct stat *statbuf, void *) FAST_FUNC;

typedef struct {
	action_fp f;
#if ENABLE_FEATURE_FIND_NOT
	bool invert;
#endif
} action;

#define ACTS(name, ...) typedef struct { action a; __VA_ARGS__ } action_##name;
#define ACTF(name) \
	static int FAST_FUNC func_##name(const char *fileName UNUSED_PARAM, \
		const struct stat *statbuf UNUSED_PARAM, \
		action_##name* ap UNUSED_PARAM)

                        ACTS(print)
                        ACTS(name,  const char *pattern; bool iname;)
IF_FEATURE_FIND_PATH(   ACTS(path,  const char *pattern;))
IF_FEATURE_FIND_REGEX(  ACTS(regex, regex_t compiled_pattern;))
IF_FEATURE_FIND_PRINT0( ACTS(print0))
IF_FEATURE_FIND_TYPE(   ACTS(type,  int type_mask;))
IF_FEATURE_FIND_PERM(   ACTS(perm,  char perm_char; mode_t perm_mask;))
IF_FEATURE_FIND_MTIME(  ACTS(mtime, char mtime_char; unsigned mtime_days;))
IF_FEATURE_FIND_MMIN(   ACTS(mmin,  char mmin_char; unsigned mmin_mins;))
IF_FEATURE_FIND_NEWER(  ACTS(newer, time_t newer_mtime;))
IF_FEATURE_FIND_INUM(   ACTS(inum,  ino_t inode_num;))
IF_FEATURE_FIND_USER(   ACTS(user,  uid_t uid;))
IF_FEATURE_FIND_SIZE(   ACTS(size,  char size_char; off_t size;))
IF_FEATURE_FIND_CONTEXT(ACTS(context, security_context_t context;))
IF_FEATURE_FIND_PAREN(  ACTS(paren, action ***subexpr;))
IF_FEATURE_FIND_PRUNE(  ACTS(prune))
IF_FEATURE_FIND_DELETE( ACTS(delete))
IF_FEATURE_FIND_EXEC(   ACTS(exec,  char **exec_argv; unsigned *subst_count; int exec_argc;))
IF_FEATURE_FIND_GROUP(  ACTS(group, gid_t gid;))
IF_FEATURE_FIND_LINKS(  ACTS(links, char links_char; int links_count;))

struct globals {
	IF_FEATURE_FIND_XDEV(dev_t *xdev_dev;)
	IF_FEATURE_FIND_XDEV(int xdev_count;)
	action ***actions;
	bool need_print;
	recurse_flags_t recurse_flags;
};
#define G (*(struct globals*)&bb_common_bufsiz1)
#define INIT_G() do { \
	struct G_sizecheck { \
		char G_sizecheck[sizeof(G) > COMMON_BUFSIZE ? -1 : 1]; \
	}; \
	/* we have to zero it out because of NOEXEC */ \
	memset(&G, 0, offsetof(struct globals, need_print)); \
	G.need_print = 1; \
	G.recurse_flags = ACTION_RECURSE; \
} while (0)

#if ENABLE_FEATURE_FIND_EXEC
static unsigned count_subst(const char *str)
{
	unsigned count = 0;
	while ((str = strstr(str, "{}")) != NULL) {
		count++;
		str++;
	}
	return count;
}


static char* subst(const char *src, unsigned count, const char* filename)
{
	char *buf, *dst, *end;
	size_t flen = strlen(filename);
	/* we replace each '{}' with filename: growth by strlen-2 */
	buf = dst = xmalloc(strlen(src) + count*(flen-2) + 1);
	while ((end = strstr(src, "{}"))) {
		memcpy(dst, src, end - src);
		dst += end - src;
		src = end + 2;
		memcpy(dst, filename, flen);
		dst += flen;
	}
	strcpy(dst, src);
	return buf;
}
#endif

/* Return values of ACTFs ('action functions') are a bit mask:
 * bit 1=1: prune (use SKIP constant for setting it)
 * bit 0=1: matched successfully (TRUE)
 */

static int exec_actions(action ***appp, const char *fileName, const struct stat *statbuf)
{
	int cur_group;
	int cur_action;
	int rc = 0;
	action **app, *ap;

	/* "action group" is a set of actions ANDed together.
	 * groups are ORed together.
	 * We simply evaluate each group until we find one in which all actions
	 * succeed. */

	/* -prune is special: if it is encountered, then we won't
	 * descend into current directory. It doesn't matter whether
	 * action group (in which -prune sits) will succeed or not:
	 * find * -prune -name 'f*' -o -name 'm*' -- prunes every dir
	 * find * -name 'f*' -o -prune -name 'm*' -- prunes all dirs
	 *     not starting with 'f' */

	/* We invert TRUE bit (bit 0). Now 1 there means 'failure'.
	 * and bitwise OR in "rc |= TRUE ^ ap->f()" will:
	 * (1) make SKIP (-prune) bit stick; and (2) detect 'failure'.
	 * On return, bit is restored.  */

	cur_group = -1;
	while ((app = appp[++cur_group]) != NULL) {
		rc &= ~TRUE; /* 'success' so far, clear TRUE bit */
		cur_action = -1;
		while (1) {
			ap = app[++cur_action];
			if (!ap) /* all actions in group were successful */
				return rc ^ TRUE; /* restore TRUE bit */
			rc |= TRUE ^ ap->f(fileName, statbuf, ap);
#if ENABLE_FEATURE_FIND_NOT
			if (ap->invert) rc ^= TRUE;
#endif
			if (rc & TRUE) /* current group failed, try next */
				break;
		}
	}
	return rc ^ TRUE; /* restore TRUE bit */
}


ACTF(name)
{
	const char *tmp = bb_basename(fileName);
	if (tmp != fileName && !*tmp) { /* "foo/bar/". Oh no... go back to 'b' */
		tmp--;
		while (tmp != fileName && *--tmp != '/')
			continue;
		if (*tmp == '/')
			tmp++;
	}
	return fnmatch(ap->pattern, tmp, FNM_PERIOD | (ap->iname ? FNM_CASEFOLD : 0)) == 0;
}

#if ENABLE_FEATURE_FIND_PATH
ACTF(path)
{
	return fnmatch(ap->pattern, fileName, 0) == 0;
}
#endif
#if ENABLE_FEATURE_FIND_REGEX
ACTF(regex)
{
	regmatch_t match;
	if (regexec(&ap->compiled_pattern, fileName, 1, &match, 0 /*eflags*/))
		return 0; /* no match */
	if (match.rm_so)
		return 0; /* match doesn't start at pos 0 */
	if (fileName[match.rm_eo])
		return 0; /* match doesn't end exactly at end of pathname */
	return 1;
}
#endif
#if ENABLE_FEATURE_FIND_TYPE
ACTF(type)
{
	return ((statbuf->st_mode & S_IFMT) == ap->type_mask);
}
#endif
#if ENABLE_FEATURE_FIND_PERM
ACTF(perm)
{
	/* -perm +mode: at least one of perm_mask bits are set */
	if (ap->perm_char == '+')
		return (statbuf->st_mode & ap->perm_mask) != 0;
	/* -perm -mode: all of perm_mask are set */
	if (ap->perm_char == '-')
		return (statbuf->st_mode & ap->perm_mask) == ap->perm_mask;
	/* -perm mode: file mode must match perm_mask */
	return (statbuf->st_mode & 07777) == ap->perm_mask;
}
#endif
#if ENABLE_FEATURE_FIND_MTIME
ACTF(mtime)
{
	time_t file_age = time(NULL) - statbuf->st_mtime;
	time_t mtime_secs = ap->mtime_days * 24*60*60;
	if (ap->mtime_char == '+')
		return file_age >= mtime_secs + 24*60*60;
	if (ap->mtime_char == '-')
		return file_age < mtime_secs;
	/* just numeric mtime */
	return file_age >= mtime_secs && file_age < (mtime_secs + 24*60*60);
}
#endif
#if ENABLE_FEATURE_FIND_MMIN
ACTF(mmin)
{
	time_t file_age = time(NULL) - statbuf->st_mtime;
	time_t mmin_secs = ap->mmin_mins * 60;
	if (ap->mmin_char == '+')
		return file_age >= mmin_secs + 60;
	if (ap->mmin_char == '-')
		return file_age < mmin_secs;
	/* just numeric mmin */
	return file_age >= mmin_secs && file_age < (mmin_secs + 60);
}
#endif
#if ENABLE_FEATURE_FIND_NEWER
ACTF(newer)
{
	return (ap->newer_mtime < statbuf->st_mtime);
}
#endif
#if ENABLE_FEATURE_FIND_INUM
ACTF(inum)
{
	return (statbuf->st_ino == ap->inode_num);
}
#endif
#if ENABLE_FEATURE_FIND_EXEC
ACTF(exec)
{
	int i, rc;
#if ENABLE_USE_PORTABLE_CODE
	char **argv = alloca(sizeof(char*) * (ap->exec_argc + 1));
#else /* gcc 4.3.1 generates smaller code: */
	char *argv[ap->exec_argc + 1];
#endif
	for (i = 0; i < ap->exec_argc; i++)
		argv[i] = subst(ap->exec_argv[i], ap->subst_count[i], fileName);
	argv[i] = NULL; /* terminate the list */

	rc = spawn_and_wait(argv);
	if (rc < 0)
		bb_simple_perror_msg(argv[0]);

	i = 0;
	while (argv[i])
		free(argv[i++]);
	return rc == 0; /* return 1 if exitcode 0 */
}
#endif
#if ENABLE_FEATURE_FIND_USER
ACTF(user)
{
	return (statbuf->st_uid == ap->uid);
}
#endif
#if ENABLE_FEATURE_FIND_GROUP
ACTF(group)
{
	return (statbuf->st_gid == ap->gid);
}
#endif
#if ENABLE_FEATURE_FIND_PRINT0
ACTF(print0)
{
	printf("%s%c", fileName, '\0');
	return TRUE;
}
#endif
ACTF(print)
{
	puts(fileName);
	return TRUE;
}
#if ENABLE_FEATURE_FIND_PAREN
ACTF(paren)
{
	return exec_actions(ap->subexpr, fileName, statbuf);
}
#endif
#if ENABLE_FEATURE_FIND_SIZE
ACTF(size)
{
	if (ap->size_char == '+')
		return statbuf->st_size > ap->size;
	if (ap->size_char == '-')
		return statbuf->st_size < ap->size;
	return statbuf->st_size == ap->size;
}
#endif
#if ENABLE_FEATURE_FIND_PRUNE
/*
 * -prune: if -depth is not given, return true and do not descend
 * current dir; if -depth is given, return false with no effect.
 * Example:
 * find dir -name 'asm-*' -prune -o -name '*.[chS]' -print
 */
ACTF(prune)
{
	return SKIP + TRUE;
}
#endif
#if ENABLE_FEATURE_FIND_DELETE
ACTF(delete)
{
	int rc;
	if (S_ISDIR(statbuf->st_mode)) {
		rc = rmdir(fileName);
	} else {
		rc = unlink(fileName);
	}
	if (rc < 0)
		bb_simple_perror_msg(fileName);
	return TRUE;
}
#endif
#if ENABLE_FEATURE_FIND_CONTEXT
ACTF(context)
{
	security_context_t con;
	int rc;

	if (G.recurse_flags & ACTION_FOLLOWLINKS) {
		rc = getfilecon(fileName, &con);
	} else {
		rc = lgetfilecon(fileName, &con);
	}
	if (rc < 0)
		return FALSE;
	rc = strcmp(ap->context, con);
	freecon(con);
	return rc == 0;
}
#endif
#if ENABLE_FEATURE_FIND_LINKS
ACTF(links)
{
	switch(ap->links_char) {
	case '-' : return (statbuf->st_nlink <  ap->links_count);
	case '+' : return (statbuf->st_nlink >  ap->links_count);
	default:   return (statbuf->st_nlink == ap->links_count);
	}
}
#endif

static int FAST_FUNC fileAction(const char *fileName,
		struct stat *statbuf,
		void *userData IF_NOT_FEATURE_FIND_MAXDEPTH(UNUSED_PARAM),
		int depth IF_NOT_FEATURE_FIND_MAXDEPTH(UNUSED_PARAM))
{
	int r;
#if ENABLE_FEATURE_FIND_MAXDEPTH
#define minmaxdepth ((int*)userData)

	if (depth < minmaxdepth[0])
		return TRUE; /* skip this, continue recursing */
	if (depth > minmaxdepth[1])
		return SKIP; /* stop recursing */
#endif

	r = exec_actions(G.actions, fileName, statbuf);
	/* Had no explicit -print[0] or -exec? then print */
	if ((r & TRUE) && G.need_print)
		puts(fileName);

#if ENABLE_FEATURE_FIND_MAXDEPTH
	if (S_ISDIR(statbuf->st_mode)) {
		if (depth == minmaxdepth[1])
			return SKIP;
	}
#endif
#if ENABLE_FEATURE_FIND_XDEV
	/* -xdev stops on mountpoints, but AFTER mountpoit itself
	 * is processed as usual */
	if (S_ISDIR(statbuf->st_mode)) {
		if (G.xdev_count) {
			int i;
			for (i = 0; i < G.xdev_count; i++) {
				if (G.xdev_dev[i] == statbuf->st_dev)
					goto found;
			}
			return SKIP;
 found: ;
		}
	}
#endif

	/* Cannot return 0: our caller, recursive_action(),
	 * will perror() and skip dirs (if called on dir) */
	return (r & SKIP) ? SKIP : TRUE;
#undef minmaxdepth
}


#if ENABLE_FEATURE_FIND_TYPE
static int find_type(const char *type)
{
	int mask = 0;

	if (*type == 'b')
		mask = S_IFBLK;
	else if (*type == 'c')
		mask = S_IFCHR;
	else if (*type == 'd')
		mask = S_IFDIR;
	else if (*type == 'p')
		mask = S_IFIFO;
	else if (*type == 'f')
		mask = S_IFREG;
	else if (*type == 'l')
		mask = S_IFLNK;
	else if (*type == 's')
		mask = S_IFSOCK;

	if (mask == 0 || type[1] != '\0')
		bb_error_msg_and_die(bb_msg_invalid_arg, type, "-type");

	return mask;
}
#endif

#if ENABLE_FEATURE_FIND_PERM \
 || ENABLE_FEATURE_FIND_MTIME || ENABLE_FEATURE_FIND_MMIN \
 || ENABLE_FEATURE_FIND_SIZE  || ENABLE_FEATURE_FIND_LINKS
static const char* plus_minus_num(const char* str)
{
	if (*str == '-' || *str == '+')
		str++;
	return str;
}
#endif

static action*** parse_params(char **argv)
{
	enum {
	                        PARM_a         ,
	                        PARM_o         ,
	IF_FEATURE_FIND_NOT(	PARM_char_not  ,)
#if ENABLE_DESKTOP
	                        PARM_and       ,
	                        PARM_or        ,
	IF_FEATURE_FIND_NOT(    PARM_not       ,)
#endif
	                        PARM_print     ,
	IF_FEATURE_FIND_PRINT0( PARM_print0    ,)
	IF_FEATURE_FIND_DEPTH(  PARM_depth     ,)
	IF_FEATURE_FIND_PRUNE(  PARM_prune     ,)
	IF_FEATURE_FIND_DELETE( PARM_delete    ,)
	IF_FEATURE_FIND_EXEC(   PARM_exec      ,)
	IF_FEATURE_FIND_PAREN(  PARM_char_brace,)
	/* All options starting from here require argument */
	                        PARM_name      ,
	                        PARM_iname     ,
	IF_FEATURE_FIND_PATH(   PARM_path      ,)
	IF_FEATURE_FIND_REGEX(  PARM_regex     ,)
	IF_FEATURE_FIND_TYPE(   PARM_type      ,)
	IF_FEATURE_FIND_PERM(   PARM_perm      ,)
	IF_FEATURE_FIND_MTIME(  PARM_mtime     ,)
	IF_FEATURE_FIND_MMIN(   PARM_mmin      ,)
	IF_FEATURE_FIND_NEWER(  PARM_newer     ,)
	IF_FEATURE_FIND_INUM(   PARM_inum      ,)
	IF_FEATURE_FIND_USER(   PARM_user      ,)
	IF_FEATURE_FIND_GROUP(  PARM_group     ,)
	IF_FEATURE_FIND_SIZE(   PARM_size      ,)
	IF_FEATURE_FIND_CONTEXT(PARM_context   ,)
	IF_FEATURE_FIND_LINKS(  PARM_links     ,)
	};

	static const char params[] ALIGN1 =
	                         "-a\0"
	                         "-o\0"
	IF_FEATURE_FIND_NOT(    "!\0"       )
#if ENABLE_DESKTOP
	                         "-and\0"
	                         "-or\0"
	IF_FEATURE_FIND_NOT(	 "-not\0"    )
#endif
	                         "-print\0"
	IF_FEATURE_FIND_PRINT0( "-print0\0" )
	IF_FEATURE_FIND_DEPTH(  "-depth\0"  )
	IF_FEATURE_FIND_PRUNE(  "-prune\0"  )
	IF_FEATURE_FIND_DELETE( "-delete\0" )
	IF_FEATURE_FIND_EXEC(   "-exec\0"   )
	IF_FEATURE_FIND_PAREN(  "(\0"       )
	/* All options starting from here require argument */
	                         "-name\0"
	                         "-iname\0"
	IF_FEATURE_FIND_PATH(   "-path\0"   )
	IF_FEATURE_FIND_REGEX(  "-regex\0"  )
	IF_FEATURE_FIND_TYPE(   "-type\0"   )
	IF_FEATURE_FIND_PERM(   "-perm\0"   )
	IF_FEATURE_FIND_MTIME(  "-mtime\0"  )
	IF_FEATURE_FIND_MMIN(   "-mmin\0"   )
	IF_FEATURE_FIND_NEWER(  "-newer\0"  )
	IF_FEATURE_FIND_INUM(   "-inum\0"   )
	IF_FEATURE_FIND_USER(   "-user\0"   )
	IF_FEATURE_FIND_GROUP(  "-group\0"  )
	IF_FEATURE_FIND_SIZE(   "-size\0"   )
	IF_FEATURE_FIND_CONTEXT("-context\0")
	IF_FEATURE_FIND_LINKS(  "-links\0"  )
	                         ;

	action*** appp;
	unsigned cur_group = 0;
	unsigned cur_action = 0;
	IF_FEATURE_FIND_NOT( bool invert_flag = 0; )

	/* This is the only place in busybox where we use nested function.
	 * So far more standard alternatives were bigger. */
	/* Suppress a warning "func without a prototype" */
	auto action* alloc_action(int sizeof_struct, action_fp f);
	action* alloc_action(int sizeof_struct, action_fp f)
	{
		action *ap;
		appp[cur_group] = xrealloc(appp[cur_group], (cur_action+2) * sizeof(*appp));
		appp[cur_group][cur_action++] = ap = xmalloc(sizeof_struct);
		appp[cur_group][cur_action] = NULL;
		ap->f = f;
		IF_FEATURE_FIND_NOT( ap->invert = invert_flag; )
		IF_FEATURE_FIND_NOT( invert_flag = 0; )
		return ap;
	}

#define ALLOC_ACTION(name) (action_##name*)alloc_action(sizeof(action_##name), (action_fp) func_##name)

	appp = xzalloc(2 * sizeof(appp[0])); /* appp[0],[1] == NULL */

/* Actions have side effects and return a true or false value
 * We implement: -print, -print0, -exec
 *
 * The rest are tests.
 *
 * Tests and actions are grouped by operators
 * ( expr )              Force precedence
 * ! expr                True if expr is false
 * -not expr             Same as ! expr
 * expr1 [-a[nd]] expr2  And; expr2 is not evaluated if expr1 is false
 * expr1 -o[r] expr2     Or; expr2 is not evaluated if expr1 is true
 * expr1 , expr2         List; both expr1 and expr2 are always evaluated
 * We implement: (), -a, -o
 */
	while (*argv) {
		const char *arg = argv[0];
		int parm = index_in_strings(params, arg);
		const char *arg1 = argv[1];

		if (parm >= PARM_name) {
			/* All options starting from -name require argument */
			if (!arg1)
				bb_error_msg_and_die(bb_msg_requires_arg, arg);
			argv++;
		}

		/* We can use big switch() here, but on i386
		 * it doesn't give smaller code. Other arches? */

	/* --- Operators --- */
		if (parm == PARM_a IF_DESKTOP(|| parm == PARM_and)) {
			/* no further special handling required */
		}
		else if (parm == PARM_o IF_DESKTOP(|| parm == PARM_or)) {
			/* start new OR group */
			cur_group++;
			appp = xrealloc(appp, (cur_group+2) * sizeof(*appp));
			/*appp[cur_group] = NULL; - already NULL */
			appp[cur_group+1] = NULL;
			cur_action = 0;
		}
#if ENABLE_FEATURE_FIND_NOT
		else if (parm == PARM_char_not IF_DESKTOP(|| parm == PARM_not)) {
			/* also handles "find ! ! -name 'foo*'" */
			invert_flag ^= 1;
		}
#endif

	/* --- Tests and actions --- */
		else if (parm == PARM_print) {
			G.need_print = 0;
			/* GNU find ignores '!' here: "find ! -print" */
			IF_FEATURE_FIND_NOT( invert_flag = 0; )
			(void) ALLOC_ACTION(print);
		}
#if ENABLE_FEATURE_FIND_PRINT0
		else if (parm == PARM_print0) {
			G.need_print = 0;
			IF_FEATURE_FIND_NOT( invert_flag = 0; )
			(void) ALLOC_ACTION(print0);
		}
#endif
#if ENABLE_FEATURE_FIND_DEPTH
		else if (parm == PARM_depth) {
			G.recurse_flags |= ACTION_DEPTHFIRST;
		}
#endif
#if ENABLE_FEATURE_FIND_PRUNE
		else if (parm == PARM_prune) {
			IF_FEATURE_FIND_NOT( invert_flag = 0; )
			(void) ALLOC_ACTION(prune);
		}
#endif
#if ENABLE_FEATURE_FIND_DELETE
		else if (parm == PARM_delete) {
			G.need_print = 0;
			G.recurse_flags |= ACTION_DEPTHFIRST;
			(void) ALLOC_ACTION(delete);
		}
#endif
#if ENABLE_FEATURE_FIND_EXEC
		else if (parm == PARM_exec) {
			int i;
			action_exec *ap;
			G.need_print = 0;
			IF_FEATURE_FIND_NOT( invert_flag = 0; )
			ap = ALLOC_ACTION(exec);
			ap->exec_argv = ++argv; /* first arg after -exec */
			ap->exec_argc = 0;
			while (1) {
				if (!*argv) /* did not see ';' until end */
					bb_error_msg_and_die("-exec CMD must end by ';'");
				if (LONE_CHAR(argv[0], ';'))
					break;
				argv++;
				ap->exec_argc++;
			}
			if (ap->exec_argc == 0)
				bb_error_msg_and_die(bb_msg_requires_arg, arg);
			ap->subst_count = xmalloc(ap->exec_argc * sizeof(int));
			i = ap->exec_argc;
			while (i--)
				ap->subst_count[i] = count_subst(ap->exec_argv[i]);
		}
#endif
#if ENABLE_FEATURE_FIND_PAREN
		else if (parm == PARM_char_brace) {
			action_paren *ap;
			char **endarg;
			unsigned nested = 1;

			endarg = argv;
			while (1) {
				if (!*++endarg)
					bb_error_msg_and_die("unpaired '('");
				if (LONE_CHAR(*endarg, '('))
					nested++;
				else if (LONE_CHAR(*endarg, ')') && !--nested) {
					*endarg = NULL;
					break;
				}
			}
			ap = ALLOC_ACTION(paren);
			ap->subexpr = parse_params(argv + 1);
			*endarg = (char*) ")"; /* restore NULLed parameter */
			argv = endarg;
		}
#endif
		else if (parm == PARM_name || parm == PARM_iname) {
			action_name *ap;
			ap = ALLOC_ACTION(name);
			ap->pattern = arg1;
			ap->iname = (parm == PARM_iname);
		}
#if ENABLE_FEATURE_FIND_PATH
		else if (parm == PARM_path) {
			action_path *ap;
			ap = ALLOC_ACTION(path);
			ap->pattern = arg1;
		}
#endif
#if ENABLE_FEATURE_FIND_REGEX
		else if (parm == PARM_regex) {
			action_regex *ap;
			ap = ALLOC_ACTION(regex);
			xregcomp(&ap->compiled_pattern, arg1, 0 /*cflags*/);
		}
#endif
#if ENABLE_FEATURE_FIND_TYPE
		else if (parm == PARM_type) {
			action_type *ap;
			ap = ALLOC_ACTION(type);
			ap->type_mask = find_type(arg1);
		}
#endif
#if ENABLE_FEATURE_FIND_PERM
/* -perm mode   File's permission bits are exactly mode (octal or symbolic).
 *              Symbolic modes use mode 0 as a point of departure.
 * -perm -mode  All of the permission bits mode are set for the file.
 * -perm +mode  Any of the permission bits mode are set for the file.
 */
		else if (parm == PARM_perm) {
			action_perm *ap;
			ap = ALLOC_ACTION(perm);
			ap->perm_char = arg1[0];
			arg1 = plus_minus_num(arg1);
			ap->perm_mask = 0;
			if (!bb_parse_mode(arg1, &ap->perm_mask))
				bb_error_msg_and_die("invalid mode: %s", arg1);
		}
#endif
#if ENABLE_FEATURE_FIND_MTIME
		else if (parm == PARM_mtime) {
			action_mtime *ap;
			ap = ALLOC_ACTION(mtime);
			ap->mtime_char = arg1[0];
			ap->mtime_days = xatoul(plus_minus_num(arg1));
		}
#endif
#if ENABLE_FEATURE_FIND_MMIN
		else if (parm == PARM_mmin) {
			action_mmin *ap;
			ap = ALLOC_ACTION(mmin);
			ap->mmin_char = arg1[0];
			ap->mmin_mins = xatoul(plus_minus_num(arg1));
		}
#endif
#if ENABLE_FEATURE_FIND_NEWER
		else if (parm == PARM_newer) {
			struct stat stat_newer;
			action_newer *ap;
			ap = ALLOC_ACTION(newer);
			xstat(arg1, &stat_newer);
			ap->newer_mtime = stat_newer.st_mtime;
		}
#endif
#if ENABLE_FEATURE_FIND_INUM
		else if (parm == PARM_inum) {
			action_inum *ap;
			ap = ALLOC_ACTION(inum);
			ap->inode_num = xatoul(arg1);
		}
#endif
#if ENABLE_FEATURE_FIND_USER
		else if (parm == PARM_user) {
			action_user *ap;
			ap = ALLOC_ACTION(user);
			ap->uid = bb_strtou(arg1, NULL, 10);
			if (errno)
				ap->uid = xuname2uid(arg1);
		}
#endif
#if ENABLE_FEATURE_FIND_GROUP
		else if (parm == PARM_group) {
			action_group *ap;
			ap = ALLOC_ACTION(group);
			ap->gid = bb_strtou(arg1, NULL, 10);
			if (errno)
				ap->gid = xgroup2gid(arg1);
		}
#endif
#if ENABLE_FEATURE_FIND_SIZE
		else if (parm == PARM_size) {
/* -size n[bckw]: file uses n units of space
 * b (default): units are 512-byte blocks
 * c: 1 byte
 * k: kilobytes
 * w: 2-byte words
 */
#if ENABLE_LFS
#define XATOU_SFX xatoull_sfx
#else
#define XATOU_SFX xatoul_sfx
#endif
			static const struct suffix_mult find_suffixes[] = {
				{ "c", 1 },
				{ "w", 2 },
				{ "", 512 },
				{ "b", 512 },
				{ "k", 1024 },
				{ "", 0 }
			};
			action_size *ap;
			ap = ALLOC_ACTION(size);
			ap->size_char = arg1[0];
			ap->size = XATOU_SFX(plus_minus_num(arg1), find_suffixes);
		}
#endif
#if ENABLE_FEATURE_FIND_CONTEXT
		else if (parm == PARM_context) {
			action_context *ap;
			ap = ALLOC_ACTION(context);
			ap->context = NULL;
			/* SELinux headers erroneously declare non-const parameter */
			if (selinux_raw_to_trans_context((char*)arg1, &ap->context))
				bb_simple_perror_msg(arg1);
		}
#endif
#if ENABLE_FEATURE_FIND_LINKS
		else if (parm == PARM_links) {
			action_links *ap;
			ap = ALLOC_ACTION(links);
			ap->links_char = arg1[0];
			ap->links_count = xatoul(plus_minus_num(arg1));
		}
#endif
		else {
			bb_error_msg("unrecognized: %s", arg);
			bb_show_usage();
		}
		argv++;
	}
	return appp;
#undef ALLOC_ACTION
}


int find_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int find_main(int argc UNUSED_PARAM, char **argv)
{
	static const char options[] ALIGN1 =
	                  "-follow\0"
IF_FEATURE_FIND_XDEV(    "-xdev\0"    )
IF_FEATURE_FIND_MAXDEPTH("-mindepth\0""-maxdepth\0")
	                  ;
	enum {
	                  OPT_FOLLOW,
IF_FEATURE_FIND_XDEV(    OPT_XDEV    ,)
IF_FEATURE_FIND_MAXDEPTH(OPT_MINDEPTH,)
	};

	char *arg;
	char **argp;
	int i, firstopt, status = EXIT_SUCCESS;
#if ENABLE_FEATURE_FIND_MAXDEPTH
	int minmaxdepth[2] = { 0, INT_MAX };
#else
#define minmaxdepth NULL
#endif

	INIT_G();

	for (firstopt = 1; argv[firstopt]; firstopt++) {
		if (argv[firstopt][0] == '-')
			break;
		if (ENABLE_FEATURE_FIND_NOT && LONE_CHAR(argv[firstopt], '!'))
			break;
#if ENABLE_FEATURE_FIND_PAREN
		if (LONE_CHAR(argv[firstopt], '('))
			break;
#endif
	}
	if (firstopt == 1) {
		argv[0] = (char*)".";
		argv--;
		firstopt++;
	}

/* All options always return true. They always take effect
 * rather than being processed only when their place in the
 * expression is reached.
 * We implement: -follow, -xdev, -maxdepth
 */
	/* Process options, and replace then with -a */
	/* (-a will be ignored by recursive parser later) */
	argp = &argv[firstopt];
	while ((arg = argp[0])) {
		int opt = index_in_strings(options, arg);
		if (opt == OPT_FOLLOW) {
			G.recurse_flags |= ACTION_FOLLOWLINKS | ACTION_DANGLING_OK;
			argp[0] = (char*)"-a";
		}
#if ENABLE_FEATURE_FIND_XDEV
		if (opt == OPT_XDEV) {
			struct stat stbuf;
			if (!G.xdev_count) {
				G.xdev_count = firstopt - 1;
				G.xdev_dev = xzalloc(G.xdev_count * sizeof(G.xdev_dev[0]));
				for (i = 1; i < firstopt; i++) {
					/* not xstat(): shouldn't bomb out on
					 * "find not_exist exist -xdev" */
					if (stat(argv[i], &stbuf) == 0)
						G.xdev_dev[i-1] = stbuf.st_dev;
					/* else G.xdev_dev[i-1] stays 0 and
					 * won't match any real device dev_t */
				}
			}
			argp[0] = (char*)"-a";
		}
#endif
#if ENABLE_FEATURE_FIND_MAXDEPTH
		if (opt == OPT_MINDEPTH || opt == OPT_MINDEPTH + 1) {
			if (!argp[1])
				bb_show_usage();
			minmaxdepth[opt - OPT_MINDEPTH] = xatoi_u(argp[1]);
			argp[0] = (char*)"-a";
			argp[1] = (char*)"-a";
			argp++;
		}
#endif
		argp++;
	}

	G.actions = parse_params(&argv[firstopt]);

	for (i = 1; i < firstopt; i++) {
		if (!recursive_action(argv[i],
				G.recurse_flags,/* flags */
				fileAction,     /* file action */
				fileAction,     /* dir action */
#if ENABLE_FEATURE_FIND_MAXDEPTH
				minmaxdepth,    /* user data */
#else
				NULL,           /* user data */
#endif
				0))             /* depth */
			status = EXIT_FAILURE;
	}
	return status;
}
