/*
 * statistics module - script interface to internal statistics manager
 *
 * Copyright (C) 2006 Voice Sistem S.R.L.
 *
 * This file is part of opensips, a free SIP server.
 *
 * opensips is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 * opensips is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 *
 * History:
 * --------
 *  2006-03-14  initial version (bogdan)
 */


#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../../sr_module.h"
#include "../../dprint.h"
#include "../../ut.h"
#include "../../mod_fix.h"
#include "../../statistics.h"
#include "../../mem/mem.h"
#include "../../trim.h"
#include "../../lib/list.h"
#include "stats_funcs.h"



static int reg_param_stat( modparam_t type, void* val);
static int reg_stat_group( modparam_t type, void* val);
static int mod_init(void);
static void mod_destroy(void);
static int w_update_stat(struct sip_msg* msg, char* stat, char* n);
static int w_reset_stat(struct sip_msg* msg, char* stat, char* foo);
static int fixup_stat(void** param, int param_no);
static int fixup_iter_init(void** param, int param_no);
static int fixup_iter_next(void** param, int param_no);
static int _fixup_iter_param(void **param);

int pv_parse_name(pv_spec_p sp, str *in);
int pv_set_stat(struct sip_msg* msg, pv_param_t *param, int op,
													pv_value_t *val);
int pv_get_stat(struct sip_msg *msg,  pv_param_t *param, pv_value_t *res);
static int w_stat_iter_init(struct sip_msg *msg, char *module, char *pv_iter);
static int w_stat_iter_next(struct sip_msg *msg, char *key, char *val, char *pv_iter);


#define STAT_PARAM_TYPE_STAT  1
#define STAT_PARAM_TYPE_NAME  2
#define STAT_PARAM_TYPE_PVAR  3
#define STAT_PARAM_TYPE_FMT   4
struct stat_param {
	unsigned int type;
	union {
		stat_var   *stat;
		pv_spec_t  *pvar;
		str        *name;
		pv_elem_t  *format;
	} u;
};

struct stat_iter {
	str name;
	stat_var *cur;
	struct list_head list;
};

struct list_head script_iters;

static cmd_export_t cmds[]={
	{"update_stat",  (cmd_function)w_update_stat,  2, fixup_stat, 0,
		REQUEST_ROUTE|BRANCH_ROUTE|FAILURE_ROUTE|ONREPLY_ROUTE|
		LOCAL_ROUTE|STARTUP_ROUTE|TIMER_ROUTE|EVENT_ROUTE},
	{"reset_stat",   (cmd_function)w_reset_stat,    1, fixup_stat, 0,
		REQUEST_ROUTE|BRANCH_ROUTE|FAILURE_ROUTE|ONREPLY_ROUTE|
		LOCAL_ROUTE|STARTUP_ROUTE|TIMER_ROUTE|EVENT_ROUTE},
	{"stat_iter_init",  (cmd_function)w_stat_iter_init,  2, fixup_iter_init, 0,
		REQUEST_ROUTE|BRANCH_ROUTE|FAILURE_ROUTE|ONREPLY_ROUTE|
		LOCAL_ROUTE|STARTUP_ROUTE|TIMER_ROUTE|EVENT_ROUTE},
	{"stat_iter_next",  (cmd_function)w_stat_iter_next,  3, fixup_iter_next, 0,
		REQUEST_ROUTE|BRANCH_ROUTE|FAILURE_ROUTE|ONREPLY_ROUTE|
		LOCAL_ROUTE|STARTUP_ROUTE|TIMER_ROUTE|EVENT_ROUTE},
	{0,0,0,0,0,0}
};

static param_export_t mod_params[]={
	{ "variable",  STR_PARAM|USE_FUNC_PARAM, (void*)reg_param_stat },
	{ "stat_groups",  STR_PARAM|USE_FUNC_PARAM, (void*)reg_stat_group },
	{ 0,0,0 }
};


static pv_export_t mod_items[] = {
	{ {"stat",     sizeof("stat")-1},      1100, pv_get_stat,
		pv_set_stat,    pv_parse_name, 0, 0, 0},
	{ {0, 0}, 0, 0, 0, 0, 0, 0, 0 }
};



struct module_exports exports= {
	"statistics",		/* module's name */
	MOD_TYPE_DEFAULT,/* class of this module */
	MODULE_VERSION,
	DEFAULT_DLFLAGS,	/* dlopen flags */
	NULL,				/* OpenSIPS module dependencies */
	cmds,				/* exported functions */
	0,					/* exported async functions */
	mod_params,			/* param exports */
	0,					/* exported statistics */
	0,					/* exported MI functions */
	mod_items,			/* exported pseudo-variables */
	0,					/* extra processes */
	mod_init,			/* module initialization function */
	0,					/* reply processing function */
	mod_destroy,		/* module destroy function */
	0					/* per-child init function */
};



static int reg_param_stat( modparam_t type, void* val)
{
	return reg_statistic( (char*)val);
}

static int reg_stat_group(modparam_t type, void *val)
{
	char *it, *p, save;
	str group;
	int len;

	len = strlen(val);
	it = val;
	do {
		p = strchr(it, ',');

		group.s = it;
		group.len = p ? (p - it) : ((char *)val + len - it);
		trim(&group);

		if (!group.s || group.len <= 0) {
			goto next;
		}

		save = group.s[group.len];
		group.s[group.len] = '\0';

		LM_DBG("creating stat group '%s' ...\n", group.s);
		if (!add_stat_module(group.s)) {
			LM_ERR("failed to add stat group '%s'!\n", group.s);
			return -1;
		}

		group.s[group.len] = save;
next:
		it = p + 1;
	} while (p);

	return 0;
}


static int mod_init(void)
{
	LM_INFO("initializing\n");

	INIT_LIST_HEAD(&script_iters);

	if (register_all_mod_stats()!=0) {
		LM_ERR("failed to register statistic variables\n");
		return E_UNSPEC;
	}
	return 0;
}


static void mod_destroy(void)
{
	struct list_head *ele, *next;
	struct stat_iter *iter;

	list_for_each_safe(ele, next, &script_iters) {
		iter = list_entry(ele, struct stat_iter, list);
		list_del(&iter->list);
		pkg_free(iter);
	}
}

static int resolve_stat(str *in, str *out_group, str *out_name, int *out_grp_idx)
{
	module_stats *ms;

	parse_groupname(in, out_group, out_name);
	if (out_group->s) {
		ms = get_stat_module(out_group);
		if (!ms) {
			LM_ERR("stat group '%.*s' must be explicitly defined "
			       "using the 'stat_groups' module parameter!\n",
			       out_group->len, out_group->s);
			return -1;
		}
		*out_grp_idx = ms->idx;
	} else {
		*out_grp_idx = -1;
	}

	return 0;
}

static int fixup_stat(void** param, int param_no)
{
	struct stat_param *sp;
	pv_elem_t *format;
	str s, sname, group;
	int grp_idx __attribute__((unused));

	s.s = (char*)*param;
	s.len = strlen(s.s);
	if (param_no==1) {
		/* reference to the statistic name */
		sp = (struct stat_param *)pkg_malloc(sizeof(struct stat_param));
		if (sp==NULL) {
			LM_ERR("no more pkg mem (%d)\n", (int)sizeof(struct stat_param));
			return E_OUT_OF_MEM;
		}
		memset( sp, 0 , sizeof(struct stat_param) );
		/* parse it */
		if (pv_parse_format( &s, &sp->u.format)!=0) {
			LM_ERR("failed to parse statistic name format <%s> \n",s.s);
			return E_CFG;
		}
		format = sp->u.format;
		/* is it only one token ? */
		if (format->next==NULL && (format->text.len==0 || format->spec.type==PVT_NONE)) {
			if (format->text.s && format->text.len) {
				if (resolve_stat(&format->text, &group, &sname, &grp_idx) != 0) {
					return E_CFG;
				}
				/* text token */
				sp->u.stat = __get_stat(&sname, grp_idx);
				if (sp->u.stat) {
					/* statistic found */
					sp->type = STAT_PARAM_TYPE_STAT;
				} else {
					/* stat not found, keep the name for later */
					sp->type = STAT_PARAM_TYPE_NAME;
					sp->u.name = &format->text;
				}
			} else {
				/* pvar token */
				sp->type = STAT_PARAM_TYPE_PVAR;
				sp->u.pvar = &format->spec;
			}
			/* we do not free "format" as we keep links inside of it! */
		} else {
			/* if more tokens, keep the entire format */
			sp->type = STAT_PARAM_TYPE_FMT;
		}
		/* do not free the original string, the "format" points inside ! */
		*param=(void*)sp;
		return 0;
	} else if (param_no==2) {
		/* update value - integer or variable */
		return fixup_igp(param);
	}
	return 0;
}

static int _fixup_iter_param(void **param)
{
	str name;
	struct list_head *ele;
	struct stat_iter *iter;

	list_for_each(ele, &script_iters) {
		iter = list_entry(ele, struct stat_iter, list);

		name.s = *param;
		name.len = strlen(name.s);
		if (str_strcmp(&name, &iter->name) == 0) {
			*param = &iter->cur;
			return 0;
		}
	}

	iter = pkg_malloc(sizeof *iter);
	if (!iter) {
		LM_ERR("oom!\n");
		return E_OUT_OF_MEM;
	}
	memset(iter, 0, sizeof *iter);

	iter->name.s = *param;
	iter->name.len = strlen(*param);
	list_add(&iter->list, &script_iters);

	*param = &iter->cur;
	return 0;
}

static int fixup_iter_init(void** param, int param_no)
{
	str *group;

	if (param_no == 1) {
		group = pkg_malloc(sizeof *group);
		if (!group) {
			LM_ERR("oom\n");
			return E_OUT_OF_MEM;
		}
		group->s = *param;
		group->len = strlen(*param);
		if (!get_stat_module(group)) {
			pkg_free(group);
			LM_ERR("stat group '%.*s' must be explicitly defined using the "
			       "'stat_groups' module parameter!\n", group->len, group->s);
			return E_UNSPEC;
		}
		*param = group;
	} else if (param_no == 2) {
		return _fixup_iter_param(param);
	} else {
		LM_ERR("invalid parameter number %d\n", param_no);
		return E_UNSPEC;
	}

	return 0;
}

static int fixup_iter_next(void** param, int param_no)
{
	if (param_no == 1 || param_no == 2) {
	    return fixup_pvar(param);
	}

	if (param_no == 3) {
	    return _fixup_iter_param(param);
	}

	LM_ERR("invalid parameter number %d\n", param_no);
	return E_UNSPEC;
}

static int w_update_stat(struct sip_msg *msg, char *stat_p, char *val)
{
	struct stat_param *sp = (struct stat_param *)stat_p;
	pv_value_t pv_val;
	stat_var *stat;
	int n;
	str name, group;
	int grp_idx __attribute__((unused));

	/* evaluate the value first */
	if (fixup_get_ivalue( msg, (gparam_p)val, &n)<0) {
		LM_ERR("failed to extran a numerical value\n");
		return -1;
	}
	/* update with 0 value makes no sense */
	if (n==0)
		return 1;

	if (sp->type==STAT_PARAM_TYPE_STAT) {
		/* we have the statistic */
		update_stat( sp->u.stat, (long)n);
		return 1;
	}

	if (sp->type==STAT_PARAM_TYPE_PVAR) {
		/* take name from PVAR */
		if (pv_get_spec_value(msg, sp->u.pvar, &pv_val)!=0 ||
		(pv_val.flags & PV_VAL_STR)==0 ) {
			LM_ERR("failed to get pv string value\n");
			return -1;
		}
	} else if (sp->type==STAT_PARAM_TYPE_FMT) {
		/* take name from FMT */
		if (pv_printf_s( msg, sp->u.format, &(pv_val.rs) )!=0 ) {
			LM_ERR("failed to get format string value\n");
			return -1;
		}
	} else if (sp->type==STAT_PARAM_TYPE_NAME) {
		/* take name from STRING */
		pv_val.rs = *sp->u.name;
	}

	LM_DBG("needed statistic is <%.*s>\n", pv_val.rs.len, pv_val.rs.s);

	if (resolve_stat(&pv_val.rs, &group, &name, &grp_idx) != 0) {
		return E_CFG;
	}

	stat = __get_stat(&name, grp_idx);
	if ( stat==NULL ) {
		/* stats not found -> create it */
		LM_DBG("creating statistic <%.*s>\n", pv_val.rs.len, pv_val.rs.s);

		if (grp_idx > 0) {
			if (__register_dynamic_stat(&group, &name, &stat) != 0) {
				LM_ERR("failed to create statistic <%.*s:%.*s>\n",
				       group.len, group.s, name.len, name.s);
				return -1;
			}
		} else {
			if (register_dynamic_stat(&name, &stat)!=0) {
				LM_ERR("failed to create statistic <%.*s>\n",
				       name.len, name.s);
				return -1;
			}
		}
		if (sp->type==STAT_PARAM_TYPE_NAME) {
			sp->u.stat = stat;
			sp->type=STAT_PARAM_TYPE_STAT;
		}
	}

	/* statistic exists ! */
	update_stat( stat, (long)n);
	return 1;
}


static int w_reset_stat(struct sip_msg *msg, char* stat_p, char *foo)
{
	struct stat_param *sp = (struct stat_param *)stat_p;
	pv_value_t pv_val;
	stat_var *stat;
	str group, name;
	int grp_idx __attribute__((unused));

	if (sp->type==STAT_PARAM_TYPE_STAT) {
		/* we have the statistic */
		reset_stat( sp->u.stat);
		return 1;
	}

	if (sp->type==STAT_PARAM_TYPE_PVAR) {
		/* take name from PVAR */
		if (pv_get_spec_value(msg, sp->u.pvar, &pv_val)!=0 ||
		(pv_val.flags & PV_VAL_STR)==0 ) {
			LM_ERR("failed to get pv string value\n");
			return -1;
		}
	} else if (sp->type==STAT_PARAM_TYPE_FMT) {
		/* take name from FMT */
		if (pv_printf_s( msg, sp->u.format, &(pv_val.rs) )!=0 ) {
			LM_ERR("failed to get format string value\n");
			return -1;
		}
	} else if (sp->type==STAT_PARAM_TYPE_NAME) {
		/* take name from STRING */
		pv_val.rs = *sp->u.name;
	}

	LM_DBG("needed statistic is <%.*s>\n", pv_val.rs.len, pv_val.rs.s);

	if (resolve_stat(&pv_val.rs, &group, &name, &grp_idx) != 0) {
		return E_CFG;
	}

	stat = __get_stat(&name, grp_idx);
	if ( stat==NULL ) {
		/* stats not found -> create it */
		LM_DBG("creating statistic <%.*s>\n", pv_val.rs.len, pv_val.rs.s);

		if (grp_idx > 0) {
			if (__register_dynamic_stat(&group, &name, &stat) != 0) {
				LM_ERR("failed to create statistic <%.*s:%.*s>\n",
				       group.len, group.s, name.len, name.s);
				return -1;
			}
		} else {
			if (register_dynamic_stat( &name, &stat )!=0) {
				LM_ERR("failed to create statistic <%.*s>\n",
				       name.len, name.s);
				return -1;
			}
		}
		if (sp->type==STAT_PARAM_TYPE_NAME) {
			sp->u.stat = stat;
			sp->type=STAT_PARAM_TYPE_STAT;
		}
	}

	/* statistic exists ! */
	reset_stat( stat );
	return 1;
}


int pv_parse_name(pv_spec_p sp, str *in)
{
	stat_var *stat;
	pv_elem_t *format;

	if(in==NULL || in->s==NULL || sp==NULL)
		return -1;

	LM_DBG("name %p with name <%.*s>\n", &sp->pvp.pvn, in->len, in->s);
	if (pv_parse_format( in, &format)!=0) {
		LM_ERR("failed to parse statistic name format <%.*s> \n",
			in->len,in->s);
		return -1;
	}

	/* text only ? */
	if (format->next==NULL && format->spec.type==PVT_NONE) {

		/* search for the statistic */
		stat = get_stat( &format->text );

		if (stat==NULL) {
			/* statistic does not exist (yet) -> fill in the string name */
			sp->pvp.pvn.type = PV_NAME_INTSTR;
			sp->pvp.pvn.u.isname.type = AVP_NAME_STR;
			if (clone_pv_stat_name( in, &sp->pvp.pvn.u.isname.name.s )!=0) {
				LM_ERR("failed to clone name of statistic \n");
				return -1;
			}
			LM_DBG("name %p, name cloned (in=%p, out=%p)\n",
				&sp->pvp.pvn, in->s, sp->pvp.pvn.u.isname.name.s.s);
		} else {
			/* link the stat pointer directly as dynamic name */
			sp->pvp.pvn.type = PV_NAME_PVAR;
			sp->pvp.pvn.u.dname = (void*)stat;
			LM_DBG("name %p, stat found\n", &sp->pvp.pvn);
		}

	} else {

			sp->pvp.pvn.type = PV_NAME_INTSTR;
			sp->pvp.pvn.u.isname.type = 0; /* not string */
			sp->pvp.pvn.u.isname.name.s.s = (char*)(void*)format;
			sp->pvp.pvn.u.isname.name.s.len = 0;
			LM_DBG("name %p, stat name is FMT\n", &sp->pvp.pvn);

	}

	return 0;
}


static inline int get_stat_name(struct sip_msg* msg, pv_name_t *name,
												int create, stat_var **stat)
{
	pv_value_t pv_val;
	str sname, group;
	int grp_idx __attribute__((unused));

	/* is the statistic found ? */
	if (name->type==PV_NAME_INTSTR) {
		LM_DBG("stat with name %p still not found\n", name);
		/* not yet :( */
		/* do we have at least the name ?? */
		if (name->u.isname.type==0) {
			/* name is FMT */
			if (pv_printf_s( msg, (pv_elem_t *)name->u.isname.name.s.s,
			&(pv_val.rs) )!=0) {
				LM_ERR("failed to get format string value\n");
				return -1;
			}
		} else {
			/* name is string */
			pv_val.rs = name->u.isname.name.s;
		}

		if (resolve_stat(&pv_val.rs, &group, &sname, &grp_idx) != 0) {
			return E_CFG;
		}

		/* lookup for the statistic */
		*stat = __get_stat(&sname, grp_idx);
		LM_DBG("stat name %p (%.*s) after lookup is %p\n",
		       name, pv_val.rs.len, pv_val.rs.s, *stat);
		if (*stat==NULL) {
			if (!create)
				return 0;
			LM_DBG("creating statistic <%.*s>\n", pv_val.rs.len, pv_val.rs.s);
			if (grp_idx > 0) {
				if (__register_dynamic_stat(&group, &sname, stat) != 0) {
					LM_ERR("failed to create statistic <%.*s>\n",
					       pv_val.rs.len, pv_val.rs.s);
					return -1;
				}
			} else {
				if (register_dynamic_stat(&sname, stat)!=0) {
					LM_ERR("failed to create statistic <%.*s>\n",
					       pv_val.rs.len, pv_val.rs.s);
					return -1;
				}
			}
		}
		/* if name is static string, better link the stat directly
		 * and discard name */
		if (name->u.isname.type==AVP_NAME_STR) {
			LM_DBG("name %p freeing %p\n",name,name->u.isname.name.s.s);
			/* it is totally unsafe to free this shm block here, as it is
			 * referred by the spec from all the processess. Even if we create
			 * here a small leak (one time only), we do not have a better fix
			 * until a final review of the specs in pkg and shm mem - bogdan */
			//shm_free(name->u.isname.name.s.s);
			name->u.isname.name.s.s = NULL;
			name->u.isname.name.s.len = 0;
			name->type = PV_NAME_PVAR;
			name->u.dname = (void*)*stat;
		}
	} else {
		/* stat already found ! */
		*stat = (stat_var*)name->u.dname;
		LM_DBG("found stat name %p\n",name);
	}

	return 0;
}

static int w_stat_iter_init(struct sip_msg *msg, char *group, char *ppstat)
{
	module_stats *ms;
	str *grp = (str *)group;
	stat_var **stat = (stat_var **)ppstat;

	ms = get_stat_module((str *)group);
	if (!ms) {
		LM_ERR("unknown group %.*s\n", grp->len, grp->s);
		return -1;
	}
	*stat = ms->head;

	return 1;
}

static int w_stat_iter_next(struct sip_msg *msg, char *key, char *val, char *pps)
{
	pv_value_t pval;
	stat_var **ppstat = (stat_var **)pps, *stat = *ppstat;

	if (!stat) {
		LM_DBG("no more stats to iterate\n");
		return -1;
	}

	pval.flags = PV_VAL_STR;
	pval.rs = stat->name;
	if (pv_set_value(msg, (pv_spec_p)key, 0, &pval) != 0) {
		LM_ERR("failed to set pv value for stat key '%.*s'\n",
		       stat->name.len, stat->name.s);
		return -1;
	}

	pval.flags = PV_VAL_INT|PV_TYPE_INT;
	pval.ri = get_stat_val(stat);
	if (pv_set_value(msg, (pv_spec_p)val, 0, &pval) != 0) {
		LM_ERR("failed to set pv value for stat val '%d'\n",
		       pval.ri);
		return -1;
	}

	*ppstat = stat->lnext;
	return 1;
}

int pv_set_stat(struct sip_msg* msg, pv_param_t *param, int op,
													pv_value_t *val)
{
	stat_var *stat;

	if (get_stat_name( msg, &(param->pvn), 1, &stat)!=0) {
		LM_ERR("failed to generate/get statistic name\n");
		return -1;
	}

	if (val->ri != 0)
		LM_WARN("non-zero value - setting value to 0\n");

	reset_stat( stat );

	return 0;
}


int pv_get_stat(struct sip_msg *msg,  pv_param_t *param, pv_value_t *res)
{
	stat_var *stat;

	if(msg==NULL || res==NULL)
		return -1;

	if (get_stat_name( msg, &(param->pvn), 0, &stat)!=0) {
		LM_ERR("failed to generate/get statistic name\n");
		return -1;
	}

	if (stat==NULL)
		return pv_get_null(msg, param, res);

	res->ri = (int)get_stat_val( stat );
	res->rs.s = sint2str(res->ri, &res->rs.len);
	res->flags = PV_VAL_INT|PV_VAL_STR|PV_TYPE_INT;

	return 0;
}
