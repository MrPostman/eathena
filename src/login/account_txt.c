#include "../common/db.h"
#include "../common/lock.h"
#include "../common/malloc.h"
#include "../common/mmo.h"
#include "../common/showmsg.h"
#include "../common/strlib.h"
#include "../common/timer.h"
#include "account.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// global defines
extern char account_txt[];
#define AUTHS_BEFORE_SAVE 10 // flush every 10 saves
#define AUTH_SAVING_INTERVAL 60000 // flush every 10 minutes

/// internal structure
typedef struct AccountDB_TXT
{
	AccountDB vtable;      // public interface
	DBMap* accounts;       // in-memory accounts storage
	int next_account_id;   // auto_increment
	int auths_before_save; // prevents writing to disk too often
	int save_timer;        // save timer id

} AccountDB_TXT;

/// internal functions
static bool account_db_txt_init(AccountDB* self);
static bool account_db_txt_free(AccountDB* self);
static bool account_db_txt_create(AccountDB* self, const struct mmo_account* acc);
static bool account_db_txt_remove(AccountDB* self, const int account_id);
static bool account_db_txt_save(AccountDB* self, const struct mmo_account* acc);
static bool account_db_txt_load_num(AccountDB* self, struct mmo_account* acc, const int account_id);
static bool account_db_txt_load_str(AccountDB* self, struct mmo_account* acc, const char* userid);

static struct mmo_account* mmo_auth_fromstr(char* str, unsigned int version);
static void mmo_auth_tostr(char* str, const struct mmo_account* acc);
static void mmo_auth_sync(AccountDB_TXT* self);
static int mmo_auth_sync_timer(int tid, unsigned int tick, int id, int data);

/// public constructor
AccountDB* account_db_txt(void)
{
	AccountDB_TXT* db = (AccountDB_TXT*)aCalloc(1, sizeof(AccountDB_TXT));

	db->vtable.init     = &account_db_txt_init;
	db->vtable.free     = &account_db_txt_free;
	db->vtable.save     = &account_db_txt_save;
	db->vtable.create   = &account_db_txt_create;
	db->vtable.remove   = &account_db_txt_remove;
	db->vtable.load_num = &account_db_txt_load_num;
	db->vtable.load_str = &account_db_txt_load_str;

	return &db->vtable;
}


/* ------------------------------------------------------------------------- */


/// opens accounts file, loads it, and starts a periodic saving timer
static bool account_db_txt_init(AccountDB* self)
{
	AccountDB_TXT* db = (AccountDB_TXT*)self;
	DBMap* accounts;
	FILE* fp;
	char line[2048];
	unsigned int version = 0;

	// create accounts database
	db->accounts = idb_alloc(DB_OPT_RELEASE_DATA);
	db->next_account_id = START_ACCOUNT_NUM;
	db->auths_before_save = AUTHS_BEFORE_SAVE;
	db->save_timer = INVALID_TIMER;
	accounts = db->accounts;

	// open data file
	fp = fopen(account_txt, "r");
	if( fp == NULL )
	{
		// no account file -> no account -> no login, including char-server (ERROR)
		ShowError(CL_RED"account_db_txt_init: Accounts file [%s] not found."CL_RESET"\n", account_txt);
		return false;
	}

	// load data file
	while( fgets(line, sizeof(line), fp) != NULL )
	{
		int account_id, n;
		unsigned int v;
		struct mmo_account* acc;

		if( line[0] == '/' && line[1] == '/' )
			continue;

		if( sscanf(line, "%d%n", &v, &n) == 1 && line[n] == '\n' )
		{// format version definition
			version = v;
			continue;
		}

		if( sscanf(line, "%d\t%%newid%%%n", &account_id, &n) == 1 && line[n] == '\n' && account_id > db->next_account_id )
		{// auto-increment
			db->next_account_id = account_id;
			continue;
		}

		if( (acc = mmo_auth_fromstr(line, version)) == NULL )
			continue;

		//TODO: apply constraints & checks here
/*
		if (account_id > END_ACCOUNT_NUM) {
			ShowError(CL_RED"mmmo_auth_init: an account has an id higher than %d\n", END_ACCOUNT_NUM);
			ShowError("               account id #%d -> account not read (data is lost!)."CL_RESET"\n", account_id);
			continue;
		}
*/
/*
		userid[23] = '\0';
		remove_control_chars(userid);
		for(j = 0; j < auth_num; j++) {
			if (auth_dat[j].account_id == account_id) {
				ShowError(CL_RED"mmmo_auth_init: an account has an identical id to another.\n");
				ShowError("               account id #%d -> new account not read (data is lost!)."CL_RED"\n", account_id);
				break;
			} else if (strcmp(auth_dat[j].userid, userid) == 0) {
				ShowError(CL_RED"mmmo_auth_init: account name already exists.\n");
				ShowError("               account name '%s' -> new account not read (data is lost!)."CL_RESET"\n", userid); // 2 lines, account name can be long.
				break;
			}
		}
		if (j != auth_num)
			continue;
*/

		if( idb_get(accounts, acc->account_id) != NULL )
		{// account id already occupied
			aFree(acc);
			continue;
		}

		// record entry in db
		idb_put(accounts, acc->account_id, acc);

		if( db->next_account_id < acc->account_id)
			db->next_account_id = acc->account_id + 1;
	}

	// close data file
	fclose(fp);

	// initialize data saving timer
	add_timer_func_list(mmo_auth_sync_timer, "mmo_auth_sync_timer");
	db->save_timer = add_timer_interval(gettick() + AUTH_SAVING_INTERVAL, mmo_auth_sync_timer, 0, (int)db, AUTH_SAVING_INTERVAL);

	return true;
}

/// flush accounts db, close savefile and deallocate structures
static bool account_db_txt_free(AccountDB* self)
{
	AccountDB_TXT* db = (AccountDB_TXT*)self;
	DBMap* accounts = db->accounts;

	// stop saving timer
	delete_timer(db->save_timer, mmo_auth_sync_timer);

	// write data
	mmo_auth_sync(db);

	// delete accounts database
	accounts->destroy(accounts, NULL);
	db->accounts = NULL;

	// delete entire structure
	aFree(db);

	return true;
}

/// add a new entry for this account to the account db and save it
static bool account_db_txt_create(AccountDB* self, const struct mmo_account* acc)
{
	AccountDB_TXT* db = (AccountDB_TXT*)self;
	DBMap* accounts = db->accounts;
	int account_id = db->next_account_id;

	// check if the account_id is free
	struct mmo_account* tmp = idb_get(accounts, account_id);
	if( tmp != NULL )
	{// fatal error condition - entry already present
		return false;
	}

	// copy the data and store it in the db
	CREATE(tmp, struct mmo_account, 1);
	memcpy(tmp, acc, sizeof(struct mmo_account));
	idb_put(accounts, account_id, tmp);

	// increment the auto_increment value
	db->next_account_id++;

	// flush data
	mmo_auth_sync(db);

	return true;
}

/// find an existing entry for this account id and delete it
static bool account_db_txt_remove(AccountDB* self, const int account_id)
{
	AccountDB_TXT* db = (AccountDB_TXT*)self;
	DBMap* accounts = db->accounts;

	//TODO: find out if this really works
	struct mmo_account* tmp = idb_remove(accounts, account_id);
	if( tmp == NULL )
	{// error condition - entry not present
		return false;
	}

	// flush data
	mmo_auth_sync(db);

	return true;
}

/// rewrite the data stored in the account_db with the one provided
static bool account_db_txt_save(AccountDB* self, const struct mmo_account* acc)
{
	AccountDB_TXT* db = (AccountDB_TXT*)self;
	DBMap* accounts = db->accounts;
	int account_id = acc->account_id;

	// retrieve previous data
	struct mmo_acount* tmp = idb_get(accounts, account_id);
	if( tmp == NULL )
	{// error condition - entry not found
		return false;
	}
	
	// overwrite with new data
	memcpy(tmp, acc, sizeof(struct mmo_account));

	// modify save counter and save if needed
	if( --db->auths_before_save == 0 )
		mmo_auth_sync(db);

	return true;
}

/// retrieve data from db and store it in the provided data structure
static bool account_db_txt_load_num(AccountDB* self, struct mmo_account* acc, const int account_id)
{
	AccountDB_TXT* db = (AccountDB_TXT*)self;
	DBMap* accounts = db->accounts;

	// retrieve data
	struct mmo_account* tmp = idb_get(accounts, account_id);
	if( tmp == NULL )
	{// entry not found
		memset(acc, '\0', sizeof(struct mmo_account));
		return false;
	}

	// store it
	memcpy(acc, tmp, sizeof(struct mmo_account));

	return true;
}

/// retrieve data from db and store it in the provided data structure
static bool account_db_txt_load_str(AccountDB* self, struct mmo_account* acc, const char* userid)
{
	AccountDB_TXT* db = (AccountDB_TXT*)self;
	DBMap* accounts = db->accounts;

	// retrieve data
	struct DBIterator* iter = accounts->iterator(accounts);
	struct mmo_account* tmp;
	for( tmp = (struct mmo_account*)iter->first(iter,NULL); iter->exists(iter); tmp = (struct mmo_account*)iter->next(iter,NULL) )
	{
		//TODO: case-sensitivity settings
		if( strcmp(userid, tmp->userid) == 0 )
			break;
	}
	iter->destroy(iter);

	if( tmp == NULL )
	{// entry not found
		memset(acc, '\0', sizeof(struct mmo_account));
		return false;
	}

	// store it
	memcpy(acc, tmp, sizeof(struct mmo_account));

	return true;
}


/// parse input string into a newly allocated account data structure
static struct mmo_account* mmo_auth_fromstr(char* str, unsigned int version)
{
	struct mmo_account* a;
	char* fields[32];
	int count;
	char* regs;
	int i, n;

	CREATE(a, struct mmo_account, 1);

	// extract tab-separated columns from line
	count = sv_split(str, strlen(str), 0, '\t', fields, ARRAYLENGTH(fields), SV_NOESCAPE_NOTERMINATE);

	if( version == 20080409 && count == 13 )
	{
		a->account_id = strtol(fields[1], NULL, 10);
		safestrncpy(a->userid, fields[2], sizeof(a->userid));
		safestrncpy(a->pass, fields[3], sizeof(a->pass));
		a->sex = fields[4][0];
		safestrncpy(a->email, fields[5], sizeof(a->email));
		a->level = strtoul(fields[6], NULL, 10);
		a->state = strtoul(fields[7], NULL, 10);
		a->unban_time = strtol(fields[8], NULL, 10);
		a->expiration_time = strtol(fields[9], NULL, 10);
		a->logincount = strtol(fields[10], NULL, 10);
		safestrncpy(a->lastlogin, fields[11], sizeof(a->lastlogin));
		safestrncpy(a->last_ip, fields[12], sizeof(a->last_ip));
		regs = fields[13];
	}
	else
	if( version == 0 && count == 14 )
	{
		a->account_id = strtol(fields[1], NULL, 10);
		safestrncpy(a->userid, fields[2], sizeof(a->userid));
		safestrncpy(a->pass, fields[3], sizeof(a->pass));
		safestrncpy(a->lastlogin, fields[4], sizeof(a->lastlogin));
		a->sex = fields[5][0];
		a->logincount = strtol(fields[6], NULL, 10);
		a->state = strtoul(fields[7], NULL, 10);
		safestrncpy(a->email, fields[8], sizeof(a->email));
		//safestrncpy(a->error_message, fields[9], sizeof(a->error_message));
		a->expiration_time = strtol(fields[10], NULL, 10);
		safestrncpy(a->last_ip, fields[11], sizeof(a->last_ip));
		//safestrncpy(a->memo, fields[12], sizeof(a->memo));
		a->unban_time = strtol(fields[13], NULL, 10);
		regs = fields[14];
	}
	else
	if( version == 0 && count == 13 )
	{
		a->account_id = strtol(fields[1], NULL, 10);
		safestrncpy(a->userid, fields[2], sizeof(a->userid));
		safestrncpy(a->pass, fields[3], sizeof(a->pass));
		safestrncpy(a->lastlogin, fields[4], sizeof(a->lastlogin));
		a->sex = fields[5][0];
		a->logincount = strtol(fields[6], NULL, 10);
		a->state = strtoul(fields[7], NULL, 10);
		safestrncpy(a->email, fields[8], sizeof(a->email));
		//safestrncpy(a->error_message, fields[9], sizeof(a->error_message));
		a->expiration_time = strtol(fields[10], NULL, 10);
		safestrncpy(a->last_ip, fields[11], sizeof(a->last_ip));
		//safestrncpy(a->memo, fields[12], sizeof(a->memo));
		regs = fields[13];
	}
	else
	if( version == 0 && count == 8 )
	{
		a->account_id = strtol(fields[1], NULL, 10);
		safestrncpy(a->userid, fields[2], sizeof(a->userid));
		safestrncpy(a->pass, fields[3], sizeof(a->pass));
		safestrncpy(a->lastlogin, fields[4], sizeof(a->lastlogin));
		a->sex = fields[5][0];
		a->logincount = strtol(fields[6], NULL, 10);
		a->state = strtoul(fields[7], NULL, 10);
		regs = fields[8];
	}
	else
	{// unmatched row
		aFree(a);
		return NULL;
	}

	// extract account regs
	// {reg name<COMMA>reg value<SPACE>}*
	n = 0;
	for( i = 0; i < ACCOUNT_REG2_NUM; ++i )
	{
		char key[32];
		char value[256];
	
		regs += n;

		if (sscanf(regs, "%31[^\t,],%255[^\t ] %n", key, value, &n) != 2)
		{
			// We must check if a str is void. If it's, we can continue to read other REG2.
			// Account line will have something like: str2,9 ,9 str3,1 (here, ,9 is not good)
			if (regs[0] == ',' && sscanf(regs, ",%[^\t ] %n", value, &n) == 1) { 
				i--;
				continue;
			} else
				break;
		}
		
		safestrncpy(a->account_reg2[i].str, key, 32);
		safestrncpy(a->account_reg2[i].value, value, 256);
	}
	a->account_reg2_num = i;

	return a;
}

/// dump the contents of the account data into the provided string buffer
static void mmo_auth_tostr(char* str, const struct mmo_account* a)
{
	int i;
	char* str_p = str;

	str_p += sprintf(str_p, "%d\t%s\t%s\t%c\t%s\t%u\t%u\t%ld\t%ld\t%d\t%s\t%s\t",
	                 a->account_id, a->userid, a->pass, a->sex, a->email, a->level,
	                 a->state, (long)a->unban_time, (long)a->expiration_time,
	                 a->logincount, a->lastlogin, a->last_ip);

	for( i = 0; i < a->account_reg2_num; ++i )
		if( a->account_reg2[i].str[0] )
			str_p += sprintf(str_p, "%s,%s ", a->account_reg2[i].str, a->account_reg2[i].value);
}

/// dump the entire account db to disk
static void mmo_auth_sync(AccountDB_TXT* db)
{
	int lock;
	FILE *fp;
	struct DBIterator* iter;
	struct mmo_account* acc;

	fp = lock_fopen(account_txt, &lock);
	if( fp == NULL )
	{
		return;
	}

	fprintf(fp, "%d\n", 20080409); // savefile version

	fprintf(fp, "// Accounts file: here are saved all information about the accounts.\n");
	fprintf(fp, "// Structure: ID, account name, password, last login time, sex, # of logins, state, email, error message for state 7, validity time, last (accepted) login ip, memo field, ban timestamp, repeated(register text, register value)\n");
	fprintf(fp, "// Some explanations:\n");
	fprintf(fp, "//   account name    : between 4 to 23 char for a normal account (standard client can't send less than 4 char).\n");
	fprintf(fp, "//   account password: between 4 to 23 char\n");
	fprintf(fp, "//   sex             : M or F for normal accounts, S for server accounts\n");
	fprintf(fp, "//   state           : 0: account is ok, 1 to 256: error code of packet 0x006a + 1\n");
	fprintf(fp, "//   email           : between 3 to 39 char (a@a.com is like no email)\n");
	fprintf(fp, "//   error message   : text for the state 7: 'Your are Prohibited to login until <text>'. Max 19 char\n");
	fprintf(fp, "//   valitidy time   : 0: unlimited account, <other value>: date calculated by addition of 1/1/1970 + value (number of seconds since the 1/1/1970)\n");
	fprintf(fp, "//   memo field      : max 254 char\n");
	fprintf(fp, "//   ban time        : 0: no ban, <other value>: banned until the date: date calculated by addition of 1/1/1970 + value (number of seconds since the 1/1/1970)\n");

	//TODO: sort?

	iter = db->accounts->iterator(db->accounts);
	for( acc = (struct mmo_account*)iter->first(iter,NULL); iter->exists(iter); acc = (struct mmo_account*)iter->next(iter,NULL) )
	{
		char buf[2048]; // ought to be big enough ^^
		mmo_auth_tostr(buf, acc);
		fprintf(fp, "%s\n", buf);
	}
	fprintf(fp, "%d\t%%newid%%\n", db->next_account_id);
	iter->destroy(iter);

	lock_fclose(fp, account_txt, &lock);

	// reset save counter
	db->auths_before_save = AUTHS_BEFORE_SAVE;
}

static int mmo_auth_sync_timer(int tid, unsigned int tick, int id, int data)
{
	AccountDB_TXT* db = (AccountDB_TXT*)data;

	if( db->auths_before_save < AUTHS_BEFORE_SAVE )
		mmo_auth_sync(db); // db was modified, flush it

	return 0;
}