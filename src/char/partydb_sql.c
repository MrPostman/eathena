// Copyright (c) Athena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#include "../common/malloc.h"
#include "../common/mapindex.h"
#include "../common/mmo.h"
#include "../common/showmsg.h"
#include "../common/strlib.h"
#include "charserverdb_sql.h"
#include "inter.h" // char_db[]
#include "partydb.h"
#include <stdlib.h>
#include <string.h>


/// internal structure
typedef struct PartyDB_SQL
{
	PartyDB vtable;    // public interface

	CharServerDB_SQL* owner;
	Sql* parties;      // SQL party storage

	// other settings
	bool case_sensitive;
	const char* char_db;
	const char* party_db;

} PartyDB_SQL;



static bool mmo_party_fromsql(PartyDB_SQL* db, struct party* p, int party_id)
{
	Sql* sql_handle = db->parties;
	char* data;
	int i;
	int leader_id, leader_char;

	memset(p, 0, sizeof(*p));

	// retrieve entry for the specified party
	if( SQL_ERROR == Sql_Query(sql_handle,
	    "SELECT `party_id`,`name`,`exp`,`item`,`leader_id`,`leader_char` FROM `%s` WHERE `party_id` = %d",
		db->party_db, party_id )
	) {
		Sql_ShowDebug(sql_handle);
		return false;
	}

	if( SQL_SUCCESS != Sql_NextRow(sql_handle) )
	{// no such entry
		Sql_FreeResult(sql_handle);
		return false;
	}

	Sql_GetData(sql_handle,  0, &data, NULL); p->party_id = atoi(data);
	Sql_GetData(sql_handle,  1, &data, NULL); safestrncpy(p->name, data, sizeof(p->name));
	Sql_GetData(sql_handle,  2, &data, NULL); p->exp = atoi(data);
	Sql_GetData(sql_handle,  3, &data, NULL); p->item = atoi(data);
	Sql_GetData(sql_handle,  4, &data, NULL); leader_id = atoi(data);
	Sql_GetData(sql_handle,  5, &data, NULL); leader_char = atoi(data);

	Sql_FreeResult(sql_handle);

	// load members
	if( SQL_ERROR == Sql_Query(sql_handle,
	    "SELECT `account_id`,`char_id` FROM `%s` WHERE `party_id`='%d'",
	    db->char_db, party_id)
	) {
		Sql_ShowDebug(sql_handle);
		return false;
	}

	for( i = 0; i < MAX_PARTY && SQL_SUCCESS == Sql_NextRow(sql_handle); ++i )
	{
		struct party_member* m = &p->member[i];
		Sql_GetData(sql_handle, 0, &data, NULL); m->account_id = atoi(data);
		Sql_GetData(sql_handle, 1, &data, NULL); m->char_id = atoi(data);
		m->leader = (m->account_id == leader_id && m->char_id == leader_char) ? 1 : 0;
	}

	Sql_FreeResult(sql_handle);

	return true;
}


static bool mmo_party_tosql(PartyDB_SQL* db, struct party* p, enum party_save_flags flag, int index)
{
	Sql* sql_handle = db->parties;
	SqlStmt* stmt = NULL;
	bool result = false;

	if( p == NULL || p->party_id == 0 )
		return result;

	if( SQL_SUCCESS != Sql_QueryStr(sql_handle, "START TRANSACTION") )
	{
		Sql_ShowDebug(sql_handle);
		return result;
	}

	// try
	do
	{

	if( flag & PS_CREATE )
	{// Create party
		int insert_id;
		unsigned char exp = p->exp;
		unsigned char item = p->item;

		stmt = SqlStmt_Malloc(sql_handle);
		if( SQL_SUCCESS != SqlStmt_Prepare(stmt, "INSERT INTO `%s` (`party_id`, `name`, `exp`, `item`, `leader_id`, `leader_char`) VALUES (?,?,?,?,?,?)", db->party_db)
		||  SQL_SUCCESS != SqlStmt_BindParam(stmt, 0, (p->party_id != -1)?SQLDT_INT:SQLDT_NULL, (void*)&p->party_id, 0)
		||  SQL_SUCCESS != SqlStmt_BindParam(stmt, 1, SQLDT_STRING, (void*)p->name, strnlen(p->name, sizeof(p->name)))
		||  SQL_SUCCESS != SqlStmt_BindParam(stmt, 2, SQLDT_UCHAR, (void*)&exp, 0)
		||  SQL_SUCCESS != SqlStmt_BindParam(stmt, 3, SQLDT_UCHAR, (void*)&item, 0)
		||  SQL_SUCCESS != SqlStmt_BindParam(stmt, 4, SQLDT_INT, (void*)&p->member[index].account_id, 0)
		||  SQL_SUCCESS != SqlStmt_BindParam(stmt, 5, SQLDT_INT, (void*)&p->member[index].char_id, 0)
		||  SQL_SUCCESS != SqlStmt_Execute(stmt)
		) {
			SqlStmt_ShowDebug(stmt);
			break;
		}

		insert_id = (int)SqlStmt_LastInsertId(stmt);
		if( p->party_id == -1 )
			p->party_id = insert_id; // fill in output value
		else
		if( p->party_id != insert_id )
			break; // error, unexpected value
	}

	if( flag & PS_BASIC )
	{// Update party info.
		char esc_name[NAME_LENGTH*2+1];// escaped party name
		Sql_EscapeStringLen(sql_handle, esc_name, p->name, safestrnlen(p->name, NAME_LENGTH));

		if( SQL_ERROR == Sql_Query(sql_handle, "UPDATE `%s` SET `name`='%s', `exp`='%d', `item`='%d' WHERE `party_id`='%d'",
			db->party_db, esc_name, p->exp, p->item, p->party_id) )
		{
			Sql_ShowDebug(sql_handle);
			break;
		}
	}

	if( flag & PS_LEADER )
	{// Update leader
		if( SQL_ERROR == Sql_Query(sql_handle, "UPDATE `%s`  SET `leader_id`='%d', `leader_char`='%d' WHERE `party_id`='%d'",
			db->party_db, p->member[index].account_id, p->member[index].char_id, p->party_id) )
		{
			Sql_ShowDebug(sql_handle);
			break;
		}
	}
	
	if( flag & PS_ADDMEMBER )
	{// Add one party member.
		if( SQL_ERROR == Sql_Query(sql_handle, "UPDATE `%s` SET `party_id`='%d' WHERE `account_id`='%d' AND `char_id`='%d'",
			db->char_db, p->party_id, p->member[index].account_id, p->member[index].char_id) )
		{
			Sql_ShowDebug(sql_handle);
			break;
		}
	}

	if( flag & PS_DELMEMBER )
	{// Remove one party member.
		if( SQL_ERROR == Sql_Query(sql_handle, "UPDATE `%s` SET `party_id`='0' WHERE `party_id`='%d' AND `account_id`='%d' AND `char_id`='%d'",
			db->char_db, p->party_id, p->member[index].account_id, p->member[index].char_id) )
		{
			Sql_ShowDebug(sql_handle);
			break;
		}
	}

	// if we got this far, everything was successful
	result = true;

	} while(0);
	// finally

	SqlStmt_Free(stmt);

	if( SQL_SUCCESS != Sql_QueryStr(sql_handle, (result == true) ? "COMMIT" : "ROLLBACK") )
	{
		Sql_ShowDebug(sql_handle);
		result = false;
	}

	return result;
}


static bool party_db_sql_init(PartyDB* self)
{
	PartyDB_SQL* db = (PartyDB_SQL*)self;
	db->parties = db->owner->sql_handle;
	return true;
}

static void party_db_sql_destroy(PartyDB* self)
{
	PartyDB_SQL* db = (PartyDB_SQL*)self;
	db->parties = NULL;
	aFree(db);
}

static bool party_db_sql_sync(PartyDB* self)
{
	return true;
}

static bool party_db_sql_create(PartyDB* self, struct party_data* p)
{
	PartyDB_SQL* db = (PartyDB_SQL*)self;
	return mmo_party_tosql(db, &p->party, PS_CREATE|PS_ADDMEMBER, 0);
}

static bool party_db_sql_remove(PartyDB* self, const int party_id)
{
	PartyDB_SQL* db = (PartyDB_SQL*)self;
	Sql* sql_handle = db->parties;
	bool result = false;

	if( SQL_SUCCESS != Sql_QueryStr(sql_handle, "START TRANSACTION") )
	{
		Sql_ShowDebug(sql_handle);
		return result;
	}

	// try
	do
	{

	if( SQL_SUCCESS != Sql_Query(sql_handle, "UPDATE `%s` SET `party_id`='0' WHERE `party_id`='%d'", db->char_db, party_id)
	||  SQL_SUCCESS != Sql_Query(sql_handle, "DELETE FROM `%s` WHERE `party_id`='%d'", db->party_db, party_id)
	) {
		Sql_ShowDebug(sql_handle);
		break;
	}
	
	// success
	result = true;

	}
	while(0);
	// finally

	if( SQL_SUCCESS != Sql_QueryStr(sql_handle, (result == true) ? "COMMIT" : "ROLLBACK") )
	{
		Sql_ShowDebug(sql_handle);
		result = false;
	}

	return result;
}

static bool party_db_sql_save(PartyDB* self, const struct party_data* p, enum party_save_flags flag, int index)
{
	PartyDB_SQL* db = (PartyDB_SQL*)self;
	return mmo_party_tosql(db, (struct party*)&p->party, flag, index);
}

static bool party_db_sql_load(PartyDB* self, struct party_data* p, int party_id)
{
	PartyDB_SQL* db = (PartyDB_SQL*)self;

	if( !mmo_party_fromsql(db, &p->party, party_id) )
		return false;

	return true;
}

static bool party_db_sql_name2id(PartyDB* self, int* party_id, const char* name)
{
	PartyDB_SQL* db = (PartyDB_SQL*)self;
	Sql* sql_handle = db->parties;
	char esc_name[2*NAME_LENGTH+1];
	char* data;

	Sql_EscapeString(sql_handle, esc_name, name);

	// get the list of party IDs for this party name
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `party_id` FROM `%s` WHERE `name`= %s '%s'",
		db->party_db, (db->case_sensitive ? "BINARY" : ""), esc_name) )
	{
		Sql_ShowDebug(sql_handle);
		return false;
	}

	if( Sql_NumRows(sql_handle) > 1 )
	{// serious problem - duplicit party name
		ShowError("party_db_sql_load_str: multiple parties found when retrieving data for party '%s'!\n", name);
		Sql_FreeResult(sql_handle);
		return false;
	}

	if( SQL_SUCCESS != Sql_NextRow(sql_handle) )
	{// no such entry
		Sql_FreeResult(sql_handle);
		return false;
	}

	Sql_GetData(sql_handle, 0, &data, NULL);
	if( party_id != NULL )
		*party_id = atoi(data);
	Sql_FreeResult(sql_handle);

	return true;
}


/// Returns an iterator over all parties.
static CSDBIterator* party_db_sql_iterator(PartyDB* self)
{
	PartyDB_SQL* db = (PartyDB_SQL*)self;
	return csdb_sql_iterator(db->parties, db->party_db, "party_id");
}


/// public constructor
PartyDB* party_db_sql(CharServerDB_SQL* owner)
{
	PartyDB_SQL* db = (PartyDB_SQL*)aCalloc(1, sizeof(PartyDB_SQL));

	// set up the vtable
	db->vtable.init      = &party_db_sql_init;
	db->vtable.destroy   = &party_db_sql_destroy;
	db->vtable.sync      = &party_db_sql_sync;
	db->vtable.create    = &party_db_sql_create;
	db->vtable.remove    = &party_db_sql_remove;
	db->vtable.save      = &party_db_sql_save;
	db->vtable.load      = &party_db_sql_load;
	db->vtable.name2id   = &party_db_sql_name2id;
	db->vtable.iterator  = &party_db_sql_iterator;

	// initialize to default values
	db->owner = owner;
	db->parties = NULL;

	// other settings
	db->case_sensitive = false;
	db->char_db = db->owner->table_chars;
	db->party_db = db->owner->table_parties;

	return &db->vtable;
}