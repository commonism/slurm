/*****************************************************************************\
 *  sinfo.c - Report overall state the system
 *****************************************************************************
 *  Copyright (C) 2002 The Regents of the University of California.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Joey Ekstrom <ekstrom1@llnl.gov>, Moe Jette <jette1@llnl.gov>
 *  UCRL-CODE-2002-040.
 *  
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://www.llnl.gov/linux/slurm/>.
 *  
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *  
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *  
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA  02111-1307  USA.
\*****************************************************************************/

#include "src/sinfo/sinfo.h"
#include "src/sinfo/print.h"

/********************
 * Global Variables *
 ********************/
struct sinfo_parameters params;
 
/************
 * Funtions *
 ************/
static int  _build_sinfo_data(List sinfo_list, 
		partition_info_msg_t *partition_msg,
		node_info_msg_t *node_msg, int node_cnt);
static void _create_sinfo(List sinfo_list, partition_info_t* part_ptr, 
		node_info_t *node_ptr);
static void _sinfo_list_delete(void *data);
static void _filter_nodes(node_info_msg_t *node_msg, int *node_rec_cnt);
static partition_info_t *_find_part(char *part_name, 
		partition_info_msg_t *partition_msg);
static int  _query_server(partition_info_msg_t ** part_pptr,
		node_info_msg_t ** node_pptr);
static int  _part_order (void *data1, void *data2);
static void _sort_sinfo_data(List sinfo_list);
static int  _strcmp(char *data1, char *data2);
static void _swap_char(char **from, char **to);
static void _swap_node_rec(node_info_t *from_node, node_info_t *to_node);
static void _update_sinfo(sinfo_data_t *sinfo_ptr, partition_info_t* part_ptr, 
		node_info_t *node_ptr);

int main(int argc, char *argv[])
{
	log_options_t opts = LOG_OPTS_STDERR_ONLY;
	partition_info_msg_t *partition_msg = NULL;
	node_info_msg_t *node_msg = NULL;
	List sinfo_list;
	int node_rec_cnt = 0;

	log_init("sinfo", opts, SYSLOG_FACILITY_DAEMON, NULL);
	parse_command_line(argc, argv);

	while (1) {
		if ( params.iterate && (params.verbose || params.long_output ))
			print_date();

		if (_query_server(&partition_msg, &node_msg) != 0)
			exit(1);

		_filter_nodes(node_msg, &node_rec_cnt);

		sinfo_list = list_create(_sinfo_list_delete);
		_build_sinfo_data(sinfo_list, partition_msg, 
				node_msg, node_rec_cnt);
		_sort_sinfo_data(sinfo_list);
		print_sinfo_list(sinfo_list, params.format_list);

		if (params.iterate) {
			list_destroy(sinfo_list);
			printf("\n");
			sleep(params.iterate);
		} else
			break;
	}

	exit(0);
}

/*
 * _query_server - download the current server state
 * part_pptr IN/OUT - partition information message
 * node_pptr IN/OUT - node information message 
 * RET zero or error code
 */
static int
_query_server(partition_info_msg_t ** part_pptr,
	      node_info_msg_t ** node_pptr)
{
	static partition_info_msg_t *old_part_ptr = NULL, *new_part_ptr;
	static node_info_msg_t *old_node_ptr = NULL, *new_node_ptr;
	int error_code;

	if (old_part_ptr) {
		error_code =
		    slurm_load_partitions(old_part_ptr->last_update,
					  &new_part_ptr);
		if (error_code == SLURM_SUCCESS)
			slurm_free_partition_info_msg(old_part_ptr);
		else if (slurm_get_errno() == SLURM_NO_CHANGE_IN_DATA) {
			error_code = SLURM_SUCCESS;
			new_part_ptr = old_part_ptr;
		}
	} else
		error_code =
		    slurm_load_partitions((time_t) NULL, &new_part_ptr);
	if (error_code) {
		slurm_perror("slurm_load_part");
		return error_code;
	}


	old_part_ptr = new_part_ptr;
	*part_pptr = new_part_ptr;

	if (old_node_ptr) {
		error_code =
		    slurm_load_node(old_node_ptr->last_update,
				    &new_node_ptr);
		if (error_code == SLURM_SUCCESS)
			slurm_free_node_info_msg(old_node_ptr);
		else if (slurm_get_errno() == SLURM_NO_CHANGE_IN_DATA) {
			error_code = SLURM_SUCCESS;
			new_node_ptr = old_node_ptr;
		}
	} else
		error_code = slurm_load_node((time_t) NULL, &new_node_ptr);
	if (error_code) {
		slurm_perror("slurm_load_node");
		return error_code;
	}
	old_node_ptr = new_node_ptr;
	*node_pptr = new_node_ptr;

	return SLURM_SUCCESS;
}

/*
 * _filter_nodes - Filter the node list based upon user options
 * node_msg IN/OUT - node info message with usable entries at the front
 * node_rec_cnt OUT - number of usable node records
 */
static void _filter_nodes(node_info_msg_t *node_msg, int *node_rec_cnt)
{
	int i, new_rec_cnt = 0;
	hostlist_t hosts = NULL;

	if (((params.nodes == NULL) && 
	     (params.partition 	== NULL) && 
	     (!params.state_flag)) ||
	     params.summarize) {
		/* Nothing to filter out */
		*node_rec_cnt = node_msg->record_count;
		return;
	}

	if (params.nodes)
		hosts = hostlist_create(params.nodes);

	for (i = 0; i < node_msg->record_count; i++) {
		if (params.nodes && hostlist_find
		      (hosts, node_msg->node_array[i].name) == -1)
			continue;
		if (params.partition && _strcmp
			 (node_msg->node_array[i].partition,
			  params.partition))
			continue;
		if (params.state_flag && 
		    (node_msg->node_array[i].node_state !=
			    params.state) && 
		    ((node_msg->node_array[i].node_state & 
		      (~NODE_STATE_NO_RESPOND)) != params.state)) 
			continue;
		_swap_node_rec(&node_msg->node_array[i], 
			       &node_msg->node_array[new_rec_cnt]);
		new_rec_cnt++;
	}

	if (hosts)
		hostlist_destroy(hosts);
	*node_rec_cnt = new_rec_cnt;
}

static void _swap_char(char **from, char **to) 
{
	char *tmp;
	tmp   = *to;
	*to   = *from;
 	*from = tmp;
}

/* _swap_node_rec - Swap the data associated with two node records. 
 * trade type *char values, just overwrite the numbers (from_node 
 * data is not used) */
static void _swap_node_rec(node_info_t *from_node, node_info_t *to_node)
{
	if (from_node != to_node) {
		_swap_char(&from_node->name,      &to_node->name);
		_swap_char(&from_node->features,  &to_node->features);
		_swap_char(&from_node->partition, &to_node->partition);
		to_node->node_state	= from_node->node_state;
		to_node->cpus		= from_node->cpus;
		to_node->real_memory	= from_node->real_memory;
		to_node->tmp_disk	= from_node->tmp_disk;
		to_node->weight	= from_node->weight;
	}
}

/*
 * _build_sinfo_data - make a sinfo_data entry for each unique node 
 * configuration and add it to the sinfo_list for later printing.
 * sinfo_list IN/OUT - list of unique sinfo_dataa records to report
 * partition_msg IN - partition info message
 * node_msg IN - node info message
 * node_cnt IN - number of usable records in node_msg (others filtered out)
 * RET zero or error code 
 */
static int _build_sinfo_data(List sinfo_list, 
		partition_info_msg_t *partition_msg, 
		node_info_msg_t *node_msg, int node_cnt)
{
	node_info_t *node_ptr;
	partition_info_t* part_ptr;
	ListIterator i;
	int j;

	/* remove any existing sinfo_list entries */

	/* make sinfo_list entries for each node */
	for (j=0; j<node_cnt; j++) {
		sinfo_data_t *sinfo_ptr;
		i = list_iterator_create(sinfo_list);
		node_ptr = &(node_msg->node_array[j]);
		part_ptr = _find_part(node_ptr->partition, partition_msg);

		/* test if node can be added to existing sinfo_data entry */
		while ((sinfo_ptr = list_next(i))) {
			if (params.match_flags.avail_flag &&
			    (part_ptr->state_up != sinfo_ptr->part_info->state_up))
					continue;
			if (params.match_flags.features_flag &&
			    (_strcmp(node_ptr->features, sinfo_ptr->features)))
					continue;
			
			if (params.match_flags.groups_flag &&
			    (_strcmp(part_ptr->allow_groups, 
			             sinfo_ptr->part_info->allow_groups)))
					continue;
			if (params.match_flags.job_size_flag &&
			    (part_ptr->min_nodes != 
			     sinfo_ptr->part_info->min_nodes))
					continue;
			if (params.match_flags.job_size_flag &&
			    (part_ptr->max_nodes != 
			     sinfo_ptr->part_info->max_nodes))
					continue;
			if (params.match_flags.max_time_flag &&
			    (part_ptr->max_time != sinfo_ptr->part_info->max_time))
					continue;
			if (params.match_flags.partition_flag &&
			    (_strcmp(part_ptr->name, sinfo_ptr->part_info->name)))
					continue;
			if (params.match_flags.root_flag &&
			    (part_ptr->root_only != 
			     sinfo_ptr->part_info->root_only))
					continue;
			if (params.match_flags.share_flag &&
			    (part_ptr->shared != 
			     sinfo_ptr->part_info->shared))
					continue;
			if (params.match_flags.state_flag &&
			    (node_ptr->node_state != sinfo_ptr->node_state))
					continue;

			/* This node has the same configuration as this 
			 * sinfo_data, just add to this record */
			_update_sinfo(sinfo_ptr, part_ptr, node_ptr);
			break;
		}
	
		/* no match, create new sinfo_data entry */
		if (sinfo_ptr == NULL)
			_create_sinfo(sinfo_list, part_ptr, node_ptr);
		list_iterator_destroy(i);
	}

	return SLURM_SUCCESS;
}

static void _update_sinfo(sinfo_data_t *sinfo_ptr, partition_info_t* part_ptr, 
		node_info_t *node_ptr)
{
	if (node_ptr->node_state == NODE_STATE_ALLOCATED)
		sinfo_ptr->nodes_alloc++;
	else if (node_ptr->node_state == NODE_STATE_IDLE)
		sinfo_ptr->nodes_idle++;
	else 
		sinfo_ptr->nodes_other++;
	sinfo_ptr->nodes_tot++;

	if (sinfo_ptr->min_cpus > node_ptr->cpus)
		sinfo_ptr->min_cpus = node_ptr->cpus;
	if (sinfo_ptr->max_cpus < node_ptr->cpus)
		sinfo_ptr->max_cpus = node_ptr->cpus;

	if (sinfo_ptr->min_disk > node_ptr->tmp_disk)
		sinfo_ptr->min_disk = node_ptr->tmp_disk;
	if (sinfo_ptr->max_disk < node_ptr->tmp_disk)
		sinfo_ptr->max_disk = node_ptr->tmp_disk;

	if (sinfo_ptr->min_mem > node_ptr->real_memory)
		sinfo_ptr->min_mem = node_ptr->real_memory;
	if (sinfo_ptr->max_mem < node_ptr->real_memory)
		sinfo_ptr->max_mem = node_ptr->real_memory;

	if (sinfo_ptr->min_weight> node_ptr->weight)
		sinfo_ptr->min_weight = node_ptr->weight;
	if (sinfo_ptr->max_weight < node_ptr->weight)
		sinfo_ptr->max_weight = node_ptr->weight;

	hostlist_push(sinfo_ptr->nodes, node_ptr->name);
}

static void _create_sinfo(List sinfo_list, partition_info_t* part_ptr, 
		node_info_t *node_ptr)
{
	sinfo_data_t *sinfo_ptr;

	/* create an entry */
	sinfo_ptr = xmalloc(sizeof(sinfo_data_t));

	sinfo_ptr->node_state = node_ptr->node_state;
	if (node_ptr->node_state == NODE_STATE_ALLOCATED)
		sinfo_ptr->nodes_alloc++;
	else if (node_ptr->node_state == NODE_STATE_IDLE)
		sinfo_ptr->nodes_idle++;
	else 
		sinfo_ptr->nodes_other++;
	sinfo_ptr->nodes_tot++;

	sinfo_ptr->min_cpus = node_ptr->cpus;
	sinfo_ptr->max_cpus = node_ptr->cpus;

	sinfo_ptr->min_disk = node_ptr->tmp_disk;
	sinfo_ptr->max_disk = node_ptr->tmp_disk;

	sinfo_ptr->min_mem = node_ptr->real_memory;
	sinfo_ptr->max_mem = node_ptr->real_memory;

	sinfo_ptr->min_weight = node_ptr->weight;
	sinfo_ptr->max_weight = node_ptr->weight;

	sinfo_ptr->features = node_ptr->features;
	sinfo_ptr->part_info = part_ptr;

	sinfo_ptr->nodes = hostlist_create(node_ptr->name);

	list_append(sinfo_list, sinfo_ptr);
}

/* Return a pointer to the given partition name or NULL on error */
static partition_info_t *_find_part(char *part_name, 
			partition_info_msg_t *partition_msg) 
{
	int i;
	for (i=0; i<partition_msg->record_count; i++) {
		if (_strcmp(part_name, partition_msg->partition_array[i].name))
			continue;
		return &(partition_msg->partition_array[i]);
	}
	return NULL;
}

static void _sort_sinfo_data(List sinfo_list)
{
	if (params.node_field_flag) 	/* already in node order */
		return;

	/* sort list in partition order */
	list_sort(sinfo_list, _part_order);
}
static int _part_order (void *data1, void *data2)
{
	sinfo_data_t *sinfo_ptr1 = data1;
	sinfo_data_t *sinfo_ptr2 = data2;

	return _strcmp(sinfo_ptr1->part_info->name,
	               sinfo_ptr2->part_info->name);
}

static void _sinfo_list_delete(void *data)
{
	sinfo_data_t *sinfo_ptr = data;

	hostlist_destroy(sinfo_ptr->nodes);
	xfree(sinfo_ptr);
}

/* like strcmp, but works with NULL pointers */
static int _strcmp(char *data1, char *data2) 
{
	if (data1 == NULL)
		data1 = "";
	if (data2 == NULL)
		data2 = "";
	return strcmp(data1, data2);
}

