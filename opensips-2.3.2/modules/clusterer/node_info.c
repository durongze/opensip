/*
 * Copyright (C) 2015-2017 OpenSIPS Project
 *
 * This file is part of opensips, a free SIP server.
 *
 * opensips is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * opensips is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 *
 * history:
 * ---------
 *	2016-07-xx split from clusterer.c (rvlad-patrascu)
 */

#include "../../str.h"
#include "../../dprint.h"
#include "../../mem/mem.h"
#include "../../mem/shm_mem.h"
#include "../../locking.h"
#include "../../rw_locking.h"
#include "../../resolve.h"
#include "../../socket_info.h"

#include "api.h"
#include "node_info.h"
#include "clusterer.h"

/* DB */
extern str clusterer_db_url;
extern str db_table;
extern str id_col;
extern str cluster_id_col;
extern str node_id_col;
extern str url_col;
extern str state_col;
extern str ls_seq_no_col;
extern str top_seq_no_col;
extern str no_ping_retries_col;
extern str priority_col;
extern str sip_addr_col;
extern str description_col;

db_con_t *db_hdl;
db_func_t dr_dbf;

static db_op_t op_eq = OP_EQ;
static db_key_t *clusterer_cluster_id_key;
static db_val_t *clusterer_cluster_id_value;

/* protects the cluster_list and the node_list from each cluster */
rw_lock_t *cl_list_lock;

cluster_info_t **cluster_list;

int add_node_info(node_info_t **new_info, cluster_info_t **cl_list, int *int_vals,
					char **str_vals)
{
	char *host;
	int hlen, port;
	int proto;
	struct hostent *he;
	int cluster_id;
	cluster_info_t *cluster = NULL;
	struct timeval t;
	str st;
	struct mod_registration *mod;
	struct cluster_mod *new_cl_mod = NULL;
	int i;

	cluster_id = int_vals[INT_VALS_CLUSTER_ID_COL];

	for (cluster = *cl_list; cluster && cluster->cluster_id != cluster_id;
		cluster = cluster->next) ;

	if (!cluster) {
		cluster = shm_malloc(sizeof *cluster);
		if (!cluster) {
			LM_ERR("no more shm memory\n");
			goto error;
		}
		memset(cluster, 0, sizeof *cluster);

		cluster->modules = NULL;

		/* look for modules that registered for this cluster */
		for (mod = clusterer_reg_modules; mod; mod = mod->next)
			for (i = 0; i < mod->no_accept_clusters; i++)
				if (mod->accept_clusters_ids[i] == cluster_id) {
					new_cl_mod = shm_malloc(sizeof *new_cl_mod);
					if (!new_cl_mod) {
						LM_ERR("No more shm memory\n");
						goto error;
					}
					new_cl_mod->reg = mod;
					new_cl_mod->next = cluster->modules;
					cluster->modules = new_cl_mod;

					break;
				}

		cluster->cluster_id = cluster_id;
		cluster->join_state = JOIN_INIT;
		cluster->next = *cl_list;
		if ((cluster->lock = lock_alloc()) == NULL) {
			LM_CRIT("Failed to allocate lock\n");
			goto error;
		}
		if (!lock_init(cluster->lock)) {
			lock_dealloc(cluster->lock);
			LM_CRIT("Failed to init lock\n");
			goto error;
		}
		*cl_list = cluster;
	}

	*new_info = NULL;
	*new_info = shm_malloc(sizeof **new_info);
	if (!*new_info) {
		LM_ERR("no more shm memory\n");
		goto error;
	}
	memset(*new_info, 0, sizeof **new_info);

	(*new_info)->flags = DB_UPDATED | DB_PROVISIONED;

	(*new_info)->id = int_vals[INT_VALS_ID_COL];
	(*new_info)->node_id = int_vals[INT_VALS_NODE_ID_COL];
	if (int_vals[INT_VALS_STATE_COL])
		(*new_info)->flags |= NODE_STATE_ENABLED;
	else
		(*new_info)->flags &= ~NODE_STATE_ENABLED;

	if (int_vals[INT_VALS_NODE_ID_COL] != current_id)
		(*new_info)->link_state = LS_RESTART_PINGING;
	else
		(*new_info)->link_state = LS_UP;

	if (strlen(str_vals[STR_VALS_DESCRIPTION_COL]) != 0) {
		(*new_info)->description.len = strlen(str_vals[STR_VALS_DESCRIPTION_COL]);
		(*new_info)->description.s = shm_malloc((*new_info)->description.len * sizeof(char));
		if ((*new_info)->description.s == NULL) {
			LM_ERR("no more shm memory\n");
			goto error;
		}
		memcpy((*new_info)->description.s, str_vals[STR_VALS_DESCRIPTION_COL],
			(*new_info)->description.len);
	} else {
		(*new_info)->description.s = NULL;
		(*new_info)->description.len = 0;
	}

	if (strlen(str_vals[STR_VALS_SIP_ADDR_COL]) != 0) {
		(*new_info)->sip_addr.len = strlen(str_vals[STR_VALS_SIP_ADDR_COL]);
		(*new_info)->sip_addr.s = shm_malloc((*new_info)->sip_addr.len * sizeof(char));
		if ((*new_info)->sip_addr.s == NULL) {
			LM_ERR("no more shm memory\n");
			goto error;
		}
		memcpy((*new_info)->sip_addr.s, str_vals[STR_VALS_SIP_ADDR_COL],
			(*new_info)->sip_addr.len);
	} else {
		(*new_info)->sip_addr.s = NULL;
		(*new_info)->sip_addr.len = 0;
	}

	if (str_vals[STR_VALS_URL_COL] == NULL) {
		LM_ERR("no url specified in DB\n");
		return 1;
	}
	(*new_info)->url.len = strlen(str_vals[STR_VALS_URL_COL]);
	(*new_info)->url.s = shm_malloc(strlen(str_vals[STR_VALS_URL_COL]) * sizeof(char));
	if (!(*new_info)->url.s) {
		LM_ERR("no more shm memory\n");
		goto error;
	}
	memcpy((*new_info)->url.s, str_vals[STR_VALS_URL_COL], (*new_info)->url.len);

	if (int_vals[INT_VALS_NODE_ID_COL] != current_id) {
		if (parse_phostport((*new_info)->url.s, (*new_info)->url.len, &host, &hlen,
			&port, &proto) < 0) {
			LM_ERR("Bad URL!\n");
			return 1;
		}

		if (proto == PROTO_NONE)
			proto = clusterer_proto;
		if (proto != clusterer_proto) {
			LM_ERR("Clusterer currently supports only BIN protocol, but node: %d "
				"has proto=%d\n", int_vals[INT_VALS_NODE_ID_COL], proto);
			return 1;
		}

		st.s = host;
		st.len = hlen;
		he = sip_resolvehost(&st, (unsigned short *) &port,
			(unsigned short *)&proto, 0, 0);
		if (!he) {
			LM_ERR("Cannot resolve host: %.*s\n", hlen, host);
			return 1;
		}

		hostent2su(&((*new_info)->addr), he, 0, port);

		t.tv_sec = 0;
		t.tv_usec = 0;
		(*new_info)->last_ping = t;
		(*new_info)->last_pong = t;
	}

	(*new_info)->priority = int_vals[INT_VALS_PRIORITY_COL];

	(*new_info)->no_ping_retries = int_vals[INT_VALS_NO_PING_RETRIES_COL];

	(*new_info)->cluster = cluster;

	(*new_info)->ls_seq_no = int_vals[INT_VALS_LS_SEQ_COL];
	(*new_info)->top_seq_no = int_vals[INT_VALS_TOP_SEQ_COL];
	(*new_info)->sp_info = shm_malloc(sizeof(struct node_search_info));
	if (!(*new_info)->sp_info) {
		LM_ERR("no more shm memory\n");
		goto error;
	}
	(*new_info)->sp_info->node = *new_info;

	if (int_vals[INT_VALS_NODE_ID_COL] != current_id) {
		(*new_info)->next = cluster->node_list;
		cluster->node_list = *new_info;
		cluster->no_nodes++;
	} else {
		(*new_info)->next = NULL;
		cluster->current_node = *new_info;
	}

	if (((*new_info)->lock = lock_alloc()) == NULL) {
		LM_CRIT("Failed to allocate lock\n");
		goto error;
	}
	if (!lock_init((*new_info)->lock)) {
		lock_dealloc((*new_info)->lock);
		LM_CRIT("Failed to init lock\n");
		goto error;
	}

	return 0;
error:
	if (*new_info) {
		if ((*new_info)->sip_addr.s)
			shm_free((*new_info)->sip_addr.s);

		if ((*new_info)->description.s)
			shm_free((*new_info)->description.s);

		if ((*new_info)->url.s)
			shm_free((*new_info)->url.s);

		if ((*new_info)->sp_info)
			shm_free((*new_info)->sp_info);

		shm_free(*new_info);
	}
	return -1;
}

#define check_val( _col, _val, _type, _not_null, _is_empty_str) \
    do { \
        if ((_val)->type!=_type) { \
            LM_ERR("column %.*s has a bad type\n", _col.len, _col.s); \
            return 2; \
        } \
        if (_not_null && (_val)->nul) { \
            LM_ERR("column %.*s is null\n", _col.len, _col.s); \
            return 2; \
        } \
        if (_is_empty_str && !VAL_STRING(_val)) { \
            LM_ERR("column %.*s (str) is empty\n", _col.len, _col.s); \
            return 2; \
        } \
    } while (0)

/* loads info from the db */
int load_db_info(db_func_t *dr_dbf, db_con_t* db_hdl, str *db_table, cluster_info_t **cl_list)
{
	int int_vals[NO_DB_INT_VALS];
	char *str_vals[NO_DB_STR_VALS];
	int no_clusters;
	int i;
	int rc;
	node_info_t *new_info = NULL;
	db_key_t columns[NO_DB_COLS];	/* the columns from the db table */
	db_res_t *res = NULL;
	db_row_t *row;
	static db_key_t clusterer_node_id_key = &node_id_col;
	static db_val_t clusterer_node_id_value = {
		.type = DB_INT,
		.nul = 0,
	};

	*cl_list = NULL;

	columns[0] = &id_col;
	columns[1] = &cluster_id_col;
	columns[2] = &node_id_col;
	columns[3] = &url_col;
	columns[4] = &state_col;
	columns[5] = &ls_seq_no_col;
	columns[6] = &top_seq_no_col;
	columns[7] = &no_ping_retries_col;
	columns[8] = &priority_col;
	columns[9] = &sip_addr_col;
	columns[10] = &description_col;

	CON_OR_RESET(db_hdl);

	if (db_check_table_version(dr_dbf, db_hdl, db_table, CLUSTERER_TABLE_VERSION) != 0)
		goto error;

	if (dr_dbf->use_table(db_hdl, db_table) < 0) {
		LM_ERR("cannot select table: \"%.*s\"\n", db_table->len, db_table->s);
		goto error;
	}

	LM_DBG("DB query - retrieve the list of clusters"
		" in which the current node runs\n");

	VAL_INT(&clusterer_node_id_value) = current_id;

	/* first we see in which clusters the current node runs*/
	if (dr_dbf->query(db_hdl, &clusterer_node_id_key, &op_eq,
		&clusterer_node_id_value, columns+1, 1, 1, 0, &res) < 0) {
		LM_ERR("DB query failed - cannot retrieve the list of clusters in which"
			" the current node runs\n");
		goto error;
	}

	LM_DBG("%d rows found in %.*s\n",
		RES_ROW_N(res), db_table->len, db_table->s);

	if (RES_ROW_N(res) == 0) {
		LM_WARN("No nodes found in cluster\n");
		return 1;
	}

	clusterer_cluster_id_key = pkg_realloc(clusterer_cluster_id_key,
		RES_ROW_N(res) * sizeof(db_key_t));
	if (!clusterer_cluster_id_key) {
		LM_ERR("no more pkg memory\n");
		goto error;
	}

	for (i = 0; i < RES_ROW_N(res); i++)
		clusterer_cluster_id_key[i] = &cluster_id_col;

	clusterer_cluster_id_value = pkg_realloc(clusterer_cluster_id_value,
		RES_ROW_N(res) * sizeof(db_val_t));
	if (!clusterer_cluster_id_value) {
		LM_ERR("no more pkg memory\n");
		goto error;
	}

	for (i = 0; i < RES_ROW_N(res); i++) {
		VAL_TYPE(clusterer_cluster_id_value + i) = DB_INT;
		VAL_NULL(clusterer_cluster_id_value + i) = 0;
	}

	for (i = 0; i < RES_ROW_N(res); i++) {
		row = RES_ROWS(res) + i;
		check_val(cluster_id_col, ROW_VALUES(row), DB_INT, 1, 0);
		VAL_INT(clusterer_cluster_id_value + i) = VAL_INT(ROW_VALUES(row));
	}

	no_clusters = RES_ROW_N(res);
	dr_dbf->free_result(db_hdl, res);
	res = NULL;

	LM_DBG("DB query - retrieve nodes info\n");

	CON_USE_OR_OP(db_hdl);

	if (dr_dbf->query(db_hdl, clusterer_cluster_id_key, 0,
		clusterer_cluster_id_value, columns, no_clusters, NO_DB_COLS, 0, &res) < 0) {
		LM_ERR("DB query failed - retrieve valid connections\n");
		goto error;
	}

	LM_DBG("%d rows found in %.*s\n",
		RES_ROW_N(res), db_table->len, db_table->s);

	for (i = 0; i < RES_ROW_N(res); i++) {
		row = RES_ROWS(res) + i;

		check_val(id_col, ROW_VALUES(row), DB_INT, 1, 0);
		int_vals[INT_VALS_ID_COL] = VAL_INT(ROW_VALUES(row));

		check_val(cluster_id_col, ROW_VALUES(row) + 1, DB_INT, 1, 0);
		int_vals[INT_VALS_CLUSTER_ID_COL] = VAL_INT(ROW_VALUES(row) + 1);

		check_val(node_id_col, ROW_VALUES(row) + 2, DB_INT, 1, 0);
		int_vals[INT_VALS_NODE_ID_COL] = VAL_INT(ROW_VALUES(row) + 2);

		check_val(url_col, ROW_VALUES(row) + 3, DB_STRING, 1, 1);
		str_vals[STR_VALS_URL_COL] = (char*) VAL_STRING(ROW_VALUES(row) + 3);

		check_val(state_col, ROW_VALUES(row) + 4, DB_INT, 1, 0);
		int_vals[INT_VALS_STATE_COL] = VAL_INT(ROW_VALUES(row) + 4);

		check_val(ls_seq_no_col, ROW_VALUES(row) + 5, DB_INT, 1, 0);
		int_vals[INT_VALS_LS_SEQ_COL] = VAL_INT(ROW_VALUES(row) + 5);

		check_val(top_seq_no_col, ROW_VALUES(row) + 6, DB_INT, 1, 0);
		int_vals[INT_VALS_TOP_SEQ_COL] = VAL_INT(ROW_VALUES(row) + 6);

		check_val(no_ping_retries_col, ROW_VALUES(row) + 7, DB_INT, 1, 0);
		int_vals[INT_VALS_NO_PING_RETRIES_COL] = VAL_INT(ROW_VALUES(row) + 7);

		check_val(priority_col, ROW_VALUES(row) + 8, DB_INT, 1, 0);
		int_vals[INT_VALS_PRIORITY_COL] = VAL_INT(ROW_VALUES(row) + 8);

		check_val(sip_addr_col, ROW_VALUES(row) + 9, DB_STRING, 0, 0);
		str_vals[STR_VALS_SIP_ADDR_COL] = (char*) VAL_STRING(ROW_VALUES(row) + 9);

		check_val(description_col, ROW_VALUES(row) + 10, DB_STRING, 0, 0);
		str_vals[STR_VALS_DESCRIPTION_COL] = (char*) VAL_STRING(ROW_VALUES(row) + 10);

		/* add info to backing list */
		if ((rc = add_node_info(&new_info, cl_list, int_vals, str_vals)) != 0) {
			LM_ERR("Unable to add node info to backing list\n");
			if (rc < 0)
				return -1;
			else
				return 2;
		}
	}

	if (RES_ROW_N(res) == 1)
		LM_INFO("The current node is the only one in the cluster\n");

	dr_dbf->free_result(db_hdl, res);

	return 0;
error:
	if (res)
		dr_dbf->free_result(db_hdl, res);
	if (*cl_list)
		free_info(*cl_list);
	*cl_list = NULL;
	return -1;
}

int update_db_current(void)
{
	cluster_info_t *cluster;
	db_key_t node_id_key = &id_col;
	db_val_t node_id_val;
	db_key_t update_keys[3];
	db_val_t update_vals[3];
	int ret = 0;

	VAL_TYPE(&node_id_val) = DB_INT;
	VAL_NULL(&node_id_val) = 0;
	VAL_INT(&node_id_val) = current_id;

	update_keys[0] = &ls_seq_no_col;
	update_keys[1] = &top_seq_no_col;
	update_keys[2] = &state_col;

	CON_OR_RESET(db_hdl);

	if (dr_dbf.use_table(db_hdl, &db_table) < 0) {
		LM_ERR("cannot select table: \"%.*s\"\n", db_table.len, db_table.s);
		return -1;
	}

	lock_start_read(cl_list_lock);

	for (cluster = *cluster_list; cluster; cluster = cluster->next) {
		lock_get(cluster->current_node->lock);

		if ((cluster->current_node->flags & (DB_PROVISIONED | DB_UPDATED)) ==
			(DB_PROVISIONED | DB_UPDATED)) {
			lock_release(cluster->current_node->lock);
			continue;
		}

		VAL_TYPE(&update_vals[0]) = DB_INT;
		VAL_NULL(&update_vals[0]) = 0;
		VAL_INT(&update_vals[0]) = cluster->current_node->ls_seq_no;
		VAL_TYPE(&update_vals[1]) = DB_INT;
		VAL_NULL(&update_vals[1]) = 0;
		VAL_INT(&update_vals[1]) = cluster->current_node->top_seq_no;
		VAL_TYPE(&update_vals[2]) = DB_INT;
		VAL_NULL(&update_vals[2]) = 0;
		if (cluster->current_node->flags & NODE_STATE_ENABLED)
			VAL_INT(&update_vals[2]) = STATE_ENABLED;
		else
			VAL_INT(&update_vals[2]) = STATE_DISABLED;

		lock_release(cluster->current_node->lock);

		if (dr_dbf.update(db_hdl, &node_id_key, 0, &node_id_val, update_keys,
			update_vals, 1, 3) < 0) {
			LM_ERR("Failed to update clusterer DB for cluster: %d\n", cluster->cluster_id);
			ret = -1;
		} else {
			lock_get(cluster->current_node->lock);
			cluster->current_node->flags |= DB_UPDATED;
			lock_release(cluster->current_node->lock);
			LM_DBG("Updated clusterer DB for cluster: %d\n", cluster->cluster_id);
		}
	}

	lock_stop_read(cl_list_lock);

	return ret;
}

void free_info(cluster_info_t *cl_list)
{
	cluster_info_t *tmp_cl;
	node_info_t *info, *tmp_info;
	struct cluster_mod *cl_m, *tmp_cl_m;

	while (cl_list != NULL) {
		tmp_cl = cl_list;
		cl_list = cl_list->next;

		info = tmp_cl->node_list;
		while (info != NULL) {
			if (info->url.s)
				shm_free(info->url.s);
			if (info->sip_addr.s)
				shm_free(info->sip_addr.s);
			if (info->description.s)
				shm_free(info->description.s);
			if (info->lock) {
				lock_destroy(info->lock);
				lock_dealloc(info->lock);
			}

			tmp_info = info;
			info = info->next;
			shm_free(tmp_info);
		}

		cl_m = tmp_cl->modules;
		while (cl_m != NULL) {
			tmp_cl_m = cl_m;
			cl_m = cl_m->next;
			shm_free(tmp_cl_m);
		}

		if (tmp_cl->lock) {
			lock_destroy(tmp_cl->lock);
			lock_dealloc(tmp_cl->lock);
		}

		shm_free(tmp_cl);
	}
}

static inline void free_clusterer_node(clusterer_node_t *node)
{
	if (node->description.s)
		pkg_free(node->description.s);
	if (node->sip_addr.s)
		pkg_free(node->sip_addr.s);
	pkg_free(node);
}

static int add_clusterer_node(clusterer_node_t **cl_node_list, node_info_t *n_info)
{
	clusterer_node_t *new_node = NULL;

	new_node = pkg_malloc(sizeof *new_node);
	if (!new_node) {
		LM_ERR("no more pkg memory\n");
		goto error;
	}

	new_node->node_id = n_info->node_id;

	if (n_info->description.s) {
		new_node->description.s = pkg_malloc(n_info->description.len * sizeof(char));
		if (!new_node->description.s) {
			LM_ERR("no more pkg memory\n");
			goto error;
		}
		new_node->description.len = n_info->description.len;
		memcpy(new_node->description.s, n_info->description.s, n_info->description.len);
	} else {
		new_node->description.s = NULL;
		new_node->description.len = 0;
	}

	if (n_info->sip_addr.s) {
		new_node->sip_addr.s = pkg_malloc(n_info->sip_addr.len * sizeof(char));
		if (!new_node->sip_addr.s) {
			LM_ERR("no more pkg memory\n");
			goto error;
		}
		new_node->sip_addr.len = n_info->sip_addr.len;
		memcpy(new_node->sip_addr.s, n_info->sip_addr.s, n_info->sip_addr.len);
	} else {
		new_node->sip_addr.s = NULL;
		new_node->sip_addr.len = 0;
	}

	memcpy(&new_node->addr, &n_info->addr, sizeof(n_info->addr));
	new_node->next = NULL;

	if (*cl_node_list)
		new_node->next = *cl_node_list;

	*cl_node_list = new_node;
	return 0;

error:
	if (new_node)
		free_clusterer_node(new_node);
	return -1;
}

void free_clusterer_nodes(clusterer_node_t *nodes)
{
	clusterer_node_t *tmp;

	while (nodes) {
		tmp = nodes;
		nodes = nodes->next;
		free_clusterer_node(tmp);
	}
}

clusterer_node_t* get_clusterer_nodes(int cluster_id)
{
	clusterer_node_t *ret_nodes = NULL;
	node_info_t *node;
	cluster_info_t *cl;

	lock_start_read(cl_list_lock);

	cl = get_cluster_by_id(cluster_id);
	if (!cl) {
		LM_DBG("cluster id: %d not found!\n", cluster_id);
		goto end;
	}
	for (node = cl->node_list; node; node = node->next) {
		if (get_next_hop(node) > 0)
			if (add_clusterer_node(&ret_nodes, node) < 0) {
				lock_stop_read(cl_list_lock);
				LM_ERR("Unable to add node: %d to the returned list of reachable nodes\n",
					node->node_id);
				free_clusterer_nodes(ret_nodes);
				return NULL;
			}
	}

end:
	lock_stop_read(cl_list_lock);

	return ret_nodes;
}

clusterer_node_t *api_get_next_hop(int cluster_id, int node_id)
{
	clusterer_node_t *ret = NULL;
	node_info_t *dest_node;
	cluster_info_t *cluster;
	int rc;

	lock_start_read(cl_list_lock);

	cluster = get_cluster_by_id(cluster_id);
	if (!cluster) {
		LM_DBG("Cluster id: %d not found!\n", cluster_id);
		return NULL;
	}
	dest_node = get_node_by_id(cluster, node_id);
	if (!dest_node) {
		LM_DBG("Node id: %d no found!\n", node_id);
		return NULL;
	}

	rc = get_next_hop(dest_node);
	if (rc < 0)
		return NULL;
	else if (rc == 0) {
		LM_DBG("No other path to node: %d\n", node_id);
		return NULL;
	}

	lock_get(dest_node->lock);

	if (add_clusterer_node(&ret, dest_node->next_hop) < 0) {
		LM_ERR("Failed to allocate next hop\n");
		return NULL;
	}

	lock_release(dest_node->lock);

	lock_stop_read(cl_list_lock);

	return ret;
}

void api_free_next_hop(clusterer_node_t *next_hop)
{
	if (next_hop)
		free_clusterer_node(next_hop);
}

int cl_get_my_id(void)
{
	return current_id;
}

