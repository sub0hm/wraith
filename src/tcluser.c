/*
 * tcluser.c -- handles:
 *   Tcl stubs for the user-record-oriented commands
 *
 */

#include "main.h"
#include "users.h"
#include "chan.h"
#include "tandem.h"
#include "modules.h"


extern Tcl_Interp	*interp;
extern struct userrec	*userlist;
extern int		 default_flags, dcc_total, ignore_time;
extern struct dcc_t	*dcc;
extern char		 origbotname[], botnetnick[];
extern time_t		 now;


static int tcl_chattr STDVAR
{
  char *chan, *chg, work[100];
  struct flag_record pls, mns, user;
  struct userrec *u;

  BADARGS(2, 4, " handle ?changes? ?channel?");
  if ((argv[1][0] == '*') || !(u = get_user_by_handle(userlist, argv[1]))) {
    Tcl_AppendResult(irp, "*", NULL);
    return TCL_OK;
  }
  if (argc == 4) {
    user.match = FR_GLOBAL | FR_CHAN;
    chan = argv[3];
    chg = argv[2];
  } else if (argc == 3 && argv[2][0]) {
    int ischan = (findchan_by_dname(argv[2]) != NULL);
    if (strchr(CHANMETA, argv[2][0]) && !ischan && argv[2][0] != '+' && argv[2][0] != '-') {
      Tcl_AppendResult(irp, "no such channel", NULL);
      return TCL_ERROR;
    } else if (ischan) {
      /* Channel exists */
      user.match = FR_GLOBAL | FR_CHAN;
      chan = argv[2];
      chg = NULL;
    } else {
      /* 3rd possibility... channel doesnt exist, does start with a +.
       * In this case we assume the string is flags.
       */
      user.match = FR_GLOBAL;
      chan = NULL;
      chg = argv[2];
    }
  } else {
    user.match = FR_GLOBAL;
    chan = NULL;
    chg = NULL;
  }
  if (chan && !findchan_by_dname(chan)) {
    Tcl_AppendResult(irp, "no such channel", NULL);
    return TCL_ERROR;
  }
  get_user_flagrec(u, &user, chan);
  /* Make changes */
  if (chg) {
    pls.match = user.match;
    break_down_flags(chg, &pls, &mns);
    /* No-one can change these flags on-the-fly */
    pls.global &=~(USER_BOT);
    mns.global &=~(USER_BOT);
    if (chan) {
      pls.chan &= ~(BOT_SHARE);
      mns.chan &= ~(BOT_SHARE);
    }
    user.global = sanity_check((user.global |pls.global) &~mns.global);
    user.udef_global = (user.udef_global | pls.udef_global)
      & ~mns.udef_global;
    if (chan) {
      user.chan = chan_sanity_check((user.chan | pls.chan) & ~mns.chan,
				    user.global);
      user.udef_chan = (user.udef_chan | pls.udef_chan) & ~mns.udef_chan;
    }
    set_user_flagrec(u, &user, chan);
  }
  /* Retrieve current flags and return them */
  build_flags(work, &user, NULL);
  Tcl_AppendResult(irp, work, NULL);
  return TCL_OK;
}

static int tcl_adduser STDVAR
{
  BADARGS(2, 3, " handle ?hostmask?");
  if (strlen(argv[1]) > HANDLEN)
    argv[1][HANDLEN] = 0;
  if ((argv[1][0] == '*') || get_user_by_handle(userlist, argv[1]))
    Tcl_AppendResult(irp, "0", NULL);
  else {
    userlist = adduser(userlist, argv[1], argv[2], "-", default_flags);
    Tcl_AppendResult(irp, "1", NULL);
  }
  return TCL_OK;
}

static int tcl_deluser STDVAR
{
  BADARGS(2, 2, " handle");
  Tcl_AppendResult(irp, (argv[1][0] == '*') ? "0" :
		   int_to_base10(deluser(argv[1])), NULL);
  return TCL_OK;
}

static int tcl_delhost STDVAR
{
  BADARGS(3, 3, " handle hostmask");
  if ((!get_user_by_handle(userlist, argv[1])) || (argv[1][0] == '*')) {
    Tcl_AppendResult(irp, "non-existent user", NULL);
    return TCL_ERROR;
  }
  Tcl_AppendResult(irp, delhost_by_handle(argv[1], argv[2]) ? "1" : "0",
		   NULL);
  return TCL_OK;
}

static int tcl_userlist STDVAR
{
  struct userrec *u;
  struct flag_record user, plus, minus;
  int ok = 1, f = 0;

  BADARGS(1, 3, " ?flags ?channel??");
  if (argc == 3 && !findchan_by_dname(argv[2])) {
    Tcl_AppendResult(irp, "Invalid channel: ", argv[2], NULL);
    return TCL_ERROR;
  }
  if (argc >= 2) {
    plus.match = FR_GLOBAL | FR_CHAN | FR_BOT;
    break_down_flags(argv[1], &plus, &minus);
    f = (minus.global || minus.udef_global || minus.chan ||
	 minus.udef_chan || minus.bot);
  }
  minus.match = plus.match ^ (FR_AND | FR_OR);
  for (u = userlist; u; u = u->next) {
    if (argc >= 2) {
      user.match = FR_GLOBAL | FR_CHAN | FR_BOT | (argc == 3 ? 0 : FR_ANYWH);
      if (argc == 3) 
	      get_user_flagrec(u, &user, argv[2]);
      else
	      get_user_flagrec(u, &user, NULL);

      if (flagrec_eq(&plus, &user) && !(f && flagrec_eq(&minus, &user)))
	ok = 1;
      else
	ok = 0;
    }
    if (ok)
      Tcl_AppendElement(interp, u->handle);
  }
  return TCL_OK;
}

#ifdef HUB
static int tcl_save STDVAR
{
  write_userfile(-1);
  return TCL_OK;
}

static int tcl_reload STDVAR
{
  reload();
  return TCL_OK;
}
#endif /* HUB */

static int tcl_chhandle STDVAR
{
  struct userrec *u;
  char newhand[HANDLEN + 1];
  int x = 1, i;

  BADARGS(3, 3, " oldnick newnick");
  u = get_user_by_handle(userlist, argv[1]);
  if (!u)
     x = 0;
  else {
    strncpyz(newhand, argv[2], sizeof newhand);
    for (i = 0; i < strlen(newhand); i++)
      if ((newhand[i] <= 32) || (newhand[i] >= 127) || (newhand[i] == '@'))
	newhand[i] = '?';
    if (strchr(BADHANDCHARS, newhand[0]) != NULL)
      x = 0;
    else if (strlen(newhand) < 1)
      x = 0;
    else if (get_user_by_handle(userlist, newhand))
      x = 0;
    else if (!egg_strcasecmp(botnetnick, newhand) &&
             (!(u->flags & USER_BOT) || nextbot (argv [1]) != -1))
      x = 0;
    else if (newhand[0] == '*')
      x = 0;
  }
  if (x)
     x = change_handle(u, newhand);
  Tcl_AppendResult(irp, x ? "1" : "0", NULL);
  return TCL_OK;
}

static int tcl_getting_users STDVAR
{
  int i;

  BADARGS(1, 1, "");
  for (i = 0; i < dcc_total; i++) {
    if (dcc[i].type == &DCC_BOT && dcc[i].status & STAT_GETTING) {
      Tcl_AppendResult(irp, "1", NULL);
      return TCL_OK;
    }
  }
  Tcl_AppendResult(irp, "0", NULL);
  return TCL_OK;
}

static int tcl_isignore STDVAR
{
  BADARGS(2, 2, " nick!user@host");
  Tcl_AppendResult(irp, match_ignore(argv[1]) ? "1" : "0", NULL);
  return TCL_OK;
}

static int tcl_newignore STDVAR
{
  time_t expire_time;
  char ign[UHOSTLEN], cmt[66], from[HANDLEN + 1];

  BADARGS(4, 5, " hostmask creator comment ?lifetime?");
  strncpyz(ign, argv[1], sizeof ign);
  strncpyz(from, argv[2], sizeof from);
  strncpyz(cmt, argv[3], sizeof cmt);
  if (argc == 4)
     expire_time = now + (60 * ignore_time);
  else {
    if (argc == 5 && atol(argv[4]) == 0)
      expire_time = 0L;
    else
      expire_time = now + (60 * atol(argv[4])); /* This is a potential crash. FIXME  -poptix */
  }
  addignore(ign, from, cmt, expire_time);
  return TCL_OK;
}

static int tcl_killignore STDVAR
{
  BADARGS(2, 2, " hostmask");
  Tcl_AppendResult(irp, delignore(argv[1]) ? "1" : "0", NULL);
  return TCL_OK;
}

/* { hostmask note expire-time create-time creator }
 */
static int tcl_ignorelist STDVAR
{
  struct igrec *i;
  char expire[11], added[11], *p;
#if (((TCL_MAJOR_VERSION == 8) && (TCL_MINOR_VERSION >= 4)) || (TCL_MAJOR_VERSION > 8))
    CONST char *list[5];
#else
    char *list[5];
#endif

  BADARGS(1, 1, "");
  for (i = global_ign; i; i = i->next) {
    list[0] = i->igmask;
    list[1] = i->msg;
    egg_snprintf(expire, sizeof expire, "%lu", i->expire);
    list[2] = expire;
    egg_snprintf(added, sizeof added, "%lu", i->added);
    list[3] = added;
    list[4] = i->user;
    p = Tcl_Merge(5, list);
    Tcl_AppendElement(irp, p);
    Tcl_Free((char *) p);
  }
  return TCL_OK;
}

static int tcl_getuser STDVAR
{
  struct user_entry_type *et;
  struct userrec *u;
  struct user_entry *e;

  BADARGS(3, 999, " handle type");
  if (!(et = find_entry_type(argv[2])) && egg_strcasecmp(argv[2], "HANDLE")) {
    Tcl_AppendResult(irp, "No such info type: ", argv[2], NULL);
    return TCL_ERROR;
  }
  if (!(u = get_user_by_handle(userlist, argv[1]))) {
    if (argv[1][0] != '*') {
      Tcl_AppendResult(irp, "No such user.", NULL);
      return TCL_ERROR;
    } else
      return TCL_OK;		/* silently ignore user * */
  }
  if (!egg_strcasecmp(argv[2], "HANDLE")) {
    Tcl_AppendResult(irp,u->handle, NULL);
  } else {
  e = find_user_entry(et, u);
  if (e)
    return et->tcl_get(irp, u, e, argc, argv);
  }
  return TCL_OK;
}

static int tcl_setuser STDVAR
{
  struct user_entry_type *et;
  struct userrec *u;
  struct user_entry *e;
  int r;
  module_entry *me;

  BADARGS(3, 999, " handle type ?setting....?");
  if (!(et = find_entry_type(argv[2]))) {
    Tcl_AppendResult(irp, "No such info type: ", argv[2], NULL);
    return TCL_ERROR;
  }
  if (!(u = get_user_by_handle(userlist, argv[1]))) {
    if (argv[1][0] != '*') {
      Tcl_AppendResult(irp, "No such user.", NULL);
      return TCL_ERROR;
    } else
      return TCL_OK;		/* Silently ignore user * */
  }
  me = module_find("irc", 0, 0);
  if (me && !strcmp(argv[2], "hosts") && argc == 3) {
    Function *func = me->funcs;

    (func[IRC_CHECK_THIS_USER]) (argv[1], 1, NULL);
  }
  if (!(e = find_user_entry(et, u))) {
    e = user_malloc(sizeof(struct user_entry));
    e->type = et;
    e->name = NULL;
    e->u.list = NULL;
    list_insert((&(u->entries)), e);
  }
  r = et->tcl_set(irp, u, e, argc, argv);
  /* Yeah... e is freed, and we read it... (tcl: setuser hand HOSTS none) */
  if (!e->u.list) {
    if (list_delete((struct list_type **) &(u->entries),
		    (struct list_type *) e))
      nfree(e);
    /* else maybe already freed... (entry_type==HOSTS) <drummer> */
  }
  if (me && !strcmp(argv[2], "hosts") && argc == 4) {
    Function *func = me->funcs;

    (func[IRC_CHECK_THIS_USER]) (argv[1], 0, NULL);
  }
  return r;
}

tcl_cmds tcluser_cmds[] =
{
  {"chattr",		tcl_chattr},
  {"adduser",		tcl_adduser},
  {"deluser",		tcl_deluser},
  {"delhost",		tcl_delhost},
  {"userlist",		tcl_userlist},
#ifdef HUB
  {"save",		tcl_save},
  {"reload",		tcl_reload},
#endif /* HUB */
  {"chhandle",		tcl_chhandle},
  {"chnick",		tcl_chhandle},
  {"getting-users",	tcl_getting_users},
  {"isignore",		tcl_isignore},
  {"newignore",		tcl_newignore},
  {"killignore",	tcl_killignore},
  {"ignorelist",	tcl_ignorelist},
  {"getuser",		tcl_getuser},
  {"setuser",		tcl_setuser},
  {NULL,		NULL}
};
