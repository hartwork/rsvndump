/*
 *      rsvndump - remote svn repository dump
 *      Copyright (C) 2008-2009 Jonas Gehring
 *
 *      This program is free software: you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation, either version 3 of the License, or
 *      (at your option) any later version.
 *
 *      This program is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *      GNU General Public License for more details.
 *
 *      You should have received a copy of the GNU General Public License
 *      along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *
 *      file: dump.c
 *      desc: Main working place
 */


#include <stdio.h>
#include <sys/stat.h>

#include <svn_pools.h>
#include <svn_props.h>
#include <svn_ra.h>
#include <svn_repos.h>

#include <apr_hash.h>
#include <apr_pools.h>

#include "main.h"
#include "delta.h"
#include "list.h"
#include "log.h"
#include "property.h"
#include "utils.h"

#include "dump.h"


/*---------------------------------------------------------------------------*/
/* Static functions                                                          */
/*---------------------------------------------------------------------------*/


/* Dumps a revision header using the given properties */
static void dump_revision_header(log_revision_t *revision, svn_revnum_t local_revnum, dump_options_t *opts)
{
	int props_length = 0;

	/* Determine length of revision properties */
	props_length += property_strlen("svn:log", revision->message);
	props_length += property_strlen("svn:author", revision->author);
	props_length += property_strlen("svn:date", revision->date);
	if (props_length > 0) {
		props_length += PROPS_END_LEN;
	}

	printf("%s: %ld\n", SVN_REPOS_DUMPFILE_REVISION_NUMBER, local_revnum);	
	printf("%s: %d\n", SVN_REPOS_DUMPFILE_PROP_CONTENT_LENGTH, props_length);
	printf("%s: %d\n\n", SVN_REPOS_DUMPFILE_CONTENT_LENGTH, props_length);
	
	if (props_length > 0) {
		property_dump("svn:log", revision->message);
		property_dump("svn:author", revision->author);
		property_dump("svn:date", revision->date);

		printf(PROPS_END"\n");
	}
}


/* Runs a diff against two revisions */
static char dump_do_diff(session_t *session, svn_revnum_t src, svn_revnum_t dest, const svn_delta_editor_t *editor, void *editor_baton, apr_pool_t *pool)
{
	const svn_ra_reporter2_t *reporter;
	void *report_baton;
	svn_error_t *err;
	apr_pool_t *subpool = svn_pool_create(pool);
#ifdef USE_TIMING
	stopwatch_t watch = stopwatch_create();
#endif

	DEBUG_MSG("diffing %d against %d\n", dest, src);
	err = svn_ra_do_diff2(session->ra, &reporter, &report_baton, dest, (session->file ? session->file : ""), TRUE, TRUE, TRUE, session->encoded_url, editor, editor_baton, subpool);
	if (err) {
		svn_handle_error2(err, stderr, FALSE, APPNAME": ");
		svn_error_clear(err);
		svn_pool_destroy(subpool);
		return 1;
	}

	err = reporter->set_path(report_baton, "", src, (src == dest), NULL, subpool);
	if (err) {
		svn_handle_error2(err, stderr, FALSE, APPNAME": ");
		svn_error_clear(err);
		svn_pool_destroy(subpool);
		return 1;
	}

	err = reporter->finish_report(report_baton, subpool);
	if (err) {
		svn_handle_error2(err, stderr, FALSE, APPNAME": ");
		svn_error_clear(err);
		svn_pool_destroy(subpool);
		return 1;
	}

	svn_pool_destroy(subpool);
#ifdef USE_TIMING
	DEBUG_MSG("dump_do_diff done in %f seconds\n", stopwatch_elapsed(&watch));
#endif
	return 0;
}


/* Determines the HEAD revision of a repository */
static char dump_determine_head(session_t *session, svn_revnum_t *rev)
{
	svn_error_t *err;
	svn_dirent_t *dirent;
	apr_pool_t *pool = svn_pool_create(session->pool);

	err = svn_ra_stat(session->ra, "",  -1, &dirent, session->pool);
	if (err) {
		svn_handle_error2(err, stderr, FALSE, APPNAME": ");
		svn_error_clear(err);
		svn_pool_destroy(pool);
		return 1;
	}
	if (dirent == NULL) {
		fprintf(stderr, _("ERROR: URL '%s' not found in HEAD revision\n"), session->url);
		svn_pool_destroy(pool);
		return 1;
	}

	*rev = dirent->created_rev;
	svn_pool_destroy(pool);
	return 0;
}


/* Checks if a path is present in a given revision */
static svn_node_kind_t dump_check_path(session_t *session, const char *path, svn_revnum_t rev)
{
	svn_error_t *err;
	svn_node_kind_t kind;
	apr_pool_t *pool = svn_pool_create(session->pool);

	err = svn_ra_check_path(session->ra, path, rev, &kind, pool);
	if (err) {
		svn_handle_error2(err, stderr, FALSE, APPNAME": ");
		svn_error_clear(err);
		svn_pool_destroy(pool);
		return svn_node_none;
	}

	svn_pool_destroy(pool);
	return kind;
}


/* Fetches the UUID of a repository */
static char dump_fetch_uuid(session_t *session, const char **uuid)
{
	svn_error_t *err;
	apr_pool_t *pool = svn_pool_create(session->pool);
#if (SVN_VER_MAJOR == 1) && (SVN_VER_MINOR >= 6)
	err = svn_ra_get_uuid2(session->ra, uuid, pool);
#else
	err = svn_ra_get_uuid(session->ra, uuid, pool);
#endif
	if (err) {
		svn_handle_error2(err, stderr, FALSE, APPNAME": ");
		svn_error_clear(err);
		svn_pool_destroy(pool);
		return 1;
	}
	svn_pool_destroy(pool);
	return 0;
}


/*---------------------------------------------------------------------------*/
/* Global functions                                                          */
/*---------------------------------------------------------------------------*/


/* Creates and intializes a new dump_options_t object */
dump_options_t dump_options_create()
{
	dump_options_t opts;

	opts.temp_dir = NULL;
	opts.prefix = NULL;
	opts.verbosity = 0;
	opts.flags = 0x00;

	opts.start = 0;
	opts.end = -1; /* HEAD */

	return opts;
}


/* Frees a dump_options_t object */
void dump_options_free(dump_options_t *opts)
{
	if (opts->temp_dir != NULL) {
		free(opts->temp_dir);
	}
	if (opts->prefix != NULL) {
		free(opts->prefix);
	}
}


/* Start the dumping process, using the given session and options */
char dump(session_t *session, dump_options_t *opts)
{
	list_t logs;
	char logs_fetched = 0;
	svn_revnum_t global_rev, local_rev;
	int list_idx;

	/* First, determine or check the start and end revision */
	if (opts->end == -1) {
		if (dump_determine_head(session, &opts->end)) {
			return 1;
		}
		if (opts->start == 0) {
			if (log_get_range(session, &opts->start, &opts->end, opts->verbosity)) {
				return 1;
			}
		} else {
			/* Check if path is present in given start revision */
			if (dump_check_path(session, "", opts->start) == svn_node_none) {
				fprintf(stderr, _("ERROR: URL '%s' not found in revision %ld\n"), session->url, opts->start);
				return 1;
			}
		}
	} else {
		/* Check if path is present in given start revision */
		if (dump_check_path(session, "", opts->start) == svn_node_none) {
			fprintf(stderr, _("ERROR: URL '%s' not found in revision %ld\n"), session->url, opts->start);
			return 1;
		}
		/* Check if path is present in given end revision */
		if (dump_check_path(session, "", opts->end) == svn_node_none) {
			fprintf(stderr, _("ERROR: URL '%s' not found in revision %ld\n"), session->url, opts->end);
			return 1;
		}
	}

	/*
	 * Check if we need to reparent the RA session. This is needed if we
	 * are only dumping the history of a single file. Else, svn_ra_do_diff()
	 * will nor work.
	 */
	if (session_check_reparent(session, opts->start)) {
		return 1;
	}

	/*
	 * Decide wether the whole repositry log should be fetched
	 * prior to dumping. This is needed if the dump is incremental and
	 * the start revision is not 0.
	 */
	if ((opts->flags & DF_INCREMENTAL) && (opts->start != 0)) {
		if (log_fetch_all(session, opts->start, opts->end, &logs, opts->verbosity)) {
			return 1;
		}
		logs_fetched = 1;
	} else {
		logs = list_create(sizeof(log_revision_t));
	}

	/* Determine end revision if neccessary */
	if (logs_fetched) {
		opts->end = ((log_revision_t *)logs.elements)[logs.size-1].revision;
	}
	else if (opts->end == -1) {
	}

	/* Determine start revision if neccessary */
	if (opts->start == 0) {
		if (strlen(session->prefix) > 0) {
			/* There arent' any subdirectories at revision 0 */
			opts->start = 1;
		}
	}

	/* Write dumpfile header */
	printf("%s: ", SVN_REPOS_DUMPFILE_MAGIC_HEADER); 
	if (opts->flags & DF_USE_DELTAS) {
		printf("3\n\n");
	} else {
		printf("2\n\n");
	}
	if (opts->flags & DF_DUMP_UUID) {
		const char *uuid;
		if (dump_fetch_uuid(session, &uuid)) {
			list_free(&logs);
			return 1;
		}
		printf("UUID: %s\n\n", uuid);
	}

	/* Pre-dumping initialization */
	global_rev = opts->start;
	local_rev = global_rev == 0 ? 0 : 1;
	list_idx = 0;

	/* Start dumping */
	do {
		svn_delta_editor_t *editor;
		void *editor_baton;
		svn_revnum_t diff_rev;
		apr_pool_t *revpool = svn_pool_create(session->pool);

		if (logs_fetched == 0) {
			log_revision_t log;
			if (log_fetch(session, global_rev, opts->end, &log, revpool)) {
				list_free(&logs);
				return 1;

			}
			list_append(&logs, &log);
			list_idx = logs.size-1;
		} else {
			++list_idx;
		}

		/* Dump the revision header */
		dump_revision_header((log_revision_t *)logs.elements + list_idx, local_rev, opts);

		/* Determine the diff base */
		diff_rev = global_rev - 1;
		if (diff_rev < 0) {
			diff_rev = 0;
		}
		if ((strlen(session->prefix) > 0) && diff_rev < opts->start) {
			/* TODO: This isn't working well with single files
			 * and a revision range */
			if (session->file) {
				diff_rev = opts->end;
			} else {
				diff_rev = opts->start;
			}
		}
		DEBUG_MSG("global = %ld, diff = %ld\n", global_rev, diff_rev);

		/* Setup the delta editor and run a diff */
		delta_setup_editor(session, opts, &logs, (log_revision_t *)logs.elements + list_idx, local_rev, &editor, &editor_baton, revpool);
		if (dump_do_diff(session, diff_rev, ((log_revision_t *)logs.elements)[list_idx].revision, editor, editor_baton, revpool)) {
			list_free(&logs);
			return 1;
		}

		if (opts->verbosity >= 0) {
			fprintf(stderr, _("* Dumped revision %ld.\n"), ((log_revision_t *)logs.elements)[list_idx].revision);
		}

		global_rev = ((log_revision_t *)logs.elements)[list_idx].revision+1;
		++local_rev;
		apr_pool_destroy(revpool);
	} while (global_rev <= opts->end);

	return 0;
}
