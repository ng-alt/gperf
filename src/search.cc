/* Search algorithm.
   Copyright (C) 1989-1998, 2000, 2002 Free Software Foundation, Inc.
   Written by Douglas C. Schmidt <schmidt@ics.uci.edu>
   and Bruno Haible <bruno@clisp.org>.

   This file is part of GNU GPERF.

   GNU GPERF is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   GNU GPERF is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; see the file COPYING.
   If not, write to the Free Software Foundation, Inc.,
   59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.  */

#include <stdio.h>
#include <stdlib.h> /* declares exit(), rand(), srand() */
#include <string.h> /* declares memset(), memcmp() */
#include <time.h> /* declares time() */
#include <limits.h> /* defines INT_MIN, INT_MAX */
#include "options.h"
#include "hash-table.h"
#include "search.h"

/* Make the hash table 8 times larger than the number of keyword entries. */
static const int TABLE_MULTIPLE     = 10;

/* Efficiently returns the least power of two greater than or equal to X! */
#define POW(X) ((!X)?1:(X-=1,X|=X>>1,X|=X>>2,X|=X>>4,X|=X>>8,X|=X>>16,(++X)))

Search::Search (KeywordExt_List *list)
  : _head (list)
{
}

bool Search::_determined[MAX_ALPHA_SIZE];

void
Search::prepare ()
{
  KeywordExt_List *temp;
  KeywordExt_List *trail = NULL;

  _total_keys = 0;
  for (temp = _head; temp; temp = temp->rest())
    {
      temp->first()->init_selchars(_occurrences);
      _total_keys++;
    }
       
  /* Hash table this number of times larger than keyword number. */
  int table_size = (_list_len = _total_keys) * TABLE_MULTIPLE;
  /* Table must be a power of 2 for the hash function scheme to work. */
  KeywordExt **table = new KeywordExt*[POW (table_size)];

  /* Make large hash table for efficiency. */
  Hash_Table found_link (table, table_size, option[NOLENGTH]);

  /* Test whether there are any links and also set the maximum length of
     an identifier in the keyword list. */
  _total_duplicates = 0;
  _max_key_len = INT_MIN;
  _min_key_len = INT_MAX;
  for (temp = _head; temp; temp = temp->rest())
    {
      KeywordExt *keyword = temp->first();
      KeywordExt *other_keyword = found_link.insert (keyword);

      /* Check for links.  We deal with these by building an equivalence class
         of all duplicate values (i.e., links) so that only 1 keyword is
         representative of the entire collection.  This *greatly* simplifies
         processing during later stages of the program. */

      if (other_keyword)
        {
          _total_duplicates++;
          _list_len--;
          trail->rest() = temp->rest();
          temp->first()->_duplicate_link = other_keyword->_duplicate_link;
          other_keyword->_duplicate_link = temp->first();

          /* Complain if user hasn't enabled the duplicate option. */
          if (!option[DUP] || option[DEBUG])
            fprintf (stderr, "Key link: \"%.*s\" = \"%.*s\", with key set \"%.*s\".\n",
                             keyword->_allchars_length, keyword->_allchars,
                             other_keyword->_allchars_length, other_keyword->_allchars,
                             keyword->_selchars_length, keyword->_selchars);
        }
      else
        trail = temp;

      /* Update minimum and maximum keyword length, if needed. */
      if (_max_key_len < keyword->_allchars_length)
        _max_key_len = keyword->_allchars_length;
      if (_min_key_len > keyword->_allchars_length)
        _min_key_len = keyword->_allchars_length;
    }

  delete[] table;

  /* Exit program if links exists and option[DUP] not set, since we can't continue */
  if (_total_duplicates)
    {
      if (option[DUP])
        fprintf (stderr, "%d input keys have identical hash values, examine output carefully...\n",
                         _total_duplicates);
      else
        {
          fprintf (stderr, "%d input keys have identical hash values,\ntry different key positions or use option -D.\n",
                           _total_duplicates);
          exit (1);
        }
    }
  /* Exit program if an empty string is used as key, since the comparison
     expressions don't work correctly for looking up an empty string. */
  if (_min_key_len == 0)
    {
      fprintf (stderr, "Empty input key is not allowed.\nTo recognize an empty input key, your code should check for\nlen == 0 before calling the gperf generated lookup function.\n");
      exit (1);
    }
}

/* Recursively merges two sorted lists together to form one sorted list. The
   ordering criteria is by frequency of occurrence of elements in the key set
   or by the hash value.  This is a kludge, but permits nice sharing of
   almost identical code without incurring the overhead of a function
   call comparison. */

KeywordExt_List *
Search::merge (KeywordExt_List *list1, KeywordExt_List *list2)
{
  KeywordExt_List *result;
  KeywordExt_List **resultp = &result;
  for (;;)
    {
      if (!list1)
        {
          *resultp = list2;
          break;
        }
      if (!list2)
        {
          *resultp = list1;
          break;
        }
      if (_occurrence_sort && list1->first()->_occurrence < list2->first()->_occurrence
          || _hash_sort && list1->first()->_hash_value > list2->first()->_hash_value)
        {
          *resultp = list2;
          resultp = &list2->rest(); list2 = list1; list1 = *resultp;
        }
      else
        {
          *resultp = list1;
          resultp = &list1->rest(); list1 = *resultp;
        }
    }
  return result;
}

/* Applies the merge sort algorithm to recursively sort the key list by
   frequency of occurrence of elements in the key set. */

KeywordExt_List *
Search::merge_sort (KeywordExt_List *head)
{
  if (!head || !head->rest())
    return head;
  else
    {
      KeywordExt_List *middle = head;
      KeywordExt_List *temp   = head->rest()->rest();

      while (temp)
        {
          temp   = temp->rest();
          middle = middle->rest();
          if (temp)
            temp = temp->rest();
        }

      temp         = middle->rest();
      middle->rest() = 0;
      return merge (merge_sort (head), merge_sort (temp));
    }
}

/* Returns the frequency of occurrence of elements in the key set. */

inline int
Search::get_occurrence (KeywordExt *ptr)
{
  int value = 0;

  const char *p = ptr->_selchars;
  unsigned int i = ptr->_selchars_length;
  for (; i > 0; p++, i--)
    value += _occurrences[static_cast<unsigned char>(*p)];

  return value;
}

/* Enables the index location of all key set elements that are now
   determined. */

inline void
Search::set_determined (KeywordExt *ptr)
{
  const char *p = ptr->_selchars;
  unsigned int i = ptr->_selchars_length;
  for (; i > 0; p++, i--)
    _determined[static_cast<unsigned char>(*p)] = true;
}

/* Returns TRUE if PTR's key set is already completely determined. */

inline bool
Search::already_determined (KeywordExt *ptr)
{
  bool is_determined = true;

  const char *p = ptr->_selchars;
  unsigned int i = ptr->_selchars_length;
  for (; is_determined && i > 0; p++, i--)
    is_determined = _determined[static_cast<unsigned char>(*p)];

  return is_determined;
}

/* Reorders the table by first sorting the list so that frequently occuring
   keys appear first, and then the list is reordered so that keys whose values
   are already determined will be placed towards the front of the list.  This
   helps prune the search time by handling inevitable collisions early in the
   search process.  See Cichelli's paper from Jan 1980 JACM for details.... */

void
Search::reorder ()
{
  KeywordExt_List *ptr;
  for (ptr = _head; ptr; ptr = ptr->rest())
    {
      KeywordExt *keyword = ptr->first();

      keyword->_occurrence = get_occurrence (keyword);
    }

  _hash_sort = false;
  _occurrence_sort = true;

  _head = merge_sort (_head);

  for (ptr = _head; ptr->rest(); ptr = ptr->rest())
    {
      set_determined (ptr->first());

      if (!already_determined (ptr->rest()->first()))
        {
          KeywordExt_List *trail_ptr = ptr->rest();
          KeywordExt_List *run_ptr   = trail_ptr->rest();

          for (; run_ptr; run_ptr = trail_ptr->rest())
            {

              if (already_determined (run_ptr->first()))
                {
                  trail_ptr->rest() = run_ptr->rest();
                  run_ptr->rest()   = ptr->rest();
                  ptr = ptr->rest() = run_ptr;
                }
              else
                trail_ptr = run_ptr;
            }
        }
    }
}

/* Returns the length of entire key list. */

int
Search::keyword_list_length ()
{
  return _list_len;
}

/* Returns length of longest key read. */

int
Search::max_key_length ()
{
  return _max_key_len;
}

/* Returns number of key positions.  */

int
Search::get_max_keysig_size ()
{
  return option[ALLCHARS] ? _max_key_len : option.get_max_keysig_size ();
}

/* Generate a key set's hash value. */

inline int
Search::hash (KeywordExt *key_node)
{
  int sum = option[NOLENGTH] ? 0 : key_node->_allchars_length;

  const char *p = key_node->_selchars;
  int i = key_node->_selchars_length;
  for (; i > 0; p++, i--)
      sum += _asso_values[static_cast<unsigned char>(*p)];

  return key_node->_hash_value = sum;
}

/* Merge two disjoint hash key multisets to form the ordered disjoint union of the sets.
   (In a multiset, an element can occur multiple times.)
   Precondition: both set_1 and set_2 must be ordered. Returns the length
   of the combined set. */

inline int
Search::compute_disjoint_union  (const char *set_1, int size_1, const char *set_2, int size_2, char *set_3)
{
  char *base = set_3;

  while (size_1 > 0 && size_2 > 0)
    if (*set_1 == *set_2)
      set_1++, size_1--, set_2++, size_2--;
    else
      {
        char next;
        if (*set_1 < *set_2)
          next = *set_1++, size_1--;
        else
          next = *set_2++, size_2--;
        if (set_3 == base || next != set_3[-1])
          *set_3++ = next;
      }

  while (size_1 > 0)
    {
      char next;
      next = *set_1++, size_1--;
      if (set_3 == base || next != set_3[-1])
        *set_3++ = next;
    }

  while (size_2 > 0)
    {
      char next;
      next = *set_2++, size_2--;
      if (set_3 == base || next != set_3[-1])
        *set_3++ = next;
    }
  return set_3 - base;
}

/* Sort the UNION_SET in increasing frequency of occurrence.
   This speeds up later processing since we may assume the resulting
   set (Set_3, in this case), is ordered. Uses insertion sort, since
   the UNION_SET is typically short. */

inline void
Search::sort_set (char *union_set, int len)
{
  int i, j;

  for (i = 0, j = len - 1; i < j; i++)
    {
      int curr;
      char tmp;

      for (curr = i + 1, tmp = union_set[curr];
           curr > 0 && _occurrences[static_cast<unsigned char>(tmp)] < _occurrences[static_cast<unsigned char>(union_set[curr-1])];
           curr--)
        union_set[curr] = union_set[curr - 1];

      union_set[curr] = tmp;
    }
}

/* Find out how character value change affects successfully hashed items.
   Returns FALSE if no other hash values are affected, else returns TRUE.
   Note that because Option.Get_Asso_Max is a power of two we can guarantee
   that all legal Asso_Values are visited without repetition since
   Option.Get_Jump was forced to be an odd value! */

inline bool
Search::affects_prev (char c, KeywordExt *curr)
{
  int original_char = _asso_values[static_cast<unsigned char>(c)];
  int total_iterations = !option[FAST]
    ? get_asso_max () : option.get_iterations () ? option.get_iterations () : keyword_list_length ();

  /* Try all legal associated values. */

  for (int i = total_iterations - 1; i >= 0; i--)
    {
      int collisions = 0;

      _asso_values[static_cast<unsigned char>(c)] =
        (_asso_values[static_cast<unsigned char>(c)] + (option.get_jump () ? option.get_jump () : rand ()))
        & (get_asso_max () - 1);

      /* Iteration Number array is a win, O(1) intialization time! */
      _collision_detector->clear ();

      /* See how this asso_value change affects previous keywords.  If
         it does better than before we'll take it! */

      for (KeywordExt_List *ptr = _head; ; ptr = ptr->rest())
        {
          KeywordExt *keyword = ptr->first();
          if (_collision_detector->set_bit (hash (keyword))
              && ++collisions >= _fewest_collisions)
            break;
          if (keyword == curr)
            {
              _fewest_collisions = collisions;
              if (option[DEBUG])
                fprintf (stderr, "- resolved after %d iterations", total_iterations - i);
              return false;
            }
        }
    }

  /* Restore original values, no more tries. */
  _asso_values[static_cast<unsigned char>(c)] = original_char;
  /* If we're this far it's time to try the next character.... */
  return true;
}

/* Change a character value, try least-used characters first. */

void
Search::change (KeywordExt *prior, KeywordExt *curr)
{
  static char *union_set;
  int union_set_length;

  if (!union_set)
    union_set = new char [2 * get_max_keysig_size ()];

  if (option[DEBUG])
    {
      fprintf (stderr, "collision on keyword #%d, prior = \"%.*s\", curr = \"%.*s\" hash = %d\n",
               _num_done,
               prior->_allchars_length, prior->_allchars,
               curr->_allchars_length, curr->_allchars,
               curr->_hash_value);
      fflush (stderr);
    }
  union_set_length = compute_disjoint_union (prior->_selchars, prior->_selchars_length, curr->_selchars, curr->_selchars_length, union_set);
  sort_set (union_set, union_set_length);

  /* Try changing some values, if change doesn't alter other values continue normal action. */
  _fewest_collisions++;

  const char *p = union_set;
  int i = union_set_length;
  for (; i > 0; p++, i--)
    if (!affects_prev (*p, curr))
      {
        if (option[DEBUG])
          {
            fprintf (stderr, " by changing asso_value['%c'] (char #%d) to %d\n",
                     *p, p - union_set + 1, _asso_values[static_cast<unsigned char>(*p)]);
            fflush (stderr);
          }
        return; /* Good, doesn't affect previous hash values, we'll take it. */
      }

  for (KeywordExt_List *ptr = _head; ; ptr = ptr->rest())
    {
      KeywordExt* keyword = ptr->first();
      if (keyword == curr)
        break;
      hash (keyword);
    }

  hash (curr);

  if (option[DEBUG])
    {
      fprintf (stderr, "** collision not resolved after %d iterations, %d duplicates remain, continuing...\n",
               !option[FAST] ? get_asso_max () : option.get_iterations () ? option.get_iterations () : keyword_list_length (),
               _fewest_collisions + _total_duplicates);
      fflush (stderr);
    }
}

/* Sorts the keys by hash value. */

void
Search::sort ()
{
  _hash_sort       = true;
  _occurrence_sort = false;

  _head = merge_sort (_head);
}

void
Search::optimize ()
{
  prepare ();
  if (option[ORDER])
    reorder ();
  _num_done          = 1;
  _fewest_collisions = 0;
  int asso_value_max    = option.get_size_multiple ();
  int non_linked_length = keyword_list_length ();
  if (asso_value_max == 0)
    asso_value_max = non_linked_length;
  else if (asso_value_max > 0)
    asso_value_max *= non_linked_length;
  else /* if (asso_value_max < 0) */
    asso_value_max = non_linked_length / -asso_value_max;
  set_asso_max (POW (asso_value_max));

  if (option[RANDOM])
    {
      srand (reinterpret_cast<long>(time (0)));

      for (int i = 0; i < ALPHA_SIZE; i++)
        _asso_values[i] = (rand () & asso_value_max - 1);
    }
  else
    {
      int asso_value = option.get_initial_asso_value ();

      if (asso_value)           /* Initialize array if user requests non-zero default. */
        for (int i = ALPHA_SIZE - 1; i >= 0; i--)
          _asso_values[i] = asso_value & get_asso_max () - 1;
    }
  _max_hash_value = max_key_length () + get_asso_max () * get_max_keysig_size ();
  _collision_detector = new Bool_Array (_max_hash_value + 1);

  if (option[DEBUG])
    fprintf (stderr, "total non-linked keys = %d\nmaximum associated value is %d"
             "\nmaximum size of generated hash table is %d\n",
             non_linked_length, asso_value_max, _max_hash_value);

  KeywordExt_List *curr;
  for (curr = _head; curr != NULL; curr = curr->rest())
    {
      KeywordExt *currkw = curr->first();

      hash (currkw);

      for (KeywordExt_List *ptr = _head; ptr != curr; ptr = ptr->rest())
        {
          KeywordExt *ptrkw = ptr->first();

          if (ptrkw->_hash_value == currkw->_hash_value)
            {
              change (ptrkw, currkw);
              break;
            }
        }
      _num_done++;
    }

  /* Make one final check, just to make sure nothing weird happened.... */

  _collision_detector->clear ();

  for (curr = _head; curr; curr = curr->rest())
    {
      unsigned int hashcode = hash (curr->first());
      if (_collision_detector->set_bit (hashcode))
        {
          if (option[DUP]) /* Keep track of this number... */
            _total_duplicates++;
          else /* Yow, big problems.  we're outta here! */
            {
              fprintf (stderr,
                       "\nInternal error, duplicate value %d:\n"
                       "try options -D or -r, or use new key positions.\n\n",
                       hashcode);
              exit (1);
            }
        }
    }

  /* Sorts the key word list by hash value.  */
  sort ();
}

/* Prints out some diagnostics upon completion. */

Search::~Search ()
{
  delete _collision_detector;
  if (option[DEBUG])
    {
      fprintf (stderr, "\ndumping occurrence and associated values tables\n");

      for (int i = 0; i < ALPHA_SIZE; i++)
        if (_occurrences[i])
          fprintf (stderr, "asso_values[%c] = %6d, occurrences[%c] = %6d\n",
                   i, _asso_values[i], i, _occurrences[i]);

      fprintf (stderr, "end table dumping\n");

      fprintf (stderr, "\nDumping key list information:\ntotal non-static linked keywords = %d"
               "\ntotal keywords = %d\ntotal duplicates = %d\nmaximum key length = %d\n",
               _list_len, _total_keys, _total_duplicates, _max_key_len);

      int field_width = get_max_keysig_size ();
      fprintf (stderr, "\nList contents are:\n(hash value, key length, index, %*s, keyword):\n",
               field_width, "selchars");
      for (KeywordExt_List *ptr = _head; ptr; ptr = ptr->rest())
        fprintf (stderr, "%11d,%11d,%6d, %*.*s, %.*s\n",
                 ptr->first()->_hash_value, ptr->first()->_allchars_length, ptr->first()->_final_index,
                 field_width, ptr->first()->_selchars_length, ptr->first()->_selchars,
                 ptr->first()->_allchars_length, ptr->first()->_allchars);

      fprintf (stderr, "End dumping list.\n\n");
    }
}
