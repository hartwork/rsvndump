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
 *      file: main.c
 *      desc: Program initialization and option parsing
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <svn_cmdline.h>
#include <svn_path.h>
#include <svn_pools.h>
#include <svn_opt.h>

#include "main.h"
#include "dump.h"
#include "utils.h"


/*---------------------------------------------------------------------------*/
/* Static functions                                                          */
/*---------------------------------------------------------------------------*/

/* Prints the program version */
static void print_version()
{
	printf(PACKAGE" "PACKAGE_VERSION"\n");
	printf("Copyright (C) 2008-2009 "PACKAGE_AUTHOR"\n");
	printf("Released under the GNU General Public License.\n");
}


/* Prints usage information */
static void print_usage()
{
	print_version();
	printf("\n");
	printf(_("USAGE:"));
	printf(" "PACKAGE" ");
	printf(_("[options] <url>\n\n"));
	printf(_("Valid options:\n"));
	printf(_("    -h [--help]               print a nice help screen\n"));
	printf(_("    --version                 print the program name and version\n"));
	printf(_("    -q [--quiet]              be quiet\n"));
	printf(_("    -v [--verbose]            print extra progress\n"));
	printf(_("    -u [--username] arg       username\n"));
	printf(_("    -p [--password] arg       password\n"));
	printf(_("    -r [--revision] arg       specify revision number (or X:Y range)\n"));
	printf(_("    --deltas                  use deltas in dump output\n"));
	printf(_("    --incremental             dump incrementally\n"));
	printf(_("    --no-auth-cache           do not cache authentication tokens\n"));
	printf(_("    --non-interactive         do no interactive prompting\n"));
	printf(_("    --prefix arg              prepend arg to the path that is being dumped\n"));
	printf(_("    --keep-revnums            keep the dumped revision numbers in sync with\n" \
	         "                              the repository by using empty revisions for\n" \
	         "                              padding\n"));
	printf("\n");
	printf("Report bugs to <"PACKAGE_BUGREPORT">\n");
}


/* Parses a revision number (or range) */
static char parse_revnum(char *str, svn_revnum_t *start, svn_revnum_t *end)
{
	char *split, *end_ptr;
	errno = 0;
	if ((split = strchr(str, ':')) == NULL) {
		/* Single number */
		if (strncmp("HEAD", str, 4) == 0) {
			*start = -1;
			*end = -1;
			return 0;
		}
		*start = strtol(str, &end_ptr, 10);
		if (*end_ptr != 0) {
			return 1;
		}
		*end = *start;
	} else {
		/* Range */
		if ((str == split) || (*(++split) == '\0')) {
			return 1;
		}

		if (strncmp("HEAD", str, 4) == 0) {
			*start = -1;
		} else {
			*start = strtol(str, &end_ptr, 10);
			if ((errno == EINVAL) || (errno == ERANGE) || (end_ptr != split-1)) {
				return 1;
			}
		}

		if (strncmp("HEAD", split, 4) == 0) {
			*end = -1;
		} else {
			*end = strtol(split, &end_ptr, 10);
			if (*end_ptr != 0) {
				return 1;
			}
		}

		if ((*start > *end) && (*end != -1)) {
			return 1;
		}
	}

	if ((errno == EINVAL) || (errno == ERANGE)) {
		return 1;
	}
	return 0;
}


/*---------------------------------------------------------------------------*/
/* Global functions                                                          */
/*---------------------------------------------------------------------------*/

/* Program entry point */
int main(int argc, char **argv)
{
	char ret = 1, dir_created = 0;
	int i;
	session_t session;
	dump_options_t opts;

#if ENABLE_NLS
	setlocale(LC_ALL, "");
	bindtextdomain(PACKAGE, LOCALEDIR);
	textdomain(PACKAGE);
#endif /* ENABLE_NLS */

	/* Init subversion (sets apr locale etc.) */
	if (svn_cmdline_init(PACKAGE, stderr) != EXIT_SUCCESS) {
		return EXIT_FAILURE;
	}
	atexit(apr_terminate);

	session = session_create();
	opts = dump_options_create();

	/* Parse arguments */
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			print_usage();
			return EXIT_SUCCESS;
		} else if (!strcmp(argv[i], "--version")) {
			print_version();
			session_free(&session);
			dump_options_free(&opts);
			return EXIT_SUCCESS;
		} else if (!strcmp(argv[i], "-q") || !strcmp(argv[i], "--quiet")) {
			opts.verbosity = -1;
		} else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) {
			opts.verbosity = 1;
		} else if (!strcmp(argv[i], "--no-auth-cache")) {
			session.flags |= SF_NO_AUTH_CACHE;
		} else if (!strcmp(argv[i], "--non-interactive")) {
			session.flags |= SF_NON_INTERACTIVE;
		} else if (!strcmp(argv[i], "--keep-revnums")) {
			opts.flags |= DF_KEEP_REVNUMS;
		} else if (!strcmp(argv[i], "--deltas")) {
			opts.flags |= DF_USE_DELTAS;
		} else if (!strcmp(argv[i], "--incremental")) {
			opts.flags |= DF_INCREMENTAL;
		} else if (i+1 < argc && (!strcmp(argv[i], "-r") || !strcmp(argv[i], "--revision"))) {
			if (parse_revnum(argv[++i], &opts.start, &opts.end)) {
				fprintf(stderr, _("ERROR: invalid revision range '%s'.\n"), argv[i]);
				session_free(&session);
				dump_options_free(&opts);
				return EXIT_FAILURE;
			}
		} else if (i+1 < argc && (!strcmp(argv[i], "-u") || !strcmp(argv[i], "--username"))) {
			if (session.username != NULL) {
				free(session.username);
			}
			session.username = apr_pstrdup(session.pool, argv[++i]);
			/* No one needs to know the username */
			memset(argv[i], ' ', strlen(argv[i]));
		} else if (i+1 < argc && (!strcmp(argv[i], "-p") || !strcmp(argv[i], "--password"))) {
			if (session.password != NULL) {
				free(session.password);
			}
			session.password = apr_pstrdup(session.pool, argv[++i]);
			/* No one needs to know the password */
			memset(argv[i], ' ', strlen(argv[i]));
		} else if (i+1 < argc && !strcmp(argv[i], "--prefix")) {
			if (opts.prefix != NULL) {
				free(opts.prefix);
			}
			opts.prefix = apr_pstrdup(session.pool, argv[++i]);

		/* Deprecated options */
		} else if (i+1 < argc && !strcmp(argv[i], "--stop")) {
			fprintf(stderr, _("WARNING: the '--stop' option is deprated. Please use '--revision'.\n" \
			                  "         The resulting dump WILL DIFFER from the one obtained with\n" \
			                  "         previous versions of the program if you are dumping a subdirectory.\n"));
			opts.start = 0;
			if (parse_revnum(argv[++i], &opts.end, &opts.end)) {
				fprintf(stderr, _("ERROR: invalid revision number '%s'.\n"), argv[i]);
				session_free(&session);
				dump_options_free(&opts);
				return EXIT_FAILURE;
			}
		} else if (!strcmp(argv[i], "--online") || !strcmp(argv[i], "--dump-uuid")) {
			fprintf(stderr, _("WARNING: the '%s' option is deprecated.\n"), argv[i]);
		} else if (i+1 < argc && (!strcmp(argv[i], "-d") || !strcmp(argv[i], "--download-dir"))) {
			fprintf(stderr, _("WARNING: the '%s' option is deprecated.\n"), argv[i]);
			++i;
		} else if (!strcmp(argv[i], "--no-check-certificate")) {
			fprintf(stderr, _("WARNING: the '%s' option is deprecated and will be IGNORED!.\n"), argv[i]);
		} else if ((i+1 < argc && (!strcmp(argv[i], "-o") || !strcmp(argv[i], "--outfile")))) {
			fprintf(stderr, _("WARNING: the '%s' option is deprecated and will be IGNORED!.\n"), argv[i]);
			++i;
		
		/* An url */
		} else if (svn_path_is_url(argv[i])) {
			if (session.url != NULL) {
				fprintf(stderr, _("ERROR: multiple URLs detected.\n"));
				session_free(&session);
				dump_options_free(&opts);
				return EXIT_FAILURE;
			}
			session.url = apr_pstrdup(session.pool, argv[i]);
		} else {
			fprintf(stderr, _("ERROR: Unkown argument or malformed url '%s'.\n"), argv[i]);
			fprintf(stderr, _("Type %s --help for usage information.\n"), argv[0]);
			session_free(&session);
			dump_options_free(&opts);
			return EXIT_FAILURE;
		}
	}

	/* URL given ? */
	if (session.url == NULL) {
		session_free(&session);
		dump_options_free(&opts);
		print_usage();
		return EXIT_FAILURE;
	}

	/* Generate temporary directory */
	const char *tdir = getenv("TMPDIR");
	if (tdir != NULL) {
		char *tmp = apr_psprintf(session.pool, "%s/"PACKAGE"XXXXXX", tdir);
		opts.temp_dir = utils_canonicalize_pstrdup(session.pool, tmp);
	} else {
		opts.temp_dir = utils_canonicalize_pstrdup(session.pool, "/tmp/"PACKAGE"XXXXXX");
	}
	opts.temp_dir = mkdtemp(opts.temp_dir);
	if (opts.temp_dir == NULL) {
		fprintf(stderr, _("ERROR: Unable to create download directory.\n"));
		session_free(&session);
		dump_options_free(&opts);
		return EXIT_FAILURE;
	}

	/* Do the real work */
	if (session_open(&session) == 0) {
		ret = dump(&session, &opts);
		session_close(&session);
	}

	/* Clean up temporary directory */
#ifndef DUMP_DEBUG
	utils_rrmdir(session.pool, opts.temp_dir, 1);
#endif

	session_free(&session);
	dump_options_free(&opts);
	if (ret == 0) {
		return EXIT_SUCCESS;
	} else {
		return EXIT_FAILURE;
	}
}
