/* util.c - Genereal utility functions.
 * Copyright (C) 1998, 1999, 2000, 2001, 2003, 2004, 2005, 2006, 2007,
 *               2008, 2009, 2010  Free Software Foundation, Inc.
 * Copyright (C) 2014 g10 Code GmbH
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

#include <config.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdarg.h>
#include <errno.h>
#include <ctype.h>

#include "util.h"

/* The error source number for Payproc.  */
gpg_err_source_t default_errsource;



static void
out_of_core(void)
{
  fputs ("\nfatal: out of memory\n", stderr);
  exit (2);
}


void *
xmalloc( size_t n )
{
  void *p = malloc( n );
  if (!p)
    out_of_core ();
  return p;
}


void *
xrealloc (void *a, size_t n)
{
  void *p = realloc (a, n);
  if (!p)
    out_of_core();
  return p;
}


void *
xcalloc (size_t n, size_t m)
{
  void *p = calloc( n, m );
  if (!p)
    out_of_core ();
  return p;
}


char *
xstrdup (const char *string)
{
  void *p = xmalloc (strlen(string)+1);
  strcpy (p, string);
  return p;
}



static char *
do_strconcat (const char *s1, va_list arg_ptr)
{
  const char *argv[48];
  size_t argc;
  size_t needed;
  char *buffer, *p;

  argc = 0;
  argv[argc++] = s1;
  needed = strlen (s1);
  while (((argv[argc] = va_arg (arg_ptr, const char *))))
    {
      needed += strlen (argv[argc]);
      if (argc >= DIM (argv)-1)
        {
          gpg_err_set_errno (EINVAL);
          return NULL;
        }
      argc++;
    }
  needed++;
  buffer = xtrymalloc (needed);
  if (buffer)
    {
      for (p = buffer, argc=0; argv[argc]; argc++)
        p = stpcpy (p, argv[argc]);
    }
  return buffer;
}


/* Concatenate the string S1 with all the following strings up to a
   NULL.  Returns a malloced buffer with the new string or NULL on a
   malloc error or if too many arguments are given.  */
char *
strconcat (const char *s1, ...)
{
  va_list arg_ptr;
  char *result;

  if (!s1)
    result = xtrystrdup ("");
  else
    {
      va_start (arg_ptr, s1);
      result = do_strconcat (s1, arg_ptr);
      va_end (arg_ptr);
    }
  return result;
}



/*
 * Check whether STRING starts with KEYWORD.  The keyword is
 * delimited by end of string, a space or a tab.  Returns NULL if not
 * found or a pointer into STRING to the next non-space character
 * after the KEYWORD (which may be end of string).
 */
char *
has_leading_keyword (const char *string, const char *keyword)
{
  size_t n = strlen (keyword);

  if (!strncmp (string, keyword, n)
      && (!string[n] || string[n] == ' ' || string[n] == '\t'))
    {
      string += n;
      while (*string == ' ' || *string == '\t')
        string++;
      return (char*)string;
    }
  return NULL;
}


/* Find string SUB in (BUFFER,BUFLEN).  */
const char *
memstr (const void *buffer, size_t buflen, const char *sub)
{
  const char *buf = buffer;
  const char *t = buf;
  const char *s = sub;
  size_t n = buflen;

  for (; n; t++, n--)
    {
      if (*t == *s)
        {
          for (buf = t++, buflen = n--, s++; n && *t == *s; t++, s++, n--)
            ;
          if (!*s)
            return buf;
          t = buf;
          s = sub ;
          n = buflen;
	}
    }
  return NULL;
}


/* Find string SUB in (BUFFER,BUFLEN).
 * Comparison is case-insensitive.  */
const char *
memistr (const void *buffer, size_t buflen, const char *sub)
{
  const unsigned char *buf = buffer;
  const unsigned char *t = (const unsigned char *)buffer;
  const unsigned char *s = (const unsigned char *)sub;
  size_t n = buflen;

  for ( ; n ; t++, n-- )
    {
      if ( toupper (*t) == toupper (*s) )
        {
          for ( buf=t++, buflen = n--, s++;
                n && toupper (*t) == toupper (*s); t++, s++, n-- )
            ;
          if (!*s)
            return (const char*)buf;
          t = buf;
          s = (const unsigned char *)sub ;
          n = buflen;
	}
    }
  return NULL;
}


int
memicmp (const char *a, const char *b, size_t n)
{
  for ( ; n; n--, a++, b++ )
    if (*a != *b && (toupper (*(const unsigned char*)a)
                     != toupper(*(const unsigned char*)b)))
      return *(const unsigned char *)a - *(const unsigned char*)b;
  return 0;
}


/*
 * Remove leading and trailing white space from STR.  Return STR.
 */
char *
trim_spaces (char *str)
{
  char *string, *p, *mark;

  string = str;
  /* Find first non space character.  */
  for( p=string; *p && isspace (*(unsigned char*)p) ; p++ )
    ;
  /* Move characters. */
  for (mark = NULL; (*string = *p); string++, p++ )
    {
      if (isspace (*(unsigned char*)p))
        {
          if (!mark)
            mark = string;
        }
      else
        mark = NULL;
    }
  if (mark)
    *mark = '\0' ;  /* Remove trailing spaces. */

  return str;
}



keyvalue_t
keyvalue_find (keyvalue_t list, const char *key)
{
  keyvalue_t kv;

  for (kv = list; kv; kv = kv->next)
    if (!strcmp (kv->name, key))
      return kv;
  return NULL;
}

static keyvalue_t
keyvalue_create (const char *key, const char *value)
{
  keyvalue_t kv;

  /* Insert a new data item. */
  kv = xtrymalloc (sizeof *kv + strlen (key));
  if (!kv)
    return NULL;
  kv->next = NULL;
  strcpy (kv->name, key);
  kv->value = xtrystrdup (value);
  if (!kv->value)
    {
      xfree (kv);
      return NULL;
    }
  return kv;
}


/* Append the string VALUE to the current value of KV.  */
gpg_error_t
keyvalue_append_with_nl (keyvalue_t kv, const char *value)
{
  char *p;

  p = strconcat (kv->value, "\n", value, NULL);
  if (!p)
    return gpg_err_code_from_syserror ();
  xfree (kv->value);
  kv->value = p;
  return 0;
}


/* Remove all newlines from the value of KV.  This is done in place
   and and works always.  */
void
keyvalue_remove_nl (keyvalue_t kv)
{
  char *s, *d;

  if (!kv || !kv->value)
    return;
  for (s = d = kv->value; *s; s++)
    if (*s != '\n')
      *d++ = *s;
  *d = 0;
}


gpg_error_t
keyvalue_put (keyvalue_t *list, const char *key, const char *value)
{
  keyvalue_t kv;
  char *buf;

  if (!key || !*key)
    return gpg_error (GPG_ERR_INV_VALUE);

  kv = keyvalue_find (*list, key);
  if (kv) /* Update.  */
    {
      if (value)
        {
          buf = xtrystrdup (value);
          if (!buf)
            return gpg_error_from_syserror ();
        }
      else
        buf = NULL;
      xfree (kv->value);
      kv->value = buf;
    }
  else if (value) /* Insert.  */
    {
      kv = keyvalue_create (key, value);
      if (!kv)
        return gpg_error_from_syserror ();
      kv->next = *list;
      *list = kv;
    }
  return 0;
}


gpg_error_t
keyvalue_del (keyvalue_t list, const char *key)
{
  /* LIST won't change due to the del operation.  */
  return keyvalue_put (&list, key, NULL);
}



gpg_error_t
keyvalue_putf (keyvalue_t *list, const char *key, const char *format, ...)
{
  gpg_error_t err;
  va_list arg_ptr;
  char *value;

  if (!key || !*key)
    return gpg_error (GPG_ERR_INV_VALUE);

  va_start (arg_ptr, format);
  value = gpgrt_vbsprintf (format, arg_ptr);
  va_end (arg_ptr);
  if (!value)
    return gpg_error_from_syserror ();

  err = keyvalue_put (list, key, value);
  if (err)
    es_free (value);
  return err;
}


void
keyvalue_release (keyvalue_t kv)
{
  while (kv)
    {
      keyvalue_t nxt = kv->next;
      xfree (kv->value);
      kv = nxt;
    }
}


const char *
keyvalue_get (keyvalue_t list, const char *key)
{
  keyvalue_t kv;

  for (kv = list; kv; kv = kv->next)
    if (!strcmp (kv->name, key))
      return kv->value;
  return NULL;
}


/* Same as keyvalue_get but return the value as a modifiable string
   and the value in LIST to NULL.  The caller must xfree the
   result.  */
char *
keyvalue_snatch (keyvalue_t list, const char *key)
{
  keyvalue_t kv;

  for (kv = list; kv; kv = kv->next)
    if (!strcmp (kv->name, key))
      {
        char *p = kv->value;
        kv->value = NULL;
        return p;
      }
  return NULL;
}


const char *
keyvalue_get_string (keyvalue_t list, const char *key)
{
  const char *s = keyvalue_get (list, key);
  return s? s: "";
}


int
keyvalue_get_int (keyvalue_t list, const char *key)
{
  const char *s = keyvalue_get (list, key);
  if (!s)
    return 0;
  return atoi (s);
}



/* Parse the www-form-urlencoded DATA into a new dictionary and store
   that dictionary at R_DICT.  On error store NULL at R_DICT and
   return an error code.  Note that STRING will be modified on
   return. */
gpg_error_t
parse_www_form_urlencoded (keyvalue_t *r_dict, const char *data)
{
  gpg_error_t err;
  char *string, *endp, *name, *value;
  size_t n;
  char *buffer = NULL;
  keyvalue_t dict = NULL;

  *r_dict = NULL;

  string = buffer = xtrystrdup (data);
  if (!string)
    {
      err = gpg_error_from_syserror ();
      goto leave;
    }

  do
    {
      endp = strchr (string, '&');
      if (endp)
        *endp = 0;

      name = string;
      value = strchr (name, '=');
      if (value)
        *value++ = 0;

      name[(n=percent_plus_unescape_inplace (name, 0))] = 0;
      if (!n || strlen (name) != n)
        {
          err = gpg_error (GPG_ERR_INV_VALUE); /* Nul in name or empty.  */
          goto leave;
        }

      if (value)
        {
          value[(n=percent_plus_unescape_inplace (value, 0))] = 0;
          if (strlen (value) != n)
            {
              err = gpg_error (GPG_ERR_INV_VALUE); /* Nul in value.  */
              goto leave;
            }
        }

      err = keyvalue_put (&dict, name, value? value:"");
      if (err)
        goto leave;

      if (endp)
        string = endp + 1;
    }
  while (endp);

  *r_dict = dict;
  dict = NULL;
  err = 0;

 leave:
  keyvalue_release (dict);
  xfree (buffer);
  return err;
}




/* Mapping table for zb32.  */
static char const zb32asc[32] = {'y','b','n','d','r','f','g','8',
                                 'e','j','k','m','c','p','q','x',
                                 'o','t','1','u','w','i','s','z',
                                 'a','3','4','5','h','7','6','9' };

/* If C is a valid ZB32 character return its index (0..31).  If it is
   not valid return -1.  */
int
zb32_index (int c)
{
  const char *p;

  p = memchr (zb32asc, c, 32);
  if (p)
    return p - zb32asc;
  if (c >= 'A' && c <= 'Z')
    {
      p = memchr (zb32asc, c - 'A' + 'a', 32);
      if (p)
        return p - zb32asc;
    }
  return -1;
}


/* Zooko's base32 variant. See RFC-6189 and
   http://philzimmermann.com/docs/human-oriented-base-32-encoding.txt
   Caller must xfree the returned string.  Returns NULL and sets ERRNO
   on error.  To avoid integer overflow DATALEN is limited to 2^16
   bytes.  Note, that DATABITS is measured in bits!.  */
char *
zb32_encode (const void *data, unsigned int databits)
{
  const unsigned char *s;
  char *output, *d;
  size_t datalen;

  datalen = (databits + 7) / 8;
  if (datalen > (1 << 16))
    {
      errno = EINVAL;
      return NULL;
    }

  d = output = xtrymalloc (8 * (datalen / 5)
                           + 2 * (datalen % 5)
                           - ((datalen%5)>2)
                           + 1);
  if (!output)
    return NULL;

  /* I use straightforward code.  The compiler should be able to do a
     better job on optimization than me and it is easier to read.  */
  for (s = data; datalen >= 5; s += 5, datalen -= 5)
    {
      *d++ = zb32asc[((s[0]      ) >> 3)               ];
      *d++ = zb32asc[((s[0] &   7) << 2) | (s[1] >> 6) ];
      *d++ = zb32asc[((s[1] &  63) >> 1)               ];
      *d++ = zb32asc[((s[1] &   1) << 4) | (s[2] >> 4) ];
      *d++ = zb32asc[((s[2] &  15) << 1) | (s[3] >> 7) ];
      *d++ = zb32asc[((s[3] & 127) >> 2)               ];
      *d++ = zb32asc[((s[3] &   3) << 3) | (s[4] >> 5) ];
      *d++ = zb32asc[((s[4] &  31)     )               ];
    }

  switch (datalen)
    {
    case 4:
      *d++ = zb32asc[((s[0]      ) >> 3)               ];
      *d++ = zb32asc[((s[0] &   7) << 2) | (s[1] >> 6) ];
      *d++ = zb32asc[((s[1] &  63) >> 1)               ];
      *d++ = zb32asc[((s[1] &   1) << 4) | (s[2] >> 4) ];
      *d++ = zb32asc[((s[2] &  15) << 1) | (s[3] >> 7) ];
      *d++ = zb32asc[((s[3] & 127) >> 2)               ];
      *d++ = zb32asc[((s[3] &   3) << 3)               ];
      break;
    case 3:
      *d++ = zb32asc[((s[0]      ) >> 3)               ];
      *d++ = zb32asc[((s[0] &   7) << 2) | (s[1] >> 6) ];
      *d++ = zb32asc[((s[1] &  63) >> 1)               ];
      *d++ = zb32asc[((s[1] &   1) << 4) | (s[2] >> 4) ];
      *d++ = zb32asc[((s[2] &  15) << 1)               ];
      break;
    case 2:
      *d++ = zb32asc[((s[0]      ) >> 3)               ];
      *d++ = zb32asc[((s[0] &   7) << 2) | (s[1] >> 6) ];
      *d++ = zb32asc[((s[1] &  63) >> 1)               ];
      *d++ = zb32asc[((s[1] &   1) << 4)               ];
      break;
    case 1:
      *d++ = zb32asc[((s[0]      ) >> 3)               ];
      *d++ = zb32asc[((s[0] &   7) << 2)               ];
      break;
    default:
      break;
    }
  *d = 0;

  /* Need to strip some bytes if not a multiple of 40.  */
  output[(databits + 5 - 1) / 5] = 0;
  return output;
}
