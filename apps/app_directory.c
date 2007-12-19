/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 *
 * \brief Provide a directory of extensions
 *
 * \author Mark Spencer <markster@digium.com>
 * 
 * \ingroup applications
 */
 
#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include <ctype.h>

#include "asterisk/paths.h"	/* use ast_config_AST_SPOOL_DIR */
#include "asterisk/file.h"
#include "asterisk/pbx.h"
#include "asterisk/module.h"
#include "asterisk/say.h"
#include "asterisk/app.h"

#ifdef ODBC_STORAGE
#include <sys/mman.h>
#include "asterisk/res_odbc.h"

static char odbc_database[80] = "asterisk";
static char odbc_table[80] = "voicemessages";
static char vmfmts[80] = "wav";
#endif

static char *app = "Directory";

static char *synopsis = "Provide directory of voicemail extensions";
static char *descrip =
"  Directory(vm-context[,dial-context[,options]]): This application will present\n"
"the calling channel with a directory of extensions from which they can search\n"
"by name. The list of names and corresponding extensions is retrieved from the\n"
"voicemail configuration file, voicemail.conf.\n"
"  This application will immediately exit if one of the following DTMF digits are\n"
"received and the extension to jump to exists:\n"
"    0 - Jump to the 'o' extension, if it exists.\n"
"    * - Jump to the 'a' extension, if it exists.\n\n"
"  Parameters:\n"
"    vm-context   - This is the context within voicemail.conf to use for the\n"
"                   Directory.\n"
"    dial-context - This is the dialplan context to use when looking for an\n"
"                   extension that the user has selected, or when jumping to the\n"
"                   'o' or 'a' extension.\n\n"
"  Options:\n"
"    e - In addition to the name, also read the extension number to the\n"
"        caller before presenting dialing options.\n"
"    f - Allow the caller to enter the first name of a user in the directory\n"
"        instead of using the last name.\n"
"    m - Instead of reading each name sequentially and asking for confirmation,\n"
"        create a menu of up to 8 names.\n";

/* For simplicity, I'm keeping the format compatible with the voicemail config,
   but i'm open to suggestions for isolating it */

#define VOICEMAIL_CONFIG "voicemail.conf"

/* How many digits to read in */
#define NUMDIGITS 3

enum {
	OPT_LISTBYFIRSTNAME = (1 << 0),
	OPT_SAYEXTENSION =    (1 << 1),
	OPT_FROMVOICEMAIL =   (1 << 2),
	OPT_SELECTFROMMENU =  (1 << 3),
} directory_option_flags;

struct items {
	char exten[AST_MAX_EXTENSION + 1];
	char name[AST_MAX_EXTENSION + 1];
};

AST_APP_OPTIONS(directory_app_options, {
	AST_APP_OPTION('f', OPT_LISTBYFIRSTNAME),
	AST_APP_OPTION('e', OPT_SAYEXTENSION),
	AST_APP_OPTION('v', OPT_FROMVOICEMAIL),
	AST_APP_OPTION('m', OPT_SELECTFROMMENU),
});

#ifdef ODBC_STORAGE
struct generic_prepare_struct {
	const char *sql;
	const char *param;
};

static SQLHSTMT generic_prepare(struct odbc_obj *obj, void *data)
{
	struct generic_prepare_struct *gps = data;
	SQLHSTMT stmt;
	int res;

	res = SQLAllocHandle(SQL_HANDLE_STMT, obj->con, &stmt);
	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		ast_log(LOG_WARNING, "SQL Alloc Handle failed!\n");
		return NULL;
	}

	res = SQLPrepare(stmt, (unsigned char *)gps->sql, SQL_NTS);
	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		ast_log(LOG_WARNING, "SQL Prepare failed![%s]\n", (char *)gps->sql);
		SQLFreeHandle(SQL_HANDLE_STMT, stmt);
		return NULL;
	}

	if (!ast_strlen_zero(gps->param))
		SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(gps->param), 0, (void *)gps->param, 0, NULL);

	return stmt;
}

static void retrieve_file(char *dir)
{
	int x = 0;
	int res;
	int fd=-1;
	size_t fdlen = 0;
	void *fdm = MAP_FAILED;
	SQLHSTMT stmt;
	char sql[256];
	char fmt[80]="", empty[10] = "";
	char *c;
	SQLLEN colsize;
	char full_fn[256];
	struct odbc_obj *obj;
	struct generic_prepare_struct gps = { .sql = sql, .param = dir };

	obj = ast_odbc_request_obj(odbc_database, 1);
	if (obj) {
		do {
			ast_copy_string(fmt, vmfmts, sizeof(fmt));
			c = strchr(fmt, '|');
			if (c)
				*c = '\0';
			if (!strcasecmp(fmt, "wav49"))
				strcpy(fmt, "WAV");
			snprintf(full_fn, sizeof(full_fn), "%s.%s", dir, fmt);
			snprintf(sql, sizeof(sql), "SELECT recording FROM %s WHERE dir=? AND msgnum=-1", odbc_table);
			stmt = ast_odbc_prepare_and_execute(obj, generic_prepare, &gps);

			if (!stmt) {
				ast_log(LOG_WARNING, "SQL Execute error!\n[%s]\n\n", sql);
				break;
			}
			res = SQLFetch(stmt);
			if (res == SQL_NO_DATA) {
				SQLFreeHandle(SQL_HANDLE_STMT, stmt);
				break;
			} else if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
				ast_log(LOG_WARNING, "SQL Fetch error!\n[%s]\n\n", sql);
				SQLFreeHandle(SQL_HANDLE_STMT, stmt);
				break;
			}
			fd = open(full_fn, O_RDWR | O_CREAT | O_TRUNC, AST_FILE_MODE);
			if (fd < 0) {
				ast_log(LOG_WARNING, "Failed to write '%s': %s\n", full_fn, strerror(errno));
				SQLFreeHandle(SQL_HANDLE_STMT, stmt);
				break;
			}

			res = SQLGetData(stmt, 1, SQL_BINARY, empty, 0, &colsize);
			fdlen = colsize;
			if (fd > -1) {
				char tmp[1]="";
				lseek(fd, fdlen - 1, SEEK_SET);
				if (write(fd, tmp, 1) != 1) {
					close(fd);
					fd = -1;
					break;
				}
				if (fd > -1)
					fdm = mmap(NULL, fdlen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
			}
			if (fdm != MAP_FAILED) {
				memset(fdm, 0, fdlen);
				res = SQLGetData(stmt, x + 1, SQL_BINARY, fdm, fdlen, &colsize);
				if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
					ast_log(LOG_WARNING, "SQL Get Data error!\n[%s]\n\n", sql);
					SQLFreeHandle(SQL_HANDLE_STMT, stmt);
					break;
				}
			}
			SQLFreeHandle(SQL_HANDLE_STMT, stmt);
		} while (0);
		ast_odbc_release_obj(obj);
	} else
		ast_log(LOG_WARNING, "Failed to obtain database object for '%s'!\n", odbc_database);
	if (fdm != MAP_FAILED)
		munmap(fdm, fdlen);
	if (fd > -1)
		close(fd);
	return;
}
#endif

static char *convert(const char *lastname)
{
	char *tmp;
	int lcount = 0;
	tmp = ast_malloc(NUMDIGITS + 1);
	if (tmp) {
		while((*lastname > 32) && lcount < NUMDIGITS) {
			switch(toupper(*lastname)) {
			case '1':
				tmp[lcount++] = '1';
				break;
			case '2':
			case 'A':
			case 'B':
			case 'C':
				tmp[lcount++] = '2';
				break;
			case '3':
			case 'D':
			case 'E':
			case 'F':
				tmp[lcount++] = '3';
				break;
			case '4':
			case 'G':
			case 'H':
			case 'I':
				tmp[lcount++] = '4';
				break;
			case '5':
			case 'J':
			case 'K':
			case 'L':
				tmp[lcount++] = '5';
				break;
			case '6':
			case 'M':
			case 'N':
			case 'O':
				tmp[lcount++] = '6';
				break;
			case '7':
			case 'P':
			case 'Q':
			case 'R':
			case 'S':
				tmp[lcount++] = '7';
				break;
			case '8':
			case 'T':
			case 'U':
			case 'V':
				tmp[lcount++] = '8';
				break;
			case '9':
			case 'W':
			case 'X':
			case 'Y':
			case 'Z':
				tmp[lcount++] = '9';
				break;
			}
			lastname++;
		}
		tmp[lcount] = '\0';
	}
	return tmp;
}

/* play name of mailbox owner.
 * returns:  -1 for bad or missing extension
 *           '1' for selected entry from directory
 *           '*' for skipped entry from directory
 */
static int play_mailbox_owner(struct ast_channel *chan, const char *context,
		const char *ext, const char *name, struct ast_flags *flags)
{
	int res = 0;
	char fn[256];

	/* Check for the VoiceMail2 greeting first */
	snprintf(fn, sizeof(fn), "%s/voicemail/%s/%s/greet",
		ast_config_AST_SPOOL_DIR, context, ext);
#ifdef ODBC_STORAGE
	retrieve_file(fn);
#endif

	if (ast_fileexists(fn, NULL, chan->language) <= 0) {
		/* no file, check for an old-style Voicemail greeting */
		snprintf(fn, sizeof(fn), "%s/vm/%s/greet",
			ast_config_AST_SPOOL_DIR, ext);
	}
#ifdef ODBC_STORAGE
	retrieve_file(fn);
#endif

	if (ast_fileexists(fn, NULL, chan->language) > 0) {
		res = ast_stream_and_wait(chan, fn, AST_DIGIT_ANY);
		ast_stopstream(chan);
		/* If Option 'e' was specified, also read the extension number with the name */
		if (ast_test_flag(flags, OPT_SAYEXTENSION)) {
			ast_stream_and_wait(chan, "vm-extension", AST_DIGIT_ANY);
			res = ast_say_character_str(chan, ext, AST_DIGIT_ANY, chan->language);
		}
	} else {
		res = ast_say_character_str(chan, S_OR(name, ext), AST_DIGIT_ANY, chan->language);
		if (!ast_strlen_zero(name) && ast_test_flag(flags, OPT_SAYEXTENSION)) {
			ast_stream_and_wait(chan, "vm-extension", AST_DIGIT_ANY);
			res = ast_say_character_str(chan, ext, AST_DIGIT_ANY, chan->language);
		}
	}
#ifdef ODBC_STORAGE
	ast_filedelete(fn, NULL);	
#endif

	return res;
}

static int get_mailbox_response(struct ast_channel *chan, const char *context, const char *dialcontext, const char *ext, const char *name, struct ast_flags *flags)
{
	int res = 0;
	int loop = 3;

	res = play_mailbox_owner(chan, context, ext, name, flags);
	for (loop = 3 ; loop > 0; loop--) {
		if (!res)
			res = ast_stream_and_wait(chan, "dir-instr", AST_DIGIT_ANY);
		if (!res)
			res = ast_waitfordigit(chan, 3000);
		ast_stopstream(chan);
	
		if (res < 0) /* User hungup, so jump out now */
			break;
		if (res == '1') {	/* Name selected */
			if (ast_test_flag(flags, OPT_FROMVOICEMAIL)) {
				/* We still want to set the exten though */
				ast_copy_string(chan->exten, ext, sizeof(chan->exten));
			} else {
				if (ast_goto_if_exists(chan, dialcontext, ext, 1)) {
					ast_log(LOG_WARNING,
						"Can't find extension '%s' in context '%s'.  "
						"Did you pass the wrong context to Directory?\n",
						ext, dialcontext);
					res = -1;
				}
			}
			break;
		}
		if (res == '*') /* Skip to next match in list */
			break;

		/* Not '1', or '*', so decrement number of tries */
		res = 0;
	}

	return(res);
}

static int select_item(struct ast_channel *chan, const struct items *items, int itemcount, const char *context, const char *dialcontext, struct ast_flags *flags)
{
	int i, res = 0;
	char buf[9];
	for (i = 0; i < itemcount; i++) {
		snprintf(buf, sizeof(buf), "digits/%d", i + 1);
		/* Press <num> for <name>, [ extension <ext> ] */
		res = ast_streamfile(chan, "dir-multi1", chan->language) ||
			ast_waitstream(chan, AST_DIGIT_ANY) ||
			ast_streamfile(chan, buf, chan->language) ||
			ast_waitstream(chan, AST_DIGIT_ANY) ||
			ast_streamfile(chan, "dir-multi2", chan->language) ||
			ast_waitstream(chan, AST_DIGIT_ANY) ||
			play_mailbox_owner(chan, context, items[i].exten, items[i].name, flags) ||
			ast_waitstream(chan, AST_DIGIT_ANY) ||
			ast_waitfordigit(chan, 800);
		if (res)
			break;
	}
	/* Press "9" for more names. */
	if (!res)
		res = ast_waitstream(chan, AST_DIGIT_ANY) ||
			(itemcount == 8 && ast_streamfile(chan, "dir-multi9", chan->language)) ||
			ast_waitstream(chan, AST_DIGIT_ANY) ||
			ast_waitfordigit(chan, 3000);

	if (res && res > '0' && res < '9') {
		if (ast_test_flag(flags, OPT_FROMVOICEMAIL)) {
			/* We still want to set the exten */
			ast_copy_string(chan->exten, items[res - '0'].exten, sizeof(chan->exten));
		} else if (ast_goto_if_exists(chan, dialcontext, items[res - '1'].exten, 1)) {
			ast_log(LOG_WARNING,
				"Can't find extension '%s' in context '%s'.  "
				"Did you pass the wrong context to Directory?\n",
				items[res - '0'].exten, dialcontext);
			res = -1;
		}
	}
	return res;
}

static struct ast_config *realtime_directory(char *context)
{
	struct ast_config *cfg;
	struct ast_config *rtdata;
	struct ast_category *cat;
	struct ast_variable *var;
	char *mailbox;
	const char *fullname;
	const char *hidefromdir;
	char tmp[100];
	struct ast_flags config_flags = { 0 };

	/* Load flat file config. */
	cfg = ast_config_load(VOICEMAIL_CONFIG, config_flags);

	if (!cfg) {
		/* Loading config failed. */
		ast_log(LOG_WARNING, "Loading config failed.\n");
		return NULL;
	}

	/* Get realtime entries, categorized by their mailbox number
	   and present in the requested context */
	rtdata = ast_load_realtime_multientry("voicemail", "mailbox LIKE", "%", "context", context, NULL);

	/* if there are no results, just return the entries from the config file */
	if (!rtdata)
		return cfg;

	/* Does the context exist within the config file? If not, make one */
	cat = ast_category_get(cfg, context);
	if (!cat) {
		cat = ast_category_new(context, "", 99999);
		if (!cat) {
			ast_log(LOG_WARNING, "Out of memory\n");
			ast_config_destroy(cfg);
			return NULL;
		}
		ast_category_append(cfg, cat);
	}

	mailbox = NULL;
	while ( (mailbox = ast_category_browse(rtdata, mailbox)) ) {
		fullname = ast_variable_retrieve(rtdata, mailbox, "fullname");
		hidefromdir = ast_variable_retrieve(rtdata, mailbox, "hidefromdir");
		snprintf(tmp, sizeof(tmp), "no-password,%s,hidefromdir=%s",
			 fullname ? fullname : "",
			 hidefromdir ? hidefromdir : "no");
		var = ast_variable_new(mailbox, tmp, "");
		if (var)
			ast_variable_append(cat, var);
		else
			ast_log(LOG_WARNING, "Out of memory adding mailbox '%s'\n", mailbox);
	}
	ast_config_destroy(rtdata);

	return cfg;
}

static int do_directory(struct ast_channel *chan, struct ast_config *vmcfg, struct ast_config *ucfg, char *context, char *dialcontext, char digit, struct ast_flags *flags)
{
	/* Read in the first three digits..  "digit" is the first digit, already read */
	char ext[NUMDIGITS + 1], *cat;
	char name[80] = "";
	struct ast_variable *v;
	int res;
	int found=0;
	int lastuserchoice = 0;
	char *start, *conv = NULL, *stringp = NULL;
	char *pos;
	int breakout = 0;

	if (ast_strlen_zero(context)) {
		ast_log(LOG_WARNING,
			"Directory must be called with an argument "
			"(context in which to interpret extensions)\n");
		return -1;
	}
	if (digit == '0') {
		if (!ast_goto_if_exists(chan, dialcontext, "o", 1) ||
		    (!ast_strlen_zero(chan->macrocontext) &&
		     !ast_goto_if_exists(chan, chan->macrocontext, "o", 1))) {
			return 0;
		} else {
			ast_log(LOG_WARNING, "Can't find extension 'o' in current context.  "
				"Not Exiting the Directory!\n");
			res = 0;
		}
	}	
	if (digit == '*') {
		if (!ast_goto_if_exists(chan, dialcontext, "a", 1) ||
		    (!ast_strlen_zero(chan->macrocontext) &&
		     !ast_goto_if_exists(chan, chan->macrocontext, "a", 1))) {
			return 0;
		} else {
			ast_log(LOG_WARNING, "Can't find extension 'a' in current context.  "
				"Not Exiting the Directory!\n");
			res = 0;
		}
	}	
	memset(ext, 0, sizeof(ext));
	ext[0] = digit;
	res = 0;
	if (ast_readstring(chan, ext + 1, NUMDIGITS - 1, 3000, 3000, "#") < 0) res = -1;
	if (!res) {
		if (ast_test_flag(flags, OPT_SELECTFROMMENU)) {
			char buf[AST_MAX_EXTENSION + 1], *bufptr, *fullname;
			struct items menuitems[8];
			int menucount = 0;
			for (v = ast_variable_browse(vmcfg, context); v; v = v->next) {
				if (strcasestr(v->value, "hidefromdir=yes") == NULL) {
					ast_copy_string(buf, v->value, sizeof(buf));
					bufptr = buf;
					/* password,Full Name,email,pager,options */
					strsep(&bufptr, ",");
					pos = strsep(&bufptr, ",");
					fullname = pos;

					if (ast_test_flag(flags, OPT_LISTBYFIRSTNAME) && strrchr(pos, ' '))
						pos = strrchr(pos, ' ') + 1;
					conv = convert(pos);

					if (conv && strcmp(conv, ext) == 0) {
						/* Match */
						found = 1;
						ast_copy_string(menuitems[menucount].name, fullname, sizeof(menuitems[0].name));
						ast_copy_string(menuitems[menucount].exten, v->name, sizeof(menuitems[0].exten));
						menucount++;
					}

					if (menucount == 8) {
						/* We have a full menu */
						res = select_item(chan, menuitems, menucount, context, dialcontext, flags);
						menucount = 0;
						if (res != '9' && res != 0) {
							if (res != -1)
								lastuserchoice = res;
							break;
						}
					}
				}
			}

			if (menucount > 0) {
				/* We have a partial menu left over */
				res = select_item(chan, menuitems, menucount, context, dialcontext, flags);
				if (res != '9') {
					if (res != -1)
						lastuserchoice = res;
				}
			}

			/* Make this flag conform to the old expected result */
			if (lastuserchoice > '1' && lastuserchoice < '9')
				lastuserchoice = '1';
		} else {
			/* Search for all names which start with those digits */
			v = ast_variable_browse(vmcfg, context);
			while(v && !res) {
				/* Find all candidate extensions */
				while(v) {
					/* Find a candidate extension */
					start = ast_strdup(v->value);
					if (start && !strcasestr(start, "hidefromdir=yes")) {
						stringp=start;
						strsep(&stringp, ",");
						pos = strsep(&stringp, ",");
						if (pos) {
							ast_copy_string(name, pos, sizeof(name));
							/* Grab the last name */
							if (!ast_test_flag(flags, OPT_LISTBYFIRSTNAME) && strrchr(pos,' '))
								pos = strrchr(pos, ' ') + 1;
							conv = convert(pos);
							if (conv) {
								if (!strncmp(conv, ext, strlen(ext))) {
									/* Match! */
									found++;
									ast_free(conv);
									ast_free(start);
									break;
								}
								ast_free(conv);
							}
						}
						ast_free(start);
					}
					v = v->next;
				}

				if (v) {
					/* We have a match -- play a greeting if they have it */
					res = get_mailbox_response(chan, context, dialcontext, v->name, name, flags);
					switch (res) {
					case -1:
						/* user pressed '1' but extension does not exist, or
						 * user hungup
						 */
						lastuserchoice = 0;
						break;
					case '1':
						/* user pressed '1' and extensions exists;
						   get_mailbox_response will already have done
						   a goto() on the channel
						 */
						lastuserchoice = res;
						break;
					case '*':
						/* user pressed '*' to skip something found */
						lastuserchoice = res;
						res = 0;
						break;
					default:
						break;
					}
					v = v->next;
				}
			}
		}

		if (!res && ucfg) {
			/* Search users.conf for all names which start with those digits */
			if (ast_test_flag(flags, OPT_SELECTFROMMENU)) {
				char *fullname = NULL;
				struct items menuitems[8];
				int menucount = 0;
				for (cat = ast_category_browse(ucfg, NULL); cat && !res ; cat = ast_category_browse(ucfg, cat)) {
					const char *pos;
					if (!strcasecmp(cat, "general"))
						continue;
					if (!ast_true(ast_config_option(ucfg, cat, "hasdirectory")))
						continue;
				
					/* Find all candidate extensions */
					if ((pos = ast_variable_retrieve(ucfg, cat, "fullname"))) {
						ast_copy_string(name, pos, sizeof(name));
						/* Grab the last name */
						if (!ast_test_flag(flags, OPT_LISTBYFIRSTNAME) && strrchr(pos,' '))
							pos = strrchr(pos, ' ') + 1;
						conv = convert(pos);
						if (conv && strcmp(conv, ext) == 0) {
							/* Match */
							found = 1;
							ast_copy_string(menuitems[menucount].name, fullname, sizeof(menuitems[0].name));
							ast_copy_string(menuitems[menucount].exten, v->name, sizeof(menuitems[0].exten));
							menucount++;

							if (menucount == 8) {
								/* We have a full menu */
								res = select_item(chan, menuitems, menucount, context, dialcontext, flags);
								menucount = 0;
								if (res != '9' && res != 0) {
									if (res != -1)
										lastuserchoice = res;
									break;
								}
							}
						}
					}
				}
				if (menucount > 0) {
					/* We have a partial menu left over */
					res = select_item(chan, menuitems, menucount, context, dialcontext, flags);
					if (res != '9') {
						if (res != -1)
							lastuserchoice = res;
					}
				}

				/* Make this flag conform to the old expected result */
				if (lastuserchoice > '1' && lastuserchoice < '9')
					lastuserchoice = '1';
			} else { /* !menu */
				for (cat = ast_category_browse(ucfg, NULL); cat && !res ; cat = ast_category_browse(ucfg, cat)) {
					const char *pos;
					if (!strcasecmp(cat, "general"))
						continue;
					if (!ast_true(ast_config_option(ucfg, cat, "hasdirectory")))
						continue;
				
					/* Find all candidate extensions */
					if ((pos = ast_variable_retrieve(ucfg, cat, "fullname"))) {
						ast_copy_string(name, pos, sizeof(name));
						/* Grab the last name */
						if (ast_test_flag(flags, OPT_LISTBYFIRSTNAME) && strrchr(pos,' '))
							pos = strrchr(pos, ' ') + 1;
						conv = convert(pos);
						if (conv && strcmp(conv, ext) == 0) {
							/* Match! */
							found++;
							/* We have a match -- play a greeting if they have it */
							res = get_mailbox_response(chan, context, dialcontext, cat, name, flags);
						}
						switch (res) {
						case -1:
							/* user pressed '1' but extension does not exist, or
							 * user hungup
							 */
							lastuserchoice = 0;
							breakout = 1;
							break;
						case '1':
							/* user pressed '1' and extensions exists;
							   play_mailbox_owner will already have done
							   a goto() on the channel
							 */
							lastuserchoice = res;
							breakout = 1;
							break;
						case '*':
							/* user pressed '*' to skip something found */
							lastuserchoice = res;
							breakout = 0;
							res = 0;
							break;
						default:
							breakout = 1;
							break;
						}
						ast_free(conv);
						if (breakout)
							break;
					} else
						ast_free(conv);
				}
			}
		}

		if (lastuserchoice != '1') {
			res = ast_streamfile(chan, found ? "dir-nomore" : "dir-nomatch", chan->language);
			if (!res)
				res = 1;
			return res;
		}
		return 0;
	}
	return res;
}

static int directory_exec(struct ast_channel *chan, void *data)
{
	int res = 0;
	struct ast_config *cfg, *ucfg;
	const char *dirintro;
	char *parse, *opts[0];
	struct ast_flags flags = { 0 };
	struct ast_flags config_flags = { 0 };
	AST_DECLARE_APP_ARGS(args,
		AST_APP_ARG(vmcontext);
		AST_APP_ARG(dialcontext);
		AST_APP_ARG(options);
	);

	if (ast_strlen_zero(data)) {
		ast_log(LOG_WARNING, "Directory requires an argument (context[,dialcontext])\n");
		return -1;
	}

	parse = ast_strdupa(data);

	AST_STANDARD_APP_ARGS(args, parse);

	if (args.options && ast_app_parse_options(directory_app_options, &flags, opts, args.options))
		return -1;

	if (ast_strlen_zero(args.dialcontext))	
		args.dialcontext = args.vmcontext;

	cfg = realtime_directory(args.vmcontext);
	if (!cfg) {
		ast_log(LOG_ERROR, "Unable to read the configuration data!\n");
		return -1;
	}
	
	ucfg = ast_config_load("users.conf", config_flags);

	dirintro = ast_variable_retrieve(cfg, args.vmcontext, "directoryintro");
	if (ast_strlen_zero(dirintro))
		dirintro = ast_variable_retrieve(cfg, "general", "directoryintro");
	if (ast_strlen_zero(dirintro))
		dirintro = ast_test_flag(&flags, OPT_LISTBYFIRSTNAME) ? "dir-intro-fn" : "dir-intro";

	if (chan->_state != AST_STATE_UP) 
		res = ast_answer(chan);

	for (;;) {
		if (!res)
			res = ast_stream_and_wait(chan, dirintro, AST_DIGIT_ANY);
		ast_stopstream(chan);
		if (!res)
			res = ast_waitfordigit(chan, 5000);
		if (res > 0) {
			res = do_directory(chan, cfg, ucfg, args.vmcontext, args.dialcontext, res, &flags);
			if (res > 0) {
				res = ast_waitstream(chan, AST_DIGIT_ANY);
				ast_stopstream(chan);
				if (res >= 0)
					continue;
			}
		}
		break;
	}
	if (ucfg)
		ast_config_destroy(ucfg);
	ast_config_destroy(cfg);
	return res;
}

static int unload_module(void)
{
	int res;
	res = ast_unregister_application(app);
	return res;
}

static int load_module(void)
{
#ifdef ODBC_STORAGE
	struct ast_flags config_flags = { 0 };
	struct ast_config *cfg = ast_config_load(VOICEMAIL_CONFIG, config_flags);
	const char *tmp;

	if (cfg) {
		if ((tmp = ast_variable_retrieve(cfg, "general", "odbcstorage"))) {
			ast_copy_string(odbc_database, tmp, sizeof(odbc_database));
		}
		if ((tmp = ast_variable_retrieve(cfg, "general", "odbctable"))) {
			ast_copy_string(odbc_table, tmp, sizeof(odbc_table));
		}
		if ((tmp = ast_variable_retrieve(cfg, "general", "format"))) {
			ast_copy_string(vmfmts, tmp, sizeof(vmfmts));
		}
		ast_config_destroy(cfg);
	} else
		ast_log(LOG_WARNING, "Unable to load " VOICEMAIL_CONFIG " - ODBC defaults will be used\n");
#endif

	return ast_register_application(app, directory_exec, synopsis, descrip);
}

AST_MODULE_INFO_STANDARD(ASTERISK_GPL_KEY, "Extension Directory");
