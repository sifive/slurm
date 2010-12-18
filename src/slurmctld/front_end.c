/*****************************************************************************\
 *  front_end.c - Define front end node functions.
 *****************************************************************************
 *  Copyright (C) 2010 Lawrence Livermore National Security.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Morris Jette <jette1@llnl.gov>
 *  CODE-OCEC-09-009. All rights reserved.
 *
 *  This file is part of SLURM, a resource management program.
 *  For details, see <https://computing.llnl.gov/linux/slurm/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <slurm/slurm.h>
#include <src/common/list.h>
#include <src/common/log.h>
#include <src/common/node_conf.h>
#include <src/common/read_config.h>
#include <src/common/slurm_protocol_defs.h>
#include <src/common/xstring.h>
#include <src/slurmctld/locks.h>
#include <src/slurmctld/slurmctld.h>
#include <src/slurmctld/state_save.h>
#include <src/slurmctld/trigger_mgr.h>

/* Change FRONT_END_STATE_VERSION value when changing the state save format */
#define FRONT_END_STATE_VERSION      "VER001"
#define FRONT_END_2_2_STATE_VERSION  "VER001"	/* SLURM version 2.2 */

#ifdef HAVE_FRONT_END
front_end_record_t *front_end_nodes = NULL;
uint16_t front_end_node_cnt = 0;
time_t last_front_end_update = (time_t) 0;

/*
 * _dump_front_end_state - dump state of a specific front_end node to a buffer
 * IN front_end_ptr - pointer to node for which information is requested
 * IN/OUT buffer - location to store data, pointers automatically advanced
 */
static void _dump_front_end_state(front_end_record_t *front_end_ptr,
				  Buf buffer)
{
	packstr  (front_end_ptr->name, buffer);
	pack16   (front_end_ptr->node_state, buffer);
	packstr  (front_end_ptr->reason, buffer);
	pack_time(front_end_ptr->reason_time, buffer);
	pack32   (front_end_ptr->reason_uid, buffer);
}


/*
 * Open the front_end node state save file, or backup if necessary.
 * state_file IN - the name of the state save file used
 * RET the file description to read from or error code
 */
static int _open_front_end_state_file(char **state_file)
{
	int state_fd;
	struct stat stat_buf;

	*state_file = xstrdup(slurmctld_conf.state_save_location);
	xstrcat(*state_file, "/front_end_state");
	state_fd = open(*state_file, O_RDONLY);
	if (state_fd < 0) {
		error("Could not open front_end state file %s: %m",
		      *state_file);
	} else if (fstat(state_fd, &stat_buf) < 0) {
		error("Could not stat front_end state file %s: %m",
		      *state_file);
		(void) close(state_fd);
	} else if (stat_buf.st_size < 10) {
		error("Front_end state file %s too small", *state_file);
		(void) close(state_fd);
	} else 	/* Success */
		return state_fd;

	error("NOTE: Trying backup front_endstate save file. Information may "
	      "be lost!");
	xstrcat(*state_file, ".old");
	state_fd = open(*state_file, O_RDONLY);
	return state_fd;
}

/*
 * _pack_front_end - dump all configuration information about a specific
 *	front_end node in machine independent form (for network transmission)
 * IN dump_front_end_ptr - pointer to front_end node for which information is
 *	requested
 * IN/OUT buffer - buffer where data is placed, pointers automatically updated
 * IN protocol_version - slurm protocol version of client
 * NOTE: if you make any changes here be sure to make the corresponding
 *	changes to load_front_end_config in api/node_info.c
 */
static void _pack_front_end(struct front_end_record *dump_front_end_ptr,
			    Buf buffer, uint16_t protocol_version)
{
	if (protocol_version >= SLURM_2_2_PROTOCOL_VERSION) {
		pack_time(dump_front_end_ptr->boot_time, buffer);
		packstr(dump_front_end_ptr->name, buffer);
		pack16(dump_front_end_ptr->node_state, buffer);

		packstr(dump_front_end_ptr->reason, buffer);
		pack_time(dump_front_end_ptr->reason_time, buffer);
		pack32(dump_front_end_ptr->reason_uid, buffer);

		pack_time(dump_front_end_ptr->slurmd_start_time, buffer);
	} else {
		error("_pack_front_end: Unsupported slurm version %u",
		      protocol_version);
	}
}
#endif

/*
 * Update front end node state
 * update_front_end_msg_ptr IN change specification
 * RET SLURM_SUCCESS or error code
 */
extern int update_front_end(update_front_end_msg_t *msg_ptr)
{
#ifdef HAVE_FRONT_END
	char  *this_node_name = NULL;
	hostlist_t host_list;
	front_end_record_t *front_end_ptr;
	int i, rc = SLURM_SUCCESS;
	time_t now = time(NULL);

	if ((host_list = hostlist_create(msg_ptr->name)) == NULL) {
		error("hostlist_create error on %s: %m", msg_ptr->name);
		return ESLURM_INVALID_NODE_NAME;
	}

	last_front_end_update = now;
	while ((this_node_name = hostlist_shift(host_list))) {
		for (i = 0, front_end_ptr = front_end_nodes;
		     i < front_end_node_cnt; i++, front_end_ptr++) {
			xassert(front_end_ptr->magic == FRONT_END_MAGIC);
			if (strcmp(this_node_name, front_end_ptr->name))
				continue;
			if (msg_ptr->node_state == (uint16_t) NO_VAL)
				;	/* No change in node state */
			else if (msg_ptr->node_state == NODE_RESUME)
				front_end_ptr->node_state = NODE_STATE_IDLE;
			else if (msg_ptr->node_state == NODE_STATE_DRAIN)
				front_end_ptr->node_state |= NODE_STATE_DRAIN;
			else if (msg_ptr->node_state == NODE_STATE_DOWN) {
				front_end_ptr->node_state &= NODE_STATE_FLAGS;
				front_end_ptr->node_state |= NODE_STATE_DOWN;
			}
			if (IS_NODE_DRAIN(front_end_ptr) ||
			    IS_NODE_DOWN(front_end_ptr)) {
				if (msg_ptr->reason) {
					xfree(front_end_ptr->reason);
					front_end_ptr->reason =
						xstrdup(msg_ptr->reason);
					front_end_ptr->reason_time = now;
					front_end_ptr->reason_uid =
						msg_ptr->reason_uid;
				}
			} else if (front_end_ptr->reason) {
				/* Should not be any reason set */
				xfree(front_end_ptr->reason);
				front_end_ptr->reason_time = 0;
				front_end_ptr->reason_uid = 0;
			}
			break;
		}
		if (i >= front_end_node_cnt) {
			info("update_front_end: could not find front end: %s",
			     this_node_name);
			rc = ESLURM_INVALID_NODE_NAME;
		}
		free(this_node_name);
	}
	hostlist_destroy(host_list);

	return rc;
#endif
}

/*
 * find_front_end_record - find a record for front_endnode with specified name
 * input: name - name of the desired front_end node
 * output: return pointer to front_end node record or NULL if not found
 */
extern front_end_record_t *find_front_end_record(char *name)
{
#ifdef HAVE_FRONT_END
	front_end_record_t *front_end_ptr;
	int i;

	for (i = 0, front_end_ptr = front_end_nodes;
	     i < front_end_node_cnt; i++, front_end_ptr++) {
		xassert(front_end_ptr->magic == FRONT_END_MAGIC);
		if (strcmp(front_end_ptr->name, name) == 0)
			return front_end_ptr;
	}
#endif
	return NULL;
}

/*
 * log_front_end_state - log all front end node state
 */
extern void log_front_end_state(void)
{
#ifdef HAVE_FRONT_END
	front_end_record_t *front_end_ptr;
	int i;

	for (i = 0, front_end_ptr = front_end_nodes;
	     i < front_end_node_cnt; i++, front_end_ptr++) {
		xassert(front_end_ptr->magic == FRONT_END_MAGIC);
		info("FrontendName=%s FrontendAddr=%s Port=%u State=%s "
		     "Reason=%s",
		     front_end_ptr->name, front_end_ptr->comm_name,
		     front_end_ptr->port,
		     node_state_string(front_end_ptr->node_state),
		     front_end_ptr->reason);
	}
#endif
}

/*
 * purge_front_end_state - purge all front end node state
 */
extern void purge_front_end_state(void)
{
#ifdef HAVE_FRONT_END
	front_end_record_t *front_end_ptr;
	int i;

	for (i = 0, front_end_ptr = front_end_nodes;
	     i < front_end_node_cnt; i++, front_end_ptr++) {
		xassert(front_end_ptr->magic == FRONT_END_MAGIC);
		xfree(front_end_ptr->comm_name);
		xfree(front_end_ptr->name);
		xfree(front_end_ptr->reason);
	}
	xfree(front_end_nodes);
	front_end_node_cnt = 0;
#endif
}

/*
 * restore_front_end_state - restore frontend node state
 * IN recover - replace job, node and/or partition data with latest
 *              available information depending upon value
 *              0 = use no saved state information, rebuild everything from
 *		    slurm.conf contents
 *              1 = recover saved job and trigger state,
 *                  node DOWN/DRAIN/FAIL state and reason information
 *              2 = recover all saved state
 */
extern void restore_front_end_state(int recover)
{
#ifdef HAVE_FRONT_END
	slurm_conf_frontend_t *slurm_conf_fe_ptr;
	ListIterator iter;
	uint16_t state_base, state_flags;
	int i;

	if (recover == 2)
		return;
	last_front_end_update = time(NULL);
	if (recover == 0)
		purge_front_end_state();
	if (front_end_list == NULL)
		return;		/* No front ends in slurm.conf */

	iter = list_iterator_create(front_end_list);
	if (iter == NULL)
		fatal("list_iterator_create: malloc failure");
	while ((slurm_conf_fe_ptr = (slurm_conf_frontend_t *)
				    list_next(iter))) {
		if (slurm_conf_fe_ptr->frontends == NULL)
			fatal("FrontendName is NULL");
		for (i = 0; i < front_end_node_cnt; i++) {
			if (strcmp(front_end_nodes[i].name,
				   slurm_conf_fe_ptr->frontends) == 0)
				break;
		}
		if (i >= front_end_node_cnt) {
			front_end_node_cnt++;
			xrealloc(front_end_nodes,
				 sizeof(front_end_record_t) *
				 front_end_node_cnt);
			front_end_nodes[i].name =
				xstrdup(slurm_conf_fe_ptr->frontends);
			front_end_nodes[i].magic = FRONT_END_MAGIC;
		}
		xfree(front_end_nodes[i].comm_name);
		if (slurm_conf_fe_ptr->addresses) {
			front_end_nodes[i].comm_name =
				xstrdup(slurm_conf_fe_ptr->addresses);
		} else {
			front_end_nodes[i].comm_name =
				xstrdup(front_end_nodes[i].name);
		}
		state_base  = front_end_nodes[i].node_state & NODE_STATE_BASE;
		state_flags = front_end_nodes[i].node_state & NODE_STATE_FLAGS;
		if ((state_base == 0) || (state_base == NODE_STATE_UNKNOWN)) {
			front_end_nodes[i].node_state =
				slurm_conf_fe_ptr->node_state | state_flags;
		}
		if ((front_end_nodes[i].reason == NULL) &&
		    (slurm_conf_fe_ptr->reason != NULL)) {
			front_end_nodes[i].reason =
				xstrdup(slurm_conf_fe_ptr->reason);
		}
		if (slurm_conf_fe_ptr->port)
			front_end_nodes[i].port = slurm_conf_fe_ptr->port;
		else
			front_end_nodes[i].port = slurmctld_conf.slurmd_port;
		slurm_set_addr(&front_end_nodes[i].slurm_addr,
			       front_end_nodes[i].port,
			       front_end_nodes[i].comm_name);
	}
	list_iterator_destroy(iter);
	if (front_end_node_cnt == 0)
		fatal("No front end nodes defined");
	if (slurm_get_debug_flags() & DEBUG_FLAG_FRONT_END)
		log_front_end_state();
#endif
}

/*
 * pack_all_front_end - dump all front_end node information for all nodes
 *	in machine independent form (for network transmission)
 * OUT buffer_ptr - pointer to the stored data
 * OUT buffer_size - set to size of the buffer in bytes
 * IN protocol_version - slurm protocol version of client
 * NOTE: the caller must xfree the buffer at *buffer_ptr
 * NOTE: READ lock_slurmctld config before entry
 */
extern void pack_all_front_end(char **buffer_ptr, int *buffer_size, uid_t uid,
			       uint16_t protocol_version)
{
#ifdef HAVE_FRONT_END
	uint32_t nodes_packed, tmp_offset;
	front_end_record_t *front_end_ptr;
	Buf buffer;
	int i;
	time_t now = time(NULL);

	buffer_ptr[0] = NULL;
	*buffer_size = 0;

	buffer = init_buf(BUF_SIZE * 2);
	nodes_packed = 0;

	if (protocol_version >= SLURM_2_2_PROTOCOL_VERSION) {
		/* write header: count and time */
		pack32(nodes_packed, buffer);
		pack_time(now, buffer);

		/* write records */
		for (i = 0, front_end_ptr = front_end_nodes;
		     i < front_end_node_cnt; i++, front_end_ptr++) {
			xassert(front_end_ptr->magic == FRONT_END_MAGIC);
			_pack_front_end(front_end_ptr, buffer,
					protocol_version);
			nodes_packed++;
		}
	} else {
		error("pack_all_front_end: Unsupported slurm version %u",
		      protocol_version);
	}

	tmp_offset = get_buf_offset (buffer);
	set_buf_offset(buffer, 0);
	pack32(nodes_packed, buffer);
	set_buf_offset(buffer, tmp_offset);

	*buffer_size = get_buf_offset(buffer);
	buffer_ptr[0] = xfer_buf_data(buffer);
#endif
}

/* dump_all_front_end_state - save the state of all front_end nodes to file */
extern int dump_all_front_end_state(void)
{
#ifdef HAVE_FRONT_END
	/* Save high-water mark to avoid buffer growth with copies */
	static int high_buffer_size = (1024 * 1024);
	int error_code = 0, i, log_fd;
	char *old_file, *new_file, *reg_file;
	front_end_record_t *front_end_ptr;
	/* Locks: Read config and node */
	slurmctld_lock_t node_read_lock = { READ_LOCK, NO_LOCK, READ_LOCK,
					    NO_LOCK };
	Buf buffer = init_buf(high_buffer_size);
	DEF_TIMERS;

	START_TIMER;
	/* write header: version, time */
	packstr(FRONT_END_STATE_VERSION, buffer);
	pack_time(time(NULL), buffer);

	/* write node records to buffer */
	lock_slurmctld (node_read_lock);

	for (i = 0, front_end_ptr = front_end_nodes;
	     i < front_end_node_cnt; i++, front_end_ptr++) {
		xassert(front_end_ptr->magic == FRONT_END_MAGIC);
		_dump_front_end_state(front_end_ptr, buffer);
	}

	old_file = xstrdup (slurmctld_conf.state_save_location);
	xstrcat (old_file, "/front_end_state.old");
	reg_file = xstrdup (slurmctld_conf.state_save_location);
	xstrcat (reg_file, "/front_end_state");
	new_file = xstrdup (slurmctld_conf.state_save_location);
	xstrcat (new_file, "/front_end_state.new");
	unlock_slurmctld (node_read_lock);

	/* write the buffer to file */
	lock_state_files();
	log_fd = creat (new_file, 0600);
	if (log_fd < 0) {
		error ("Can't save state, error creating file %s %m", new_file);
		error_code = errno;
	} else {
		int pos = 0, nwrite = get_buf_offset(buffer), amount, rc;
		char *data = (char *)get_buf_data(buffer);
		high_buffer_size = MAX(nwrite, high_buffer_size);
		while (nwrite > 0) {
			amount = write(log_fd, &data[pos], nwrite);
			if ((amount < 0) && (errno != EINTR)) {
				error("Error writing file %s, %m", new_file);
				error_code = errno;
				break;
			}
			nwrite -= amount;
			pos    += amount;
		}

		rc = fsync_and_close(log_fd, "front_end");
		if (rc && !error_code)
			error_code = rc;
	}
	if (error_code)
		(void) unlink (new_file);
	else {	/* file shuffle */
		(void) unlink (old_file);
		if (link(reg_file, old_file))
			debug4("unable to create link for %s -> %s: %m",
			       reg_file, old_file);
		(void) unlink (reg_file);
		if (link(new_file, reg_file))
			debug4("unable to create link for %s -> %s: %m",
			       new_file, reg_file);
		(void) unlink (new_file);
	}
	xfree (old_file);
	xfree (reg_file);
	xfree (new_file);
	unlock_state_files ();

	free_buf (buffer);
	END_TIMER2("dump_all_front_end_state");
	return error_code;
#endif
}

/*
 * load_all_front_end_state - Load the front_end node state from file, recover
 *	on slurmctld restart. Execute this after loading the configuration
 *	file data. Data goes into common storage.
 * IN state_only - if true, overwrite only front_end node state and reason
 *	Use this to overwrite the "UNKNOWN state typically used in slurm.conf
 * RET 0 or error code
 * NOTE: READ lock_slurmctld config before entry
 */
extern int load_all_front_end_state(bool state_only)
{
#ifdef HAVE_FRONT_END
	char *node_name = NULL, *reason = NULL, *data = NULL, *state_file;
	int data_allocated, data_read = 0, error_code = 0, node_cnt = 0;
	uint16_t node_state;
	uint32_t data_size = 0, name_len;
	uint32_t reason_uid = NO_VAL;
	time_t reason_time = 0;
	front_end_record_t *front_end_ptr;
	int state_fd;
	time_t time_stamp;
	Buf buffer;
	char *ver_str = NULL;
	uint16_t protocol_version = (uint16_t) NO_VAL;

	/* read the file */
	lock_state_files ();
	state_fd = _open_front_end_state_file(&state_file);
	if (state_fd < 0) {
		info ("No node state file (%s) to recover", state_file);
		error_code = ENOENT;
	} else {
		data_allocated = BUF_SIZE;
		data = xmalloc(data_allocated);
		while (1) {
			data_read = read(state_fd, &data[data_size], BUF_SIZE);
			if (data_read < 0) {
				if (errno == EINTR)
					continue;
				else {
					error ("Read error on %s: %m",
						state_file);
					break;
				}
			} else if (data_read == 0)     /* eof */
				break;
			data_size      += data_read;
			data_allocated += data_read;
			xrealloc(data, data_allocated);
		}
		close (state_fd);
	}
	xfree (state_file);
	unlock_state_files ();

	buffer = create_buf (data, data_size);

	safe_unpackstr_xmalloc( &ver_str, &name_len, buffer);
	debug3("Version string in front_end_state header is %s", ver_str);
	if (ver_str) {
		if (!strcmp(ver_str, FRONT_END_STATE_VERSION)) {
			protocol_version = SLURM_PROTOCOL_VERSION;
		}
	}

	if (protocol_version == (uint16_t) NO_VAL) {
		error("*****************************************************");
		error("Can not recover front_end state, version incompatible");
		error("*****************************************************");
		xfree(ver_str);
		free_buf(buffer);
		return EFAULT;
	}
	xfree(ver_str);

	safe_unpack_time(&time_stamp, buffer);

	while (remaining_buf (buffer) > 0) {
		uint16_t base_state;
		if (protocol_version >= SLURM_2_2_PROTOCOL_VERSION) {
			safe_unpackstr_xmalloc (&node_name, &name_len, buffer);
			safe_unpack16 (&node_state,  buffer);
			safe_unpackstr_xmalloc (&reason,    &name_len, buffer);
			safe_unpack_time (&reason_time, buffer);
			safe_unpack32 (&reason_uid,  buffer);
			base_state = node_state & NODE_STATE_BASE;
		} else
			goto unpack_error;

		/* validity test as possible */

		/* find record and perform update */
		front_end_ptr = find_front_end_record(node_name);
		if (front_end_ptr == NULL) {
			error("Front_end node %s has vanished from "
			      "configuration", node_name);
		} else if (state_only) {
			uint16_t orig_flags;
			orig_flags = front_end_ptr->node_state &
				     NODE_STATE_FLAGS;
			node_cnt++;
			if (IS_NODE_UNKNOWN(front_end_ptr)) {
				if (base_state == NODE_STATE_DOWN) {
					front_end_ptr->node_state =
						NODE_STATE_DOWN | orig_flags;
				}
				if (node_state & NODE_STATE_DRAIN) {
					 front_end_ptr->node_state |=
						 NODE_STATE_DRAIN;
				}
				if (node_state & NODE_STATE_FAIL) {
					front_end_ptr->node_state |=
						NODE_STATE_FAIL;
				}
			}
			if (front_end_ptr->reason == NULL) {
				front_end_ptr->reason = reason;
				reason = NULL;	/* Nothing to free */
				front_end_ptr->reason_time = reason_time;
				front_end_ptr->reason_uid = reason_uid;
			}
		} else {
			node_cnt++;
			front_end_ptr->node_state = node_state;
			xfree(front_end_ptr->reason);
			front_end_ptr->reason	= reason;
			reason			= NULL;	/* Nothing to free */
			front_end_ptr->reason_time	= reason_time;
			front_end_ptr->reason_uid	= reason_uid;
			front_end_ptr->last_response	= (time_t) 0;
		}

		xfree(node_name);
		xfree(reason);
	}

fini:	info("Recovered state of %d front_end nodes", node_cnt);
	free_buf (buffer);
	return error_code;

unpack_error:
	error("Incomplete front_end node data checkpoint file");
	error_code = EFAULT;
	xfree (node_name);
	xfree(reason);
	goto fini;
#else
	return 0;
#endif
}

/*
 * set_front_end_down - make the specified front end node's state DOWN and
 *	kill jobs as needed
 * IN front_end_pt - pointer to the front end node
 * IN reason - why the node is DOWN
 */
extern void set_front_end_down (front_end_record_t *front_end_ptr,
				char *reason)
{
	time_t now = time(NULL);
	uint16_t state_flags = front_end_ptr->node_state & NODE_STATE_FLAGS;

	front_end_ptr->node_state = NODE_STATE_DOWN | state_flags;
	trigger_front_end_down(front_end_ptr);
	if ((front_end_ptr->reason == NULL) ||
	    (strncmp(front_end_ptr->reason, "Not responding", 14) == 0)) {
		xfree(front_end_ptr->reason);
		front_end_ptr->reason = xstrdup(reason);
		front_end_ptr->reason_time = now;
		front_end_ptr->reason_uid = slurm_get_slurm_user_id();
	}

	return;
}
