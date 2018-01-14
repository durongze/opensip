/*
 * mid-registrar User location callbacks
 *
 * This module is intended to be used as a middle layer SIP component in
 * environments where a large proportion of SIP UAs (e.g. mobile devices)
 * register at high enough frequencies that they actually degrade the
 * performance of their registrars.
 *
 * Copyright (C) 2016 OpenSIPS Solutions
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
 *  2016-10-31 initial version (liviu)
 */

#include "../usrloc/ul_callback.h"

#include "../../parser/contact/contact.h"
#include "../tm/tm_load.h"
#include "../tm/dlg.h"
#include "../../lib/reg/rerrno.h"
#include "../../lib/reg/regtime.h"

#include "mid_registrar.h"

char extra_hdrs_buf[512];
static str extra_hdrs={extra_hdrs_buf, 512};

static int build_unregister_hdrs(struct mid_reg_info *mri)
{
	char *p;

	p = extra_hdrs.s;
	memcpy(p, contact_hdr.s, contact_hdr.len);
	p += contact_hdr.len;

	LM_DBG("building contact from uri '%.*s'\n", mri->ct_uri.len, mri->ct_uri.s);

	*p++ = '<';
	memcpy(p, mri->ct_uri.s, mri->ct_uri.len);
	p += mri->ct_uri.len;
	*p++ = '>';

	*p++ = ';';
	memcpy(p, expires_param.s, expires_param.len);
	p += expires_param.len;
	*p++ = '=';

	*p++ = '0';
	memcpy(p, CRLF, CRLF_LEN); p += CRLF_LEN;

	extra_hdrs.len = (int)(p - extra_hdrs.s);
	LM_DBG("extra hdrs: '%.*s'\n", extra_hdrs.len, extra_hdrs.s);

	return 0;
}

static void reg_tm_cback(struct cell *t, int type, struct tmcb_params *ps)
{
	LM_DBG(">> [REPLY] UNREGISTER !\n");
}

static int unregister_contact(struct mid_reg_info *mri)
{
	dlg_t *dlg;
	int ret;

	/* create a mystical dialog in preparation for our De-REGISTER */
	if (tm_api.new_auto_dlg_uac(&mri->from, &mri->to, &mri->main_reg_uri,
	    &mri->callid, NULL, &dlg)) {
		LM_ERR("failed to create new TM dlg\n");
		return -1;
	}
	dlg->state = DLG_CONFIRMED;

	if (build_unregister_hdrs(mri) != 0) {
		LM_ERR("failed to build unregister headers\n");
		return -1;
	}

	ret = tm_api.t_request_within(
		&register_method,	/* method */
		&extra_hdrs,		/* extra headers*/
		NULL,			/* body */
		dlg,		/* dialog structure*/
		reg_tm_cback,		/* callback function */
		NULL,	/* callback param */
		NULL);	/* function to release the parameter */

	tm_api.free_dlg(dlg);

	return (ret == 1 ? 0 : ret);
}

void mid_reg_ct_event(void *binding, int type, void **data)
{
	ucontact_t *c = (ucontact_t *)binding;
	struct mid_reg_info *mri, *update;

	if (!data)
		return;

	update = get_ct();

	LM_DBG("Contact callback (%d): contact='%.*s' | "
	       "param=(%p -> %p) | data[%d]=(%p)\n", type, c->c.len, c->c.s, data,
	       data ? *data : NULL, ucontact_data_idx,
	       c->attached_data[ucontact_data_idx]);

	if (type & UL_CONTACT_INSERT) {
		*data = get_ct();
	} else {
		mri = *(struct mid_reg_info **)data;
		if (!mri)
			return;

		if (type & UL_CONTACT_UPDATE) {
			if (update) {
				LM_DBG("setting e_out to %d\n", update->expires_out);
				mri->expires_out = update->expires_out;
				mri->last_reg_ts = get_act_time();
			}
		} else if (type & (UL_CONTACT_DELETE|UL_CONTACT_EXPIRE)) {
			if (reg_mode == MID_REG_THROTTLE_CT) {
				if (!mri->skip_dereg && unregister_contact(mri) != 0) {
					LM_ERR("failed to unregister contact\n");
				}
			}
			mri_free(mri);
		}
	}
}

void mid_reg_aor_event(void *binding, int type, void **data)
{
	urecord_t *r = (urecord_t *)binding;
	struct mid_reg_info *mri;

	if (!data)
		return;

	LM_DBG("AOR callback (%d): contact='%.*s' | "
	       "param=(%p -> %p) | data[%d]=(%p)\n", type,
	       r->aor.len, r->aor.s, data, data ? *data : NULL,
	       urecord_data_idx, r->attached_data[urecord_data_idx]);

	if (type & UL_AOR_INSERT) {
		*data = get_ct();
	} else if (type & (UL_AOR_DELETE|UL_AOR_EXPIRE)) {
		mri = *(struct mid_reg_info **)data;
		if (!mri)
			return;

		if (!mri->skip_dereg && unregister_contact(mri) != 0)
			LM_ERR("failed to unregister contact\n");

		mri_free(mri);
	}
}
