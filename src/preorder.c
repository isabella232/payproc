/* preorder.c - Access to the preorder database.
 * Copyright (C) 2015 g10 Code GmbH
 *
 * This file is part of Payproc.
 *
 * Payproc is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Payproc is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

/* The Database used for preorders is pretty simple:  Just a single table:

   CREATE TABLE preorder (
     ref   TEXT NOT NULL PRIMARY KEY,  -- The "ABCDE" part of ABCDE-NN.
     refnn INTEGER NOT NULL,           -- The "NN"    part of ABCDE-NN
     created TEXT NOT NULL,            -- Timestamp
     paid TEXT,                        -- Timestamp of last payment
     npaid INTEGER NOT NULL,           -- Total number of payments
     amount TEXT NOT NULL,             -- with decimal digit; thus TEXT.
     currency TEXT NOT NULL,
     desc TEXT,   -- Description of the order
     email TEXT,  -- Optional mail address.
     meta TEXT    -- Using the format from the journal.
   )


  Expiring entries can be done using

     DELETE from preorder
     WHERE julianday(created) < julianday('now', '-30 days')
           AND paid IS NULL;

  this has not been implemented here but should be done at startup and
  once a day.

 */

#include <config.h>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <npth.h>
#include <gcrypt.h>
#include <sqlite3.h>

#include "util.h"
#include "logging.h"
#include "payprocd.h"
#include "journal.h"  /* Temporary for meta_field_to_string.  */
#include "preorder.h"


#define DB_DATETIME_SIZE 20 /* "1970-01-01 12:00:00" */


/* The name of the preorder database file.  */
static const char preorder_db_fname[] = "/var/lib/payproc/preorder.db";

/* The database handle used for the preorder database.  This handle
   may only used after a successful open_preorder_db call and not
   after a close_preorder_db call.  The lock variable is maintained by
   the mentioned open and close functions. */
static sqlite3 *preorder_db;
static npth_mutex_t preorder_db_lock = NPTH_MUTEX_INITIALIZER;

/* This is a prepared statement for the INSERT operation.  It is
   protected by preorder_db_lock.  */
static sqlite3_stmt *preorder_insert_stmt;




/* Create a SEPA-Ref field and store it in BUFFER.  The format is:

     AAAAA-NN

  with AAAAA being uppercase letters or digits and NN a value between
  10 and 99.  Thus the entire length of the returned string is 8.  We
  use a base 28 alphabet for the A values with the first A restricted
  to a letter.  Some letters are left out because they might be
  misrepresented due to OCR scanning.  There are about 11 million
  different values for AAAAA. */
static void
make_sepa_ref (char *buffer, size_t bufsize)
{
  static char codes[28] = { 'A', 'B', 'C', 'D', 'E', 'G', 'H', 'J',
                            'K', 'L', 'N', 'R', 'S', 'T', 'W', 'X',
                            'Y', 'Z', '0', '1', '2', '3', '4', '5',
                            '6', '7', '8', '9' };
  unsigned char nonce[5];
  int i;
  unsigned int n;

  if (bufsize < 9)
    BUG ();

  gcry_create_nonce (nonce, sizeof nonce);
  buffer[0] = codes[nonce[0] % 18];
  for (i=1; i < 5; i++)
    buffer[i] = codes[nonce[i] % 28];
  buffer[5] = '-';
  n = (((unsigned int)nonce[0] << 24) | (nonce[1] << 16)
       | (nonce[2] << 8) | nonce[3]);
  i = 10 + (n % 90);
  buffer [6] = '0' + i / 10;
  buffer [7] = '0' + i % 10;
  buffer [8] = 0;
}


/* Given a buffer of size DB_DATETIME_SIZE put the current time into it.  */
static char *
db_datetime_now (char *buffer)
{
#if DB_DATETIME_SIZE != TIMESTAMP_SIZE + 4
# error mismatching timestamp sizes
#endif
  get_current_time (buffer);
  /* "19700101T120000" to
     "1970-01-01 12:00:00" */
  buffer[19] = 0;
  buffer[18] = buffer[14];
  buffer[17] = buffer[13];
  buffer[16] = ':';
  buffer[15] = buffer[12];
  buffer[14] = buffer[11];
  buffer[13] = ':';
  buffer[12] = buffer[10];
  buffer[11] = buffer[9];
  buffer[10] = ' ';
  buffer[9] = buffer[7];
  buffer[8] = buffer[6];
  buffer[7] = '-';
  buffer[6] = buffer[5];
  buffer[5] = buffer[4];
  buffer[4] = '-';

  return buffer;
}




/* Relinquishes the lock on the database handle and if DO_CLOSE is
   true also close the database handle.  Note that we usually keep the
   database open for the lifetime of the process.  */
static void
close_preorder_db (int do_close)
{
  int res;

  if (do_close && preorder_db)
    {
      res = sqlite3_close (preorder_db);
      if (res == SQLITE_BUSY)
        {
          sqlite3_finalize (preorder_insert_stmt);
          preorder_insert_stmt = NULL;
          res = sqlite3_close (preorder_db);
        }
      if (res)
        log_error ("failed to close the preorder db: %s\n",
                   sqlite3_errstr (res));
      preorder_db = NULL;
    }

  res = npth_mutex_unlock (&preorder_db_lock);
  if (res)
    log_fatal ("failed to release preorder db lock: %s\n",
               gpg_strerror (gpg_error_from_errno (res)));
}


/* This function opens or creates the preorder database.  If the
   database is already open it merly takes a lock ion the handle. */
static gpg_error_t
open_preorder_db (void)
{
  int res;
  sqlite3_stmt *stmt;

  res = npth_mutex_lock (&preorder_db_lock);
  if (res)
    log_fatal ("failed to acquire preorder db lock: %s\n",
               gpg_strerror (gpg_error_from_errno (res)));
  if (preorder_db)
    return 0; /* Good: Already open.  */

  /* Database has not yet been opened.  Open or create it, make sure
     the tables exist, and prepare the required statements.  We use
     our own locking instead of the more complex serialization sqlite
     would have to do. */

  res = sqlite3_open_v2 (preorder_db_fname,
                         &preorder_db,
                         (SQLITE_OPEN_READWRITE
                          | SQLITE_OPEN_CREATE
                          | SQLITE_OPEN_NOMUTEX),
                         NULL);
  if (res)
    {
      log_error ("error opening '%s': %s\n",
                 preorder_db_fname, sqlite3_errstr (res));
      close_preorder_db (1);
      return gpg_error (GPG_ERR_GENERAL);
    }
  sqlite3_extended_result_codes (preorder_db, 1);


  /* Create the tables if needed.  */
  res = sqlite3_prepare_v2 (preorder_db,
                            "CREATE TABLE IF NOT EXISTS preorder ("
                            "ref      TEXT NOT NULL PRIMARY KEY,"
                            "refnn    INTEGER NOT NULL,"
                            "created  TEXT NOT NULL,"
                            "paid TEXT,"
                            "npaid INTEGER NOT NULL,"
                            "amount   TEXT NOT NULL,"
                            "currency TEXT NOT NULL,"
                            "desc     TEXT,"
                            "email    TEXT,"
                            "meta     TEXT"
                            ")",
                            -1, &stmt, NULL);
  if (res)
    {
      log_error ("error creating preorder table (prepare): %s\n",
                 sqlite3_errstr (res));
      close_preorder_db (1);
      return gpg_error (GPG_ERR_GENERAL);
    }

  res = sqlite3_step (stmt);
  sqlite3_finalize (stmt);
  if (res != SQLITE_DONE)
    {
      log_error ("error creating preorder table: %s\n", sqlite3_errstr (res));
      close_preorder_db (1);
      return gpg_error (GPG_ERR_GENERAL);
    }

  /* Prepare an insert statement.  */
  res = sqlite3_prepare_v2 (preorder_db,
                            "INSERT INTO preorder VALUES ("
                            "?1,?2,?3,NULL,0,?4,?5,?6,?7,?8"
                            ")",
                            -1, &stmt, NULL);
  if (res)
    {
      log_error ("error preparing insert statement: %s\n",
                 sqlite3_errstr (res));
      close_preorder_db (1);
      return gpg_error (GPG_ERR_GENERAL);
    }
  preorder_insert_stmt = stmt;

  return 0;
}


/* Insert a record into the preorder table.  The values are taken from
   the dictionary at DICTP.  On return a SEPA-Ref value will have been
   inserted into it; that may happen even on error.  */
static gpg_error_t
insert_preorder_record (keyvalue_t *dictp)
{
  gpg_error_t err;
  int res;
  keyvalue_t dict = *dictp;
  char separef[9];
  char *buf;
  char datetime_buf [DB_DATETIME_SIZE];
  int retrycount = 0;

 retry:
  make_sepa_ref (separef, sizeof separef);
  err = keyvalue_put (dictp, "SEPA-Ref", separef);
  if (err)
    return err;
  dict = *dictp;

  sqlite3_reset (preorder_insert_stmt);

  separef[5] = 0;
  res = sqlite3_bind_text (preorder_insert_stmt,
                           1, separef, -1, SQLITE_TRANSIENT);
  if (!res)
    res = sqlite3_bind_int (preorder_insert_stmt,
                            2, atoi (separef + 6));
  if (!res)
    res = sqlite3_bind_text (preorder_insert_stmt,
                             3, db_datetime_now (datetime_buf),
                             -1, SQLITE_TRANSIENT);
  if (!res)
    res = sqlite3_bind_text (preorder_insert_stmt,
                             4, keyvalue_get_string (dict, "Amount"),
                             -1, SQLITE_TRANSIENT);
  if (!res)
    res = sqlite3_bind_text (preorder_insert_stmt,
                             5, "EUR", -1, SQLITE_STATIC);
  if (!res)
    res = sqlite3_bind_text (preorder_insert_stmt,
                             6, keyvalue_get (dict, "Desc"),
                             -1, SQLITE_TRANSIENT);
  if (!res)
    res = sqlite3_bind_text (preorder_insert_stmt,
                             7, keyvalue_get (dict, "Email"),
                             -1, SQLITE_TRANSIENT);
  if (!res)
    {
      buf = meta_field_to_string (dict);
      if (!buf)
        res = sqlite3_bind_null (preorder_insert_stmt, 8);
      else
        res = sqlite3_bind_text (preorder_insert_stmt, 8, buf, -1, es_free);
    }

  if (res)
    {
      log_error ("error binding a value for the preorder table: %s\n",
                 sqlite3_errstr (res));
      return gpg_error (GPG_ERR_GENERAL);
    }

  res = sqlite3_step (preorder_insert_stmt);
  if (res == SQLITE_DONE)
    return 0;

  /* In case we hit the same primary key we need to retry.  This is
     limited to 11000 retries (~0.1% of the primary key space).  */
  if (res == SQLITE_CONSTRAINT_PRIMARYKEY && ++retrycount < 11000)
    goto retry;

  log_error ("error inserting into preorder table: %s (%d)\n",
             sqlite3_errstr (res), res);
  return gpg_error (GPG_ERR_GENERAL);
}




/* Create a new preorder record and store it.  Inserts a "SEPA-Ref"
   into DICT.  */
gpg_error_t
preorder_store_record (keyvalue_t *dictp)
{
  gpg_error_t err;

  err = open_preorder_db ();
  if (err)
    return err;

  err = insert_preorder_record (dictp);

  close_preorder_db (0);

  return err;
}