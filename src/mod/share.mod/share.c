/*
 * share.c -- part of share.mod
 *
 */


#include "src/common.h"
#include "src/main.h"
#include "src/rfc1459.h"
#include "src/botmsg.h"
#include "src/misc.h"
#include "src/misc_file.h"
#include "src/cmds.h"
#include "src/chanprog.h"
#include "src/users.h"
#include "src/userrec.h"
#include "src/botnet.h"
#include "src/auth.h"

#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <stdarg.h>

#include "src/chan.h"
#include "src/net.h"
#include "src/users.h"
#include "src/egg_timer.h"
#include "src/mod/transfer.mod/transfer.h"
#include "src/mod/channels.mod/channels.h"
#include "src/mod/irc.mod/irc.h"

static struct flag_record fr = { 0, 0, 0, 0 };

struct delay_mode {
  struct delay_mode *next;
  struct chanset_t *chan;
  int plsmns;
  int mode;
  int seconds;
  char *mask;
};

static struct delay_mode *start_delay = NULL;

/* Prototypes */
static void start_sending_users(int);
static void shareout_but(int, const char *, ...)  __attribute__ ((format(printf, 2, 3)));
static void cancel_user_xfer(int, void *);

#include "share.h"

/*
 *    Resync buffers
 */

/* Store info for sharebots */
struct share_msgq {
  struct share_msgq *next;
  char *msg;
};

typedef struct tandbuf_t {
  struct tandbuf_t *next;
  struct share_msgq *q;
  time_t timer;
  char bot[HANDLEN + 1];
} tandbuf;

tandbuf *tbuf = NULL;


/* Create a tandem buffer for 'bot'.
 */
static void new_tbuf(char *bot)
{
  tandbuf **old = &tbuf, *newbuf = NULL;

  newbuf = (tandbuf *) my_calloc(1, sizeof(tandbuf));
  strcpy(newbuf->bot, bot);
  newbuf->q = NULL;
  newbuf->timer = now;
  newbuf->next = *old;
  *old = newbuf;
  putlog(LOG_BOTS, "*", "Creating resync buffer for %s", bot);
}

static void del_tbuf(tandbuf *goner)
{
  struct share_msgq *q = NULL, *r = NULL;
  tandbuf *t = NULL, *old = NULL;

  for (t = tbuf; t; old = t, t = t->next) {
    if (t == goner) {
      if (old)
        old->next = t->next;
      else
        tbuf = t->next;
      for (q = t->q; q && q->msg[0]; q = r) {
        r = q->next;
        free(q->msg);
        free(q);
      }
      free(t);
      break;
    }
  }
}

/* Flush a certain bot's tbuf.
 */
static bool flush_tbuf(char *bot)
{
  tandbuf *t = NULL, *tnext = NULL;

  for (t = tbuf; t; t = tnext) {
    tnext = t->next;
    if (!egg_strcasecmp(t->bot, bot)) {
      del_tbuf(t);
      return 1;
    }
  }
  return 0;
}

static struct share_msgq *q_addmsg(struct share_msgq *qq, char *s)
{
  struct share_msgq *q = NULL;
  int cnt;

  if (!qq) {
    q = (share_msgq *) my_calloc(1, sizeof *q);

    q->next = NULL;
    q->msg = (char *) my_calloc(1, strlen(s) + 1);
    strcpy(q->msg, s);
    return q;
  }
  cnt = 0;
  for (q = qq; q->next; q = q->next)
    cnt++;
  if (cnt > 1000)
    return NULL;                /* Return null: did not alter queue */
  q->next = (share_msgq *) my_calloc(1, sizeof *q->next);

  q = q->next;
  q->next = NULL;
  q->msg = (char *) my_calloc(1, strlen(s) + 1);
  strcpy(q->msg, s);
  return qq;
}

/* Add stuff to a specific bot's tbuf.
 */
static void q_tbuf(char *bot, char *s)
{
  struct share_msgq *q = NULL;
  tandbuf *t = NULL;

  for (t = tbuf; t && t->bot[0]; t = t->next)
    if (!egg_strcasecmp(t->bot, bot)) {
      if ((q = q_addmsg(t->q, s)))
        t->q = q;
      break;
    }
}

/* Add stuff to the resync buffers.
 */
static void q_resync(char *s)
{
  struct share_msgq *q = NULL;
  tandbuf *t = NULL;

  for (t = tbuf; t && t->bot[0]; t = t->next) {
    if ((q = q_addmsg(t->q, s)))
      t->q = q;
  }
}

/* Dump the resync buffer for a bot.
 */
void dump_resync(int idx)
{
  struct share_msgq *q = NULL;
  tandbuf *t = NULL;

  for (t = tbuf; t && t->bot[0]; t = t->next)
    if (!egg_strcasecmp(dcc[idx].nick, t->bot)) {
      for (q = t->q; q && q->msg[0]; q = q->next) {
        dprintf(idx, "%s", q->msg);
      }
      flush_tbuf(dcc[idx].nick);
      break;
    }
}

/*
 *   Sup's delay code
 */

static void
add_delay(struct chanset_t *chan, int plsmns, int mode, char *mask)
{
  struct delay_mode *d = (struct delay_mode *) my_calloc(1, sizeof(struct delay_mode));

  d->chan = chan;
  d->plsmns = plsmns;
  d->mode = mode;
  d->mask = (char *) my_calloc(1, strlen(mask) + 1);

  strncpy(d->mask, mask, strlen(mask) + 1);
  d->seconds = (int) (now + randint(20));
  d->next = start_delay;
  start_delay = d;
}

static void
del_delay(struct delay_mode *delay)
{
  struct delay_mode *old = NULL;

  for (struct delay_mode *d = start_delay; d; old = d, d = d->next) {
    if (d == delay) {
      if (old)
        old->next = d->next;
      else
        start_delay = d->next;
      if (d->mask)
        free(d->mask);
      free(d);
      break;
    }
  }
}

static void
check_delay()
{
  struct delay_mode *dnext = NULL;

  for (struct delay_mode *d = start_delay; d; d = dnext) {
    dnext = d->next;
    if (d->seconds <= now) {
      add_mode(d->chan, d->plsmns, d->mode, d->mask);
      del_delay(d);
    }
  }
}

/*
 *   Botnet commands
 */

static void
share_stick_ban(int idx, char *par)
{
  if (dcc[idx].status & STAT_SHARE) {
    char *host = NULL, *val = NULL;
    bool yn;

    host = newsplit(&par);
    val = newsplit(&par);
    yn = atoi(val);
    noshare = 1;
    if (!par[0]) {              /* Global ban */
      struct chanset_t *chan = NULL;

      if (u_setsticky_ban(NULL, host, yn) > 0) {
        if (conf.bot->hub)
          putlog(LOG_CMDS, "@", "%s: %s %s", dcc[idx].nick, (yn) ? "stick" : "unstick", host);
        shareout_but(idx, "s %s %d\n", host, yn);
      }
      if (!conf.bot->hub)
        for (chan = chanset; chan != NULL; chan = chan->next)
          check_this_ban(chan, host, yn);
    } else {
      struct chanset_t *chan = findchan_by_dname(par);
      struct chanuserrec *cr;

      if ((chan != NULL) && (cr = get_chanrec(dcc[idx].user, par)))
        if (u_setsticky_ban(chan, host, yn) > 0) {
          if (conf.bot->hub)
            putlog(LOG_CMDS, "@", "%s: %s %s %s", dcc[idx].nick, (yn) ? "stick" : "unstick", host, par);
          shareout_but(idx, "s %s %d %s\n", host, yn, chan->dname);
          noshare = 0;
          return;
        }
      if (chan && !conf.bot->hub)
        check_this_ban(chan, host, yn);
      putlog(LOG_CMDS, "@", "Rejecting invalid sticky ban: %s on %s%s", host, par, yn ? "" : " (unstick)");
    }
    noshare = 0;
  }
}

/* Same as share_stick_ban, only for exempts.
 */
static void
share_stick_exempt(int idx, char *par)
{
  if (dcc[idx].status & STAT_SHARE) {
    char *host = NULL, *val = NULL;
    bool yn;

    host = newsplit(&par);
    val = newsplit(&par);
    yn = atoi(val);
    noshare = 1;
    if (!par[0]) {              /* Global exempt */
      if (u_setsticky_exempt(NULL, host, yn) > 0) {
        if (conf.bot->hub)
          putlog(LOG_CMDS, "@", "%s: %s %s", dcc[idx].nick, (yn) ? "stick" : "unstick", host);
        shareout_but(idx, "se %s %d\n", host, yn);
      }
    } else {
      struct chanset_t *chan = findchan_by_dname(par);
      struct chanuserrec *cr = NULL;

      if ((chan != NULL) && (cr = get_chanrec(dcc[idx].user, par)))
        if (u_setsticky_exempt(chan, host, yn) > 0) {
          if (conf.bot->hub)
            putlog(LOG_CMDS, "@", "%s: stick %s %c %s", dcc[idx].nick, host, yn ? 'y' : 'n', par);
          shareout_but(idx, "se %s %d %s\n", host, yn, chan->dname);
          noshare = 0;
          return;
        }
      putlog(LOG_CMDS, "@", "Rejecting invalid sticky exempt: %s on %s, %c", host, par, yn ? 'y' : 'n');
    }
    noshare = 0;
  }
}

/* Same as share_stick_ban, only for invites.
 */
static void
share_stick_invite(int idx, char *par)
{
  if (dcc[idx].status & STAT_SHARE) {
    char *host = NULL, *val = NULL;
    bool yn;

    host = newsplit(&par);
    val = newsplit(&par);
    yn = atoi(val);
    noshare = 1;
    if (!par[0]) {              /* Global invite */
      if (u_setsticky_invite(NULL, host, yn) > 0) {
        if (conf.bot->hub)
          putlog(LOG_CMDS, "@", "%s: %s %s", dcc[idx].nick, (yn) ? "stick" : "unstick", host);
        shareout_but(idx, "sInv %s %d\n", host, yn);
      }
    } else {
      struct chanset_t *chan = findchan_by_dname(par);
      struct chanuserrec *cr = NULL;

      if ((chan != NULL) && (cr = get_chanrec(dcc[idx].user, par)))
        if (u_setsticky_invite(chan, host, yn) > 0) {
          if (conf.bot->hub)
            putlog(LOG_CMDS, "@", "%s: %s %s %s", dcc[idx].nick, (yn) ? "stick" : "unstick", host, par);
          shareout_but(idx, "sInv %s %d %s\n", host, yn, chan->dname);
          noshare = 0;
          return;
        }
      putlog(LOG_CMDS, "@", "Rejecting invalid sticky invite: %s on %s%s", host, par, yn ? "" : " (unstick)");
    }
    noshare = 0;
  }
}

static void
share_chhand(int idx, char *par)
{
  if (dcc[idx].status & STAT_SHARE) {
    char *hand = NULL;
    struct userrec *u = NULL;

    hand = newsplit(&par);
    u = get_user_by_handle(userlist, hand);
    if (u) {
      int value = 0;

      shareout_but(idx, "h %s %s\n", hand, par);
      noshare = 1;
      value = change_handle(u, par);
      if (value && conf.bot->hub)
        putlog(LOG_CMDS, "@", "%s: handle %s->%s", dcc[idx].nick, hand, par);
      noshare = 0;
    }
  }
}

static void
share_chattr(int idx, char *par)
{
  if (dcc[idx].status & STAT_SHARE) {
    char *hand = NULL, *atr = NULL, s[100] = "";
    struct chanset_t *cst = NULL;
    struct userrec *u = NULL;
    struct flag_record fr2;
    flag_t ofl;

    hand = newsplit(&par);
    u = get_user_by_handle(userlist, hand);
    if (u) {
      atr = newsplit(&par);
      cst = findchan_by_dname(par);
      if (!par[0] || cst) {
        if (!(dcc[idx].status & STAT_GETTING))
          shareout_but(idx, "a %s %s %s\n", hand, atr, par);
        noshare = 1;
        if (par[0] && cst) {
          fr.match = FR_CHAN;
          fr2.match = FR_CHAN;
          break_down_flags(atr, &fr, 0);
          get_user_flagrec(u, &fr2, par);

          set_user_flagrec(u, &fr, par);
          check_dcc_chanattrs(u, par, fr.chan, fr2.chan);
          noshare = 0;
          build_flags(s, &fr, 0);
          if (conf.bot->hub) {
            if (!(dcc[idx].status & STAT_GETTING))
              putlog(LOG_CMDS, "@", "%s: chattr %s %s %s", dcc[idx].nick, hand, s, par);
          } else
            recheck_channel(cst, 0);
        } else {
          fr.match = FR_GLOBAL;
          get_user_flagrec(dcc[idx].user, &fr, 0);
          /* Don't let bot flags be altered */
          ofl = fr.global;
          break_down_flags(atr, &fr, 0);
          fr.global = sanity_check(fr.global, u->bot);
          set_user_flagrec(u, &fr, 0);
          check_dcc_attrs(u, ofl);
          noshare = 0;
          build_flags(s, &fr, 0);
          fr.match = FR_CHAN;
          if (conf.bot->hub) {
            if (!(dcc[idx].status & STAT_GETTING))
              putlog(LOG_CMDS, "@", "%s: chattr %s %s", dcc[idx].nick, hand, s);
          } else {
            for (cst = chanset; cst; cst = cst->next)
              recheck_channel(cst, 0);
          }
        }
        noshare = 0;
      }
      if (conf.bot->hub)
        write_userfile(-1);
    }
  }
}

static void
share_pls_chrec(int idx, char *par)
{
  if (dcc[idx].status & STAT_SHARE) {
    char *user = NULL;
    struct chanset_t *chan = NULL;
    struct userrec *u = NULL;

    user = newsplit(&par);
    if ((u = get_user_by_handle(userlist, user))) {
      chan = findchan_by_dname(par);
      noshare = 1;
      shareout_but(idx, "+cr %s %s\n", user, par);
      if (!get_chanrec(u, par)) {
        add_chanrec(u, par);
        if (conf.bot->hub)
          putlog(LOG_CMDS, "@", "%s: +chrec %s %s", dcc[idx].nick, user, par);
      }
      noshare = 0;
    }
  }
}

static void
share_mns_chrec(int idx, char *par)
{
  if (dcc[idx].status & STAT_SHARE) {
    char *user = NULL;
    struct chanset_t *chan = NULL;
    struct userrec *u = NULL;

    user = newsplit(&par);
    if ((u = get_user_by_handle(userlist, user))) {
      chan = findchan_by_dname(par);
      noshare = 1;
      del_chanrec(u, par);
      shareout_but(idx, "-cr %s %s\n", user, par);
      noshare = 0;
      if (conf.bot->hub)
        putlog(LOG_CMDS, "@", "%s: -chrec %s %s", dcc[idx].nick, user, par);
      else
        recheck_channel(chan, 0);

    }
  }
}

static void
share_newuser(int idx, char *par)
{
  if (dcc[idx].status & STAT_SHARE) {
    char *nick = NULL, *host = NULL, *pass = NULL;
    bool isbot = 0;
    struct userrec *u = NULL;

    nick = newsplit(&par);
    host = newsplit(&par);
    pass = newsplit(&par);
    
    if (nick[0] == '-') {
      isbot = 1;
      nick++;
    }    
    if (!(u = get_user_by_handle(userlist, nick))) {
      char s[100] = "";

      fr.global = 0;

      fr.match = FR_GLOBAL;
      break_down_flags(par, &fr, NULL);

      /* If user already exists, ignore command */
      shareout_but(idx, "n %s%s %s %s %s\n", isbot ? "-" : "", nick, host, pass, par);

      noshare = 1;
      if (strlen(nick) > HANDLEN)
        nick[HANDLEN] = 0;

      fr.match = FR_GLOBAL;
      build_flags(s, &fr, 0);

      userlist = adduser(userlist, nick, host, pass, 0, isbot);

      /* Support for userdefinedflag share - drummer */
      u = get_user_by_handle(userlist, nick);
      set_user_flagrec(u, &fr, 0);
      fr.match = FR_CHAN;       /* why?? */
      noshare = 0;
      if (conf.bot->hub)
        putlog(LOG_CMDS, "@", "%s: newuser %s %s", dcc[idx].nick, nick, s);
    }
  }
}

static void
share_killuser(int idx, char *par)
{
  struct userrec *u = NULL;

  /* If user is a share bot, ignore command */
  if ((dcc[idx].status & STAT_SHARE) && (u = get_user_by_handle(userlist, par))) {

    noshare = 1;
    if (deluser(par)) {
      shareout_but(idx, "k %s\n", par);
      if (conf.bot->hub)
        putlog(LOG_CMDS, "@", "%s: killuser %s", dcc[idx].nick, par);
    }
    noshare = 0;
  }
}

static void
share_pls_host(int idx, char *par)
{
  if (dcc[idx].status & STAT_SHARE) {
    char *hand = NULL;
    struct userrec *u = NULL;

    hand = newsplit(&par);
    if ((u = get_user_by_handle(userlist, hand))) {
      shareout_but(idx, "+h %s %s\n", hand, par);
      set_user(&USERENTRY_HOSTS, u, par);
      if (conf.bot->hub)
        putlog(LOG_CMDS, "@", "%s: +host %s %s", dcc[idx].nick, hand, par);
      else
        check_this_user(u->handle, 0, NULL);
    }
  }
}

static void
share_pls_bothost(int idx, char *par)
{
  if (dcc[idx].status & STAT_SHARE) {
    char *hand = NULL;
    struct userrec *u = NULL;

    hand = newsplit(&par);
    u = get_user_by_handle(userlist, hand);
    if (!(dcc[idx].status & STAT_GETTING))
      shareout_but(idx, "+bh %s %s\n", hand, par);
    /* Add bot to userlist if not there */
    if (u) {
      if (!u->bot)
        return;                 /* ignore */
      set_user(&USERENTRY_HOSTS, u, par);
    } else {
      userlist = adduser(userlist, hand, par, "-", 0, 1);
    }
    if (conf.bot->hub && !(dcc[idx].status & STAT_GETTING))
      putlog(LOG_CMDS, "@", "%s: +host %s %s", dcc[idx].nick, hand, par);
  }
}

static void
share_mns_host(int idx, char *par)
{
  if (dcc[idx].status & STAT_SHARE) {
    char *hand = NULL;
    struct userrec *u = NULL;

    hand = newsplit(&par);
    if ((u = get_user_by_handle(userlist, hand))) {
      shareout_but(idx, "-h %s %s\n", hand, par);
      noshare = 1;
      delhost_by_handle(hand, par);
      noshare = 0;
      if (conf.bot->hub)
        putlog(LOG_CMDS, "@", "%s: -host %s %s", dcc[idx].nick, hand, par);
      else
        check_this_user(hand, 2, par);
    }
  }
}

static void
share_change(int idx, char *par)
{
  if (dcc[idx].status & STAT_SHARE) {
    char *key = NULL, *hand = NULL;
    struct userrec *u = NULL;
    struct user_entry_type *uet = NULL;
    struct user_entry *e = NULL;

    key = newsplit(&par);
    hand = newsplit(&par);

    u = get_user_by_handle(userlist, hand);

    if (!(uet = find_entry_type(key)))
      /* If it's not a supported type, forget it */
      putlog(LOG_ERROR, "*", "Ignore ch %s from %s (unknown type)", key, dcc[idx].nick);
    else {
      if (!(dcc[idx].status & STAT_GETTING))
        shareout_but(idx, "c %s %s %s\n", key, hand, par);
      noshare = 1;
      if (!u && (uet == &USERENTRY_BOTADDR)) {
        char pass[30] = "";

        makepass(pass);
        userlist = adduser(userlist, hand, "none", pass, 0, 1);
        u = get_user_by_handle(userlist, hand);
      } else if (!u)
        return;

      if (uet->got_share) {
        if (!(e = find_user_entry(uet, u))) {
          e = (struct user_entry *) my_calloc(1, sizeof(struct user_entry));

          e->type = uet;
          e->name = NULL;
          e->u.list = NULL;
          list_insert((&(u->entries)), e);
        }
        uet->got_share(u, e, par, idx);
        if (!e->u.list) {
          list_delete((struct list_type **) &(u->entries), (struct list_type *) e);
          free(e);
        }
      }
      noshare = 0;
    }
  }
}

static void
share_chchinfo(int idx, char *par)
{
  if (dcc[idx].status & STAT_SHARE) {
    char *hand = NULL, *chan = NULL;
    struct chanset_t *cst = NULL;
    struct userrec *u = NULL;

    hand = newsplit(&par);
    if ((u = get_user_by_handle(userlist, hand))) {
      chan = newsplit(&par);
      cst = findchan_by_dname(chan);
      shareout_but(idx, "chchinfo %s %s %s\n", hand, chan, par);
      noshare = 1;
      set_handle_chaninfo(userlist, hand, chan, par);
      noshare = 0;
      if (conf.bot->hub)
        putlog(LOG_CMDS, "@", "%s: change info %s %s", dcc[idx].nick, chan, hand);
    }
  }
}

static void share_mns_mask(int idx, char *par)
{
  if (dcc[idx].status & STAT_SHARE) {
    char *types = newsplit(&par);
    const char type = types[0];

    shareout_but(idx, "-m %c %s\n", type, par);
    if (conf.bot->hub)
      putlog(LOG_CMDS, "@", "%s: cancel %s %s", dcc[idx].nick, type == 'b' ? "ban" : type == 'e' ? "exempt" : "invite", par);
    str_unescape(par, '\\');
    noshare = 1;
    if (u_delmask(type, NULL, par, 1) > 0) {
      if (!conf.bot->hub) {
        struct chanset_t *chan = NULL;
        masklist *channel_list = NULL;
        
        for (chan = chanset; chan; chan = chan->next) {
          channel_list = (type == 'b' ? chan->channel.ban : type == 'e' ? 
                          chan->channel.exempt : chan->channel.invite);

          if (channel_list && !ismasked(channel_list, par))
            add_delay(chan, '-', type, par);
        }
      }
    }
    noshare = 0;
  }
}

static void share_mns_maskchan(int idx, char *par)
{
  if (dcc[idx].status & STAT_SHARE) {
    char *types = newsplit(&par);
    const char type = types[0];

    char *chname = NULL;
    struct chanset_t *chan = NULL;
    int value = 0;

    chname = newsplit(&par);
    chan = findchan_by_dname(chname);
    shareout_but(idx, "-mc %c %s %s\n", type, chname, par);
    if (conf.bot->hub)
      putlog(LOG_CMDS, "@", "%s: cancel %s %s on %s", dcc[idx].nick, 
                            type == 'b' ? "ban" : type == 'e' ? "exempt" : "invite", par, chname);
    str_unescape(par, '\\');
    noshare = 1;
    value = u_delmask(type, chan, par, 1);
    if (!conf.bot->hub && value > 0)
      add_delay(chan, '-', type, par);
    noshare = 0;
  }
}

static void share_pls_mask(int idx, char *par)
{
  if (dcc[idx].status & STAT_SHARE) {
    char *types = newsplit(&par);
    const char type = types[0];

    const char *str_type = (type == 'b' ? "ban" : type == 'e' ? "exempt" : "invite");
    time_t expire_time;
    char *mask = NULL, *tm = NULL, *from = NULL;
    int flags = 0;
    bool stick = 0;

    shareout_but(idx, "+m %c %s\n", type, par);
    noshare = 1;
    mask = newsplit(&par);
    str_unescape(mask, '\\');
    tm = newsplit(&par);
    from = newsplit(&par);
    if (strchr(from, 's')) {
      flags |= MASKREC_STICKY;
      stick = 1;
    }
    if (strchr(from, 'p'))
      flags |= MASKREC_PERM;
    from = newsplit(&par);
    expire_time = (time_t) atoi(tm);
    if (expire_time != 0L)
      expire_time += now;
    u_addmask(type, NULL, mask, from, par, expire_time, flags);
    noshare = 0;
    if (conf.bot->hub)
      putlog(LOG_CMDS, "@", "%s: global %s %s (%s:%s)", dcc[idx].nick, str_type, mask, from, par);
    else {
      void (*check_this_mask) (struct chanset_t *, char *, bool) = NULL;

      check_this_mask = (type == 'b' ? check_this_ban : type == 'e' ? check_this_exempt : check_this_invite);

      for (struct chanset_t *chan = chanset; chan != NULL; chan = chan->next)
        check_this_mask(chan, mask, stick);
    }
  }
}

static void share_pls_maskchan(int idx, char *par)
{
  if (dcc[idx].status & STAT_SHARE) {
    char *types = newsplit(&par);
    const char type = types[0];

    const char *str_type = (type == 'b' ? "ban" : type == 'e' ? "exempt" : "invite");
    time_t expire_time;
    int flags = 0;
    char *mask = NULL, *tm = NULL, *chname = NULL, *from = NULL;
    bool stick = 0;
    struct chanset_t *chan = NULL;

    mask = newsplit(&par);
    tm = newsplit(&par);
    chname = newsplit(&par);
    chan = findchan_by_dname(chname);
    shareout_but(idx, "+mc %c %s %s %s %s\n", type, mask, tm, chname, par);
    str_unescape(mask, '\\');
    from = newsplit(&par);
    if (strchr(from, 's')) {
      flags |= MASKREC_STICKY;
      stick = 1;
    }
    if (strchr(from, 'p'))
      flags |= MASKREC_PERM;
    from = newsplit(&par);
    if (conf.bot->hub)
      putlog(LOG_CMDS, "@", "%s: %s %s on %s (%s:%s)", dcc[idx].nick, str_type, mask, chname, from, par);
    noshare = 1;
    expire_time = (time_t) atoi(tm);
    if (expire_time != 0L)
      expire_time += now;
    u_addmask(type, chan, mask, from, par, expire_time, flags);
    noshare = 0;
    if (!conf.bot->hub) {
      void (*check_this_mask) (struct chanset_t *, char *, bool) = NULL;

      check_this_mask = (type == 'b' ? check_this_ban : type == 'e' ? check_this_exempt : check_this_invite);

      check_this_mask(chan, mask, stick);
    }
  }
}

static void
share_mns_ignore(int idx, char *par)
{
  if (dcc[idx].status & STAT_SHARE) {
    shareout_but(idx, "-i %s\n", par);
    if (conf.bot->hub)
      putlog(LOG_CMDS, "@", "%s: cancel ignore %s", dcc[idx].nick, par);
    str_unescape(par, '\\');
    noshare = 1;
    delignore(par);
    noshare = 0;
  }
}

/* +i <host> +<seconds-left> <from> <note>
 */
static void
share_pls_ignore(int idx, char *par)
{
  if (dcc[idx].status & STAT_SHARE) {
    time_t expire_time;
    char *ign = NULL, *from = NULL, *ts = NULL;

    shareout_but(idx, "+i %s\n", par);
    noshare = 1;
    ign = newsplit(&par);
    str_unescape(ign, '\\');
    ts = newsplit(&par);
    if (!atoi(ts))
      expire_time = 0L;
    else
      expire_time = now + atoi(ts);
    from = newsplit(&par);
    if (strchr(from, 'p'))
      expire_time = 0;
    from = newsplit(&par);
    if (strlen(from) > HANDLEN + 1)
      from[HANDLEN + 1] = 0;
    par[65] = 0;
    if (conf.bot->hub)
      putlog(LOG_CMDS, "@", "%s: ignore %s (%s: %s)", dcc[idx].nick, ign, from, par);
    addignore(ign, from, (const char *) par, expire_time);
    noshare = 0;
  }
}

static void
share_ufno(int idx, char *par)
{
  putlog(LOG_BOTS, "@", "User file rejected by %s: %s", dcc[idx].nick, par);
  dcc[idx].status &= ~STAT_OFFERED;
  if (!(dcc[idx].status & STAT_GETTING))
    dcc[idx].status &= ~(STAT_SHARE | STAT_AGGRESSIVE);
}

static void
share_ufyes(int idx, char *par)
{
  if (dcc[idx].status & STAT_OFFERED) {
    dcc[idx].status &= ~STAT_OFFERED;
    dcc[idx].status |= STAT_SHARE;
    dcc[idx].status |= STAT_SENDING;

    dcc[idx].u.bot->uff_flags |= (UFF_OVERRIDE | UFF_INVITE | UFF_EXEMPT);
    dprintf(idx, "s feats overbots invites exempts\n");

    lower_bot_linked(idx);

    start_sending_users(idx);
    putlog(LOG_BOTS, "@", "Sending user file send request to %s", dcc[idx].nick);
  }
}

static void
share_userfileq(int idx, char *par)
{
  flush_tbuf(dcc[idx].nick);

  if (bot_aggressive_to(dcc[idx].user)) {
    putlog(LOG_ERRORS, "*", "%s offered user transfer - I'm supposed to be aggressive to it", dcc[idx].nick);
    dprintf(idx, "s un I have you marked for Agressive sharing.\n");
    botunlink(-2, dcc[idx].nick, "I'm aggressive to you");
  } else {
    bool ok = 1;

    for (int i = 0; i < dcc_total; i++)
      if (dcc[i].type && dcc[i].type->flags & DCT_BOT) {
        if ((dcc[i].status & STAT_SHARE) && (dcc[i].status & STAT_AGGRESSIVE) && (i != idx)) {
          ok = 0;
          break;
        }
      }
    if (!ok)
      dprintf(idx, "s un Already sharing.\n");
    else {
      dcc[idx].u.bot->uff_flags |= (UFF_OVERRIDE | UFF_INVITE | UFF_EXEMPT);
      dprintf(idx, "s uy overbots invites exempts\n");
      /* Set stat-getting to astatic void race condition (robey 23jun1996) */
      dcc[idx].status |= STAT_SHARE | STAT_GETTING | STAT_AGGRESSIVE;
      if (conf.bot->hub)
        putlog(LOG_BOTS, "*", "Downloading user file from %s", dcc[idx].nick);
      else
        putlog(LOG_BOTS, "*", "Downloading user file via uplink.");
    }
  }
}

/* us <ip> <port> <length>
 */
static void
share_ufsend(int idx, char *par)
{
  char *port = NULL, *ip = NULL;
  char s[1024] = "";
  int i, sock;
  FILE *f = NULL;

  egg_snprintf(s, sizeof s, "%s.share.%s.%li.users", tempdir, conf.bot->nick, now);
  if (!(b_status(idx) & STAT_SHARE)) {
    dprintf(idx, "s e You didn't ask; you just started sending.\n");
    dprintf(idx, "s e Ask before sending the userfile.\n");
    zapfbot(idx);
  } else if (dcc_total == max_dcc) {
    putlog(LOG_MISC, "@", "NO MORE DCC CONNECTIONS -- can't grab userfile");
    dprintf(idx, "s e I can't open a DCC to you; I'm full.\n");
    zapfbot(idx);
  } else if (!(f = fopen(s, "wb"))) {
    putlog(LOG_MISC, "@", "CAN'T WRITE USERFILE DOWNLOAD FILE!");
    zapfbot(idx);
  } else {
    ip = newsplit(&par);
    port = newsplit(&par);
#ifdef USE_IPV6
//    sock = getsock(SOCK_BINARY, hostprotocol(ip));      /* Don't buffer this -> mark binary. */
    sock = getsock(SOCK_BINARY, AF_INET);
#else
    sock = getsock(SOCK_BINARY);        /* Don't buffer this -> mark binary. */
#endif /* USE_IPV6 */
    if (sock < 0 || open_telnet_dcc(sock, ip, port) < 0) {
      fclose(f);
      killsock(sock);
      putlog(LOG_BOTS, "@", "Asynchronous connection failed!");
      dprintf(idx, "s e Can't connect to you!\n");
      zapfbot(idx);
    } else {
      putlog(LOG_DEBUG, "@", "Connecting to %s:%s for userfile.", ip, port);
      i = new_dcc(&DCC_FORK_SEND, sizeof(struct xfer_info));
      dcc[i].addr = my_atoul(ip);
      dcc[i].port = atoi(port);
      strcpy(dcc[i].nick, "*users");
      dcc[i].u.xfer->filename = strdup(s);
      dcc[i].u.xfer->origname = dcc[i].u.xfer->filename;
      dcc[i].u.xfer->length = atoi(par);
      dcc[i].u.xfer->f = f;
      dcc[i].sock = sock;
      strcpy(dcc[i].host, dcc[idx].nick);
      dcc[idx].status |= STAT_GETTING;
    }
  }
}

static void
share_version(int idx, char *par)
{
  /* Cleanup any share flags */
  dcc[idx].status &= ~(STAT_SHARE | STAT_GETTING | STAT_SENDING | STAT_OFFERED | STAT_AGGRESSIVE);
  dcc[idx].u.bot->uff_flags |= (UFF_OVERRIDE | UFF_INVITE | UFF_EXEMPT);
  if (bot_aggressive_to(dcc[idx].user)) {
    dprintf(idx, "s u?\n");
    dcc[idx].status |= STAT_OFFERED;
  }
  // else higher_bot_linked(idx);
}

void
hook_read_userfile()
{
  if (!noshare) {
    for (int i = 0; i < dcc_total; i++) {
      if (dcc[i].type && (dcc[i].type->flags & DCT_BOT) && (dcc[i].status & STAT_SHARE) && !(dcc[i].status & STAT_AGGRESSIVE)
          && (1)) {
        /* Cancel any existing transfers */
        if (dcc[i].status & STAT_SENDING)
          cancel_user_xfer(-i, 0);
        dprintf(i, "s u?\n");
        dcc[i].status |= STAT_OFFERED;
      }
    }
  }
}

static void
share_endstartup(int idx, char *par)
{
  dcc[idx].status &= ~STAT_GETTING;
  /* Send to any other sharebots */
  if (conf.bot->hub)
    hook_read_userfile();
}

static void
share_end(int idx, char *par)
{
  putlog(LOG_BOTS, "*", "Ending sharing with %s (%s).", dcc[idx].nick, par);
  cancel_user_xfer(-idx, 0);
  dcc[idx].status &= ~(STAT_SHARE | STAT_GETTING | STAT_SENDING | STAT_OFFERED | STAT_AGGRESSIVE);
  dcc[idx].u.bot->uff_flags = 0;
}

/* Note: these MUST be sorted. */
static botcmd_t C_share[] = {
  {"!", share_endstartup, 0},
  {"+bh", share_pls_bothost, 0},
  {"+cr", share_pls_chrec, 0},
  {"+h", share_pls_host, 0},
  {"+i", share_pls_ignore, 0},
  {"+m", share_pls_mask, 0},
  {"+mc", share_pls_maskchan, 0},
  {"-cr", share_mns_chrec, 0},
  {"-h", share_mns_host, 0},
  {"-i", share_mns_ignore, 0},
  {"-m", share_mns_mask, 0},
  {"-mc", share_mns_maskchan, 0},
  {"a", share_chattr, 0},
  {"c", share_change, 0},
  {"chchinfo", share_chchinfo, 0},
  {"e", share_end, 0},
  {"h", share_chhand, 0},
  {"k", share_killuser, 0},
  {"n", share_newuser, 0},
  {"s", share_stick_ban, 0},
  {"se", share_stick_exempt, 0},
  {"sInv", share_stick_invite, 0},
  {"u?", share_userfileq, 0},
  {"un", share_ufno, HUB},
  {"us", share_ufsend, 0},
  {"uy", share_ufyes, HUB},
  {"v", share_version, 0},
  {NULL, NULL, 0}
};


void
sharein(int idx, char *msg)
{
  char *code = newsplit(&msg);
  int y, f = 0, i = 0;

  for (f = 0, i = 0; C_share[i].name && !f; i++) {
    y = egg_strcasecmp(code, C_share[i].name);

    if (!y)
      /* Found a match */
      (C_share[i].func) (idx, msg);
    if (y < 0)
      f = 1;
  }
}

void
shareout(const char *format, ...)
{
  char s[601] = "";
  int l;
  va_list va;

  va_start(va, format);

  strcpy(s, "s ");
  if ((l = egg_vsnprintf(s + 2, 509, format, va)) < 0)
    s[2 + (l = 509)] = 0;
  va_end(va);

  for (int i = 0; i < dcc_total; i++) {
    if (dcc[i].type && (dcc[i].type->flags & DCT_BOT) && 
         (dcc[i].status & STAT_SHARE) && !(dcc[i].status & (STAT_GETTING | STAT_SENDING))) {
      tputs(dcc[i].sock, s, l + 2);
    }
  }
  q_resync(s);
}

static void
shareout_but(int x, const char *format, ...)
{
  int l;
  char s[601] = "";
  va_list va;

  va_start(va, format);

  strcpy(s, "s ");
  if ((l = egg_vsnprintf(s + 2, 509, format, va)) < 0)
    s[2 + (l = 509)] = 0;
  va_end(va);

  for (int i = 0; i < dcc_total; i++) {
    if (dcc[i].type && (dcc[i].type->flags & DCT_BOT) && (i != x) &&
        (dcc[i].status & STAT_SHARE) && (!(dcc[i].status & STAT_GETTING)) &&
        (!(dcc[i].status & STAT_SENDING))) {
      tputs(dcc[i].sock, s, l + 2);
    }
  }
  q_resync(s);
}

/* Flush all tbufs older than 15 minutes.
 */
static void
check_expired_tbufs()
{
  tandbuf *t = NULL, *tnext = NULL;

  for (t = tbuf; t; t = tnext) {
    tnext = t->next;
    if ((now - t->timer) > 300) {
      putlog(LOG_BOTS, "*", "Flushing resync buffer for clonebot %s.", t->bot);
      del_tbuf(t);
    }
  }
 
  /* Resend userfile requests */
  for (int i = 0; i < dcc_total; i++) {
    if (dcc[i].type && dcc[i].type->flags & DCT_BOT) {
      if (dcc[i].status & STAT_OFFERED) {
        if (now - dcc[i].timeval > 120) {
          if (dcc[i].user && bot_aggressive_to(dcc[i].user))
            dprintf(i, "s u?\n");
          /* ^ send it again in case they missed it */
        }
        /* If it's a share bot that hasnt been sharing, ask again */
      } else if (!(dcc[i].status & STAT_SHARE)) {
        if (dcc[i].user && bot_aggressive_to(dcc[i].user)) {
          dprintf(i, "s u?\n");
          dcc[i].status |= STAT_OFFERED;
        }
      }
    }
  }
}

static bool
write_tmp_userfile(char *fn, const struct userrec *bu, int idx)
{
  FILE *f = NULL;
  int ok = 0;

  if ((f = fopen(fn, "wb"))) {
    time_t tt = now;
    char s1[81] = "";

    fixmod(fn);
    strcpy(s1, ctime(&tt));
    lfprintf(f, "#4v: %s -- %s -- written %s", ver, conf.bot->nick, s1);

    ok += write_chans(f, idx);
    ok += write_config(f, idx);
    ok += write_bans(f, idx);
    ok += write_exempts(f, idx);
    ok += write_invites(f, idx);
    if (ok != 5)
      ok = 0;
    for (struct userrec *u = (struct userrec *) bu; u && ok; u = u->next) {
      if (!write_user(u, f, idx))
        ok = 0;
    }
    fclose(f);
  }
  if (!ok)
    putlog(LOG_MISC, "*", USERF_ERRWRITE2);
  return ok;
}

/* Erase old user list, switch to new one.
 */
void
finish_share(int idx)
{
  struct userrec *u = NULL, *ou = NULL;
  struct chanset_t *chan = NULL;
  int i, j = -1;

  for (i = 0; i < dcc_total; i++)
    if (dcc[i].type && !egg_strcasecmp(dcc[i].nick, dcc[idx].host) && (dcc[i].type->flags & DCT_BOT)) {
      j = i;
      break;
    }
  if (j == -1)
    return;

/* compress.mod 
  if (!uncompressfile(dcc[idx].u.xfer->filename)) {
    char xx[1024] = "";

    putlog(LOG_BOTS, "*", "A uff parsing function failed for the userfile!");
    unlink(dcc[idx].u.xfer->filename);

    dprintf(j, "bye\n");
    simple_snprintf(xx, sizeof xx, "Disconnected %s (uff error)", dcc[j].nick);
    botnet_send_unlinked(j, dcc[j].nick, xx);
    chatout("*** %s\n", xx);

    killsock(dcc[j].sock);
    lostdcc(j);

    return;
  }
*/

  /*
   * This is where we remove all global and channel bans/exempts/invites and
   * ignores since they will be replaced by what our hub gives us.
   */

  noshare = 1;

  while (global_bans)
    u_delmask('b', NULL, global_bans->mask, 1);
  while (global_ign)
    delignore(global_ign->igmask);
  while (global_invites)
    u_delmask('I', NULL, global_invites->mask, 1);
  while (global_exempts)
    u_delmask('e', NULL, global_exempts->mask, 1);

  for (chan = chanset; chan; chan = chan->next) {
    while (chan->bans)
      u_delmask('b', chan, chan->bans->mask, 1);
    while (chan->exempts)
      u_delmask('e', chan, chan->exempts->mask, 1);
    while (chan->invites)
      u_delmask('I', chan, chan->invites->mask, 1);
  }
  noshare = 0;
  ou = userlist;                /* Save old user list                   */
  userlist = (struct userrec *) -1;       /* Do this to prevent .user messups     */

  /* Bot user pointers are updated to point to the new list, all others
   * are set to NULL. If our userfile will be overriden, just set _all_
   * to NULL directly.
   */
  for (i = 0; i < dcc_total; i++)
    if (dcc[i].type)
      dcc[i].user = NULL;
  Auth::NullUsers();

  if (conf.bot->u)
    conf.bot->u = NULL;

  /* Read the transferred userfile. Add entries to u, which already holds
   * the bot entries in non-override mode.
   */
  loading = 1;
  checkchans(0);                /* flag all the channels.. */
  Context;
  if (!readuserfile(dcc[idx].u.xfer->filename, &u)) {   /* read the userfile into 'u' */
    /* FAILURE */
    char xx[1024] = "";

    Context;
    unlink(dcc[idx].u.xfer->filename);
    clear_userlist(u);          /* Clear new, obsolete, user list.      */
    clear_chanlist();           /* Remove all user references from the
                                 * channel lists.                       */
    for (i = 0; i < dcc_total; i++)
      if (dcc[i].type)
        dcc[i].user = get_user_by_handle(ou, dcc[i].nick);

    conf.bot->u = get_user_by_handle(ou, conf.bot->nick);

    userlist = ou;              /* Revert to old user list.             */
    lastuser = NULL;            /* Reset last accessed user ptr.        */

    Auth::FillUsers();

    checkchans(2);              /* un-flag the channels, we are keeping them.. */

    /* old userlist is now being used, safe to do this stuff... */
    loading = 0;
    putlog(LOG_MISC, "*", "%s", USERF_CANTREAD);
    dprintf(idx, "bye\n");
    simple_snprintf(xx, sizeof xx, "Disconnected %s (can't read userfile)", dcc[j].nick);
    botnet_send_unlinked(j, dcc[j].nick, xx);
    chatout("*** %s\n", xx);

    killsock(dcc[j].sock);
    lostdcc(j);
    return;
  }

  /* SUCCESS! */

  unlink(dcc[idx].u.xfer->filename);

  loading = 0;
  clear_chanlist();             /* Remove all user references from the
                                 * channel lists.                       */
  userlist = u;                 /* Set new user list.                   */
  lastuser = NULL;              /* Reset last accessed user ptr.        */
  putlog(LOG_BOTS, "*", "%s.", USERF_XFERDONE);

  /*
   * Migrate:
   *   - unshared (got_share == 0) user entries
   */
  clear_userlist(ou);

  /* copy over any auth users */
  Auth::FillUsers();

  checkchans(1);                /* remove marked channels */
  trigger_cfg_changed();
  reaffirm_owners();            /* Make sure my owners are +a   */
  updatebot(-1, dcc[j].nick, '+', 0, 0, NULL);
}

/* Begin the user transfer process.
 */
static void
start_sending_users(int idx)
{
  char share_file[1024] = "";
  int i = 1, j = -1;

  egg_snprintf(share_file, sizeof share_file, "%s.share.%s.%li", tempdir, dcc[idx].nick, now);

  write_tmp_userfile(share_file, userlist, idx);

/* compress.mod
  if (!compress_file(share_file, compress_level)) {
    unlink(share_file);
    dprintf(idx, "s e %s\n", "uff parsing failed");
    putlog(LOG_BOTS, "*", "uff parsing failed");
    dcc[idx].status &= ~(STAT_SHARE | STAT_SENDING | STAT_AGGRESSIVE);
    return;
  }
*/

  if ((i = raw_dcc_send(share_file, "*users", "(users)", share_file, &j)) > 0) {
    unlink(share_file);
    dprintf(idx, "s e %s\n", USERF_CANTSEND);
    putlog(LOG_BOTS, "@", "%s -- can't send userfile",
           i == DCCSEND_FULL ? "NO MORE DCC CONNECTIONS" :
           i == DCCSEND_NOSOCK ? "CAN'T OPEN A LISTENING SOCKET" :
           i == DCCSEND_BADFN ? "BAD FILE" : i == DCCSEND_FEMPTY ? "EMPTY FILE" : "UNKNOWN REASON!");
    dcc[idx].status &= ~(STAT_SHARE | STAT_SENDING | STAT_AGGRESSIVE);
  } else {
    updatebot(-1, dcc[idx].nick, '+', 0, 0, NULL);
    dcc[idx].status |= STAT_SENDING;
    strcpy(dcc[j].host, dcc[idx].nick); /* Store bot's nick */
    dprintf(idx, "s us %lu %d %lu\n", iptolong(getmyip()), dcc[j].port, dcc[j].u.xfer->length);
    /* Start up a tbuf to queue outgoing changes for this bot until the
     * userlist is done transferring.
     */
    new_tbuf(dcc[idx].nick);
    /* override shit removed here */
    q_tbuf(dcc[idx].nick, "s !\n");
    /* Unlink the file. We don't really care whether this causes problems
     * for NFS setups. It's not worth the trouble.
     */
    unlink(share_file);
  }
}

static void (*def_dcc_bot_kill) (int, void *) = 0;

static void
cancel_user_xfer(int idx, void *x)
{
  int i, j = -1, k = 0;

  if (idx < 0) {
    idx = -idx;
    k = 1;
    updatebot(-1, dcc[idx].nick, '-', 0, 0, NULL);
  }
  flush_tbuf(dcc[idx].nick);

  if (dcc[idx].status & STAT_SHARE) {
    if (dcc[idx].status & STAT_GETTING) {
      for (i = 0; i < dcc_total; i++)
        if (dcc[i].type && !egg_strcasecmp(dcc[i].host, dcc[idx].nick) &&
            ((dcc[i].type->flags & (DCT_FILETRAN | DCT_FILESEND)) == (DCT_FILETRAN | DCT_FILESEND))) {
          j = i;
          break;
        }
      if (j >= 0) {
        killsock(dcc[j].sock);
        unlink(dcc[j].u.xfer->filename);
        lostdcc(j);
      }
      putlog(LOG_BOTS, "*", "(Userlist download aborted.)");
    }
    if (dcc[idx].status & STAT_SENDING) {
      for (i = 0; i < dcc_total; i++)
        if (dcc[i].type && (!egg_strcasecmp(dcc[i].host, dcc[idx].nick)) &&
            ((dcc[i].type->flags & (DCT_FILETRAN | DCT_FILESEND))
             == DCT_FILETRAN)) {
          j = i;
          break;
        }
      if (j >= 0) {
        killsock(dcc[j].sock);
        unlink(dcc[j].u.xfer->filename);
        lostdcc(j);
      }
      putlog(LOG_BOTS, "*", "(Userlist transmit aborted.)");
    }
  }
  if (!k)
    def_dcc_bot_kill(idx, x);
}

void
share_report(int idx, int details)
{
  int i, j;

  if (details) {
    for (i = 0; i < dcc_total; i++)
      if (dcc[i].type && dcc[i].type == &DCC_BOT) {
        if (dcc[i].status & STAT_GETTING) {
          int ok = 0;

          for (j = 0; j < dcc_total; j++)
            if (dcc[j].type && ((dcc[j].type->flags & (DCT_FILETRAN | DCT_FILESEND))
                 == (DCT_FILETRAN | DCT_FILESEND)) && !egg_strcasecmp(dcc[j].host, dcc[i].nick)) {
              dprintf(idx, "Downloading userlist from %s (%d%% done)\n",
                      dcc[i].nick, (int) (100.0 * ((float) dcc[j].status) / ((float) dcc[j].u.xfer->length)));
              ok = 1;
              break;
            }
          if (!ok)
            dprintf(idx, "Download userlist from %s (negotiating " "botentries)\n", dcc[i].nick);
        } else if (dcc[i].status & STAT_SENDING) {
          for (j = 0; j < dcc_total; j++) {
            if (dcc[j].type && ((dcc[j].type->flags & (DCT_FILETRAN | DCT_FILESEND))
                 == DCT_FILETRAN)
                && !egg_strcasecmp(dcc[j].host, dcc[i].nick)) {
              if (dcc[j].type == &DCC_GET)
                dprintf(idx, "Sending userlist to %s (%d%% done)\n",
                        dcc[i].nick,
                        (int) (100.0 * ((float) dcc[j].status) / ((float) dcc[j].u.xfer->length)));
              else
                dprintf(idx, "Sending userlist to %s (waiting for connect)\n", dcc[i].nick);
            }
          }
        } else if (dcc[i].status & STAT_AGGRESSIVE) {
          dprintf(idx, "    Passively sharing with %s.\n", dcc[i].nick);
        } else if (dcc[i].status & STAT_SHARE) {
          dprintf(idx, "    Aggressively sharing with %s.\n", dcc[i].nick);
        }
      }
  }
}

void
share_init()
{
  if (conf.bot->hub)
    timer_create_secs(60, "check_expired_tbufs", (Function) check_expired_tbufs);
  else
    timer_create_secs(1, "check_delay", (Function) check_delay);
  def_dcc_bot_kill = DCC_BOT.kill;
  DCC_BOT.kill = cancel_user_xfer;
}
