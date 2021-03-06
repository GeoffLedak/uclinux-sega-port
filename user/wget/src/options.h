/* struct options.
   Copyright (C) 1995, 1996, 1997 Free Software Foundation, Inc.

This file is part of Wget.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.  */

/* Needed for FDP.  */
#include <stdio.h>

struct options
{
  int verbose;			/* Are we verbose? */
  int quiet;			/* Are we quiet? */
  int ntry;			/* Number of tries per URL */
  int background;		/* Whether we should work in background. */
  int kill_longer;		/* Do we reject messages with *more*
				   data than specified in
				   content-length? */
  int ignore_length;		/* Do we heed content-length at all?  */
  int recursive;		/* Are we recursive? */
  int spanhost;			/* Do we span across hosts in
				   recursion? */
  int relative_only;		/* Follow only relative links. */
  int no_parent;		/* Restrict access to the parent
				   directory.  */
  int simple_check;		/* Should we use simple checking
				   (strcmp) or do we create a host
				   hash and call gethostbyname? */
  int reclevel;			/* Maximum level of recursion */
  int dirstruct;		/* Do we build the directory structure
				  as we go along? */
  int no_dirstruct;		/* Do we hate dirstruct? */
  int cut_dirs;			/* Number of directory components to cut. */
  int add_hostdir;		/* Do we add hostname directory? */
  int noclobber;		/* Disables clobbering of existing
				   data. */
  char *dir_prefix;		/* The top of directory tree */
  char *lfilename;		/* Log filename */
  int no_flush;			/* If non-zero, inhibit flushing log. */
  char *input_filename;		/* Input filename */
  int force_html;		/* Is the input file an HTML file? */

  int spider;			/* Is Wget in spider mode? */

  char **accepts;		/* List of patterns to accept. */
  char **rejects;		/* List of patterns to reject. */
  char **excludes;		/* List of excluded FTP directories. */
  char **includes;		/* List of FTP directories to
				   follow. */

  char **domains;		/* See host.c */
  char **exclude_domains;

  char **follow_tags;           /* List of HTML tags to recursively follow. */
  char **ignore_tags;           /* List of HTML tags to ignore if recursing. */

  int follow_ftp;		/* Are FTP URL-s followed in recursive
				   retrieving? */
  int retr_symlinks;		/* Whether we retrieve symlinks in
				   FTP. */
  char *output_document;	/* The output file to which the
				   documents will be printed.  */
  int od_known_regular;		/* whether output_document is a
                                   regular file we can manipulate,
                                   i.e. not `-' or a device file. */
  FILE *dfp;			/* The file pointer to the output
				   document. */

  int always_rest;		/* Always use REST. */
  char *ftp_acc;		/* FTP username */
  char *ftp_pass;		/* FTP password */
  int netrc;			/* Whether to read .netrc. */
  int ftp_glob;			/* FTP globbing */
  int ftp_pasv;			/* Passive FTP. */

  char *http_user;		/* HTTP user. */
  char *http_passwd;		/* HTTP password. */
  char *user_header;		/* User-defined header(s). */

  int use_proxy;		/* Do we use proxy? */
  int proxy_cache;		/* Do we load from proxy cache? */
  char *http_proxy, *ftp_proxy;
  char **no_proxy;
  char *base_href;
  char *proxy_user; /*oli*/
  char *proxy_passwd;
#ifdef HAVE_SELECT
  long timeout;			/* The value of read timeout in
				   seconds. */
#endif
  long wait;			/* The wait period between retrievals. */
  long waitretry;		/* The wait period between retries. - HEH */
  int use_robots;		/* Do we heed robots.txt? */

  long quota;			/* Maximum number of bytes to
				   retrieve. */
  VERY_LONG_TYPE downloaded;	/* How much we downloaded already. */
  int downloaded_overflow;	/* Whether the above overflowed. */
  int numurls;			/* Number of successfully downloaded
				   URLs */

  int server_response;		/* Do we print server response? */
  int save_headers;		/* Do we save headers together with
				   file? */

#ifdef DEBUG
  int debug;			/* Debugging on/off */
#endif /* DEBUG */

  int timestamping;		/* Whether to use time-stamping. */

  int backup_converted;		/* Do we save pre-converted files as *.orig? */
  int backups;			/* Are numeric backups made? */

  char *useragent;		/* Naughty User-Agent, which can be
				   set to something other than
				   Wget. */
  char *referer;		/* Naughty Referer, which can be
				   set to something other than
				   NULL. */
  int convert_links;		/* Will the links be converted
				   locally? */
  int remove_listing;		/* Do we remove .listing files
				   generated by FTP? */
  int htmlify;			/* Do we HTML-ify the OS-dependent
				   listings? */

  long dot_bytes;		/* How many bytes in a printing
				   dot. */
  int dots_in_line;		/* How many dots in one line. */
  int dot_spacing;		/* How many dots between spacings. */

  int delete_after;		/* Whether the files will be deleted
				   after download. */

  int html_extension;		/* Use ".html" extension on all text/html? */

  int page_requisites;		/* Whether we need to download all files
				   necessary to display a page properly. */

  struct sockaddr_in *bind_address; /* What local IP address to bind to. */
};

#ifndef OPTIONS_DEFINED_HERE
extern struct options opt;
#endif
