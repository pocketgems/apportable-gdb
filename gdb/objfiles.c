/* GDB routines for manipulating objfiles.

   Copyright (C) 1992-2004, 2007-2012 Free Software Foundation, Inc.

   Contributed by Cygnus Support, using pieces from other GDB modules.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

/* This file contains support routines for creating, manipulating, and
   destroying objfile structures.  */

#include "defs.h"
#include "bfd.h"		/* Binary File Description */
#include "symtab.h"
#include "symfile.h"
#include "objfiles.h"
#include "gdb-stabs.h"
#include "target.h"
#include "bcache.h"
#include "mdebugread.h"
#include "expression.h"
#include "parser-defs.h"

#include "gdb_assert.h"
#include <sys/types.h>
#include "gdb_stat.h"
#include <fcntl.h>
#include "gdb_obstack.h"
#include "gdb_string.h"
#include "hashtab.h"

#include "breakpoint.h"
#include "block.h"
#include "dictionary.h"
#include "source.h"
#include "addrmap.h"
#include "arch-utils.h"
#include "exec.h"
#include "observer.h"
#include "complaints.h"
#include "psymtab.h"
#include "solist.h"

/* Prototypes for local functions */

static void objfile_alloc_data (struct objfile *objfile);
static void objfile_free_data (struct objfile *objfile);

/* Externally visible variables that are owned by this module.
   See declarations in objfile.h for more info.  */

struct objfile *rt_common_objfile;	/* For runtime common symbols */

struct objfile_pspace_info
{
  int objfiles_changed_p;
  struct obj_section **sections;
  int num_sections;
};

/* Per-program-space data key.  */
static const struct program_space_data *objfiles_pspace_data;

static void
objfiles_pspace_data_cleanup (struct program_space *pspace, void *arg)
{
  struct objfile_pspace_info *info;

  info = program_space_data (pspace, objfiles_pspace_data);
  if (info != NULL)
    {
      xfree (info->sections);
      xfree (info);
    }
}

/* Get the current svr4 data.  If none is found yet, add it now.  This
   function always returns a valid object.  */

static struct objfile_pspace_info *
get_objfile_pspace_data (struct program_space *pspace)
{
  struct objfile_pspace_info *info;

  info = program_space_data (pspace, objfiles_pspace_data);
  if (info == NULL)
    {
      info = XZALLOC (struct objfile_pspace_info);
      set_program_space_data (pspace, objfiles_pspace_data, info);
    }

  return info;
}

/* Called via bfd_map_over_sections to build up the section table that
   the objfile references.  The objfile contains pointers to the start
   of the table (objfile->sections) and to the first location after
   the end of the table (objfile->sections_end).  */

static void
add_to_objfile_sections (struct bfd *abfd, struct bfd_section *asect,
			 void *objfilep)
{
  struct objfile *objfile = (struct objfile *) objfilep;
  struct obj_section section;
  flagword aflag;

  aflag = bfd_get_section_flags (abfd, asect);
  if (!(aflag & SEC_ALLOC))
    return;
  if (bfd_section_size (abfd, asect) == 0)
    return;

  section.objfile = objfile;
  section.the_bfd_section = asect;
  section.ovly_mapped = 0;
  obstack_grow (&objfile->objfile_obstack,
		(char *) &section, sizeof (section));
  objfile->sections_end
    = (struct obj_section *) (((size_t) objfile->sections_end) + 1);
}

/* Builds a section table for OBJFILE.

   Note that while we are building the table, which goes into the
   objfile obstack, we hijack the sections_end pointer to instead hold
   a count of the number of sections.  When bfd_map_over_sections
   returns, this count is used to compute the pointer to the end of
   the sections table, which then overwrites the count.

   Also note that the OFFSET and OVLY_MAPPED in each table entry
   are initialized to zero.

   Also note that if anything else writes to the objfile obstack while
   we are building the table, we're pretty much hosed.  */

void
build_objfile_section_table (struct objfile *objfile)
{
  objfile->sections_end = 0;
  bfd_map_over_sections (objfile->obfd,
			 add_to_objfile_sections, (void *) objfile);
  objfile->sections = obstack_finish (&objfile->objfile_obstack);
  objfile->sections_end = objfile->sections + (size_t) objfile->sections_end;
}

/* Given a pointer to an initialized bfd (ABFD) and some flag bits
   allocate a new objfile struct, fill it in as best we can, link it
   into the list of all known objfiles, and return a pointer to the
   new objfile struct.

   The FLAGS word contains various bits (OBJF_*) that can be taken as
   requests for specific operations.  Other bits like OBJF_SHARED are
   simply copied through to the new objfile flags member.  */

/* NOTE: carlton/2003-02-04: This function is called with args NULL, 0
   by jv-lang.c, to create an artificial objfile used to hold
   information about dynamically-loaded Java classes.  Unfortunately,
   that branch of this function doesn't get tested very frequently, so
   it's prone to breakage.  (E.g. at one time the name was set to NULL
   in that situation, which broke a loop over all names in the dynamic
   library loader.)  If you change this function, please try to leave
   things in a consistent state even if abfd is NULL.  */

struct objfile *
allocate_objfile (bfd *abfd, int flags)
{
  struct objfile *objfile;

  objfile = (struct objfile *) xzalloc (sizeof (struct objfile));
  objfile->psymbol_cache = psymbol_bcache_init ();
  objfile->macro_cache = bcache_xmalloc (NULL, NULL);
  objfile->filename_cache = bcache_xmalloc (NULL, NULL);
  /* We could use obstack_specify_allocation here instead, but
     gdb_obstack.h specifies the alloc/dealloc functions.  */
  obstack_init (&objfile->objfile_obstack);
  terminate_minimal_symbol_table (objfile);

  objfile_alloc_data (objfile);

  /* Update the per-objfile information that comes from the bfd, ensuring
     that any data that is reference is saved in the per-objfile data
     region.  */

  objfile->obfd = gdb_bfd_ref (abfd);
  if (abfd != NULL)
    {
      /* Look up the gdbarch associated with the BFD.  */
      objfile->gdbarch = gdbarch_from_bfd (abfd);

      objfile->name = xstrdup (bfd_get_filename (abfd));
      objfile->mtime = bfd_get_mtime (abfd);

      /* Build section table.  */
      build_objfile_section_table (objfile);
    }
  else
    {
      objfile->name = xstrdup ("<<anonymous objfile>>");
    }

  objfile->pspace = current_program_space;

  /* Initialize the section indexes for this objfile, so that we can
     later detect if they are used w/o being properly assigned to.  */

  objfile->sect_index_text = -1;
  objfile->sect_index_data = -1;
  objfile->sect_index_bss = -1;
  objfile->sect_index_rodata = -1;

  /* Add this file onto the tail of the linked list of other such files.  */

  objfile->next = NULL;
  if (object_files == NULL)
    object_files = objfile;
  else
    {
      struct objfile *last_one;

      for (last_one = object_files;
	   last_one->next;
	   last_one = last_one->next);
      last_one->next = objfile;
    }

  /* Save passed in flag bits.  */
  objfile->flags |= flags;

  /* Rebuild section map next time we need it.  */
  get_objfile_pspace_data (objfile->pspace)->objfiles_changed_p = 1;

  return objfile;
}

/* Retrieve the gdbarch associated with OBJFILE.  */
struct gdbarch *
get_objfile_arch (struct objfile *objfile)
{
  return objfile->gdbarch;
}

/* Apple addition begin */


/* APPLE LOCAL - with the advent of ZeroLink, it is not uncommon for Mac OS X 
   applications to consist of 500+ shared libraries.  At that point searching
   linearly for address->obj_section becomes very costly, and it is a common
   operation.  So we maintain an ordered array of obj_sections, and use that
   to do a binary search for the matching section. 

   N.B. We could just use an array of pointers to the obj_section
   instead of this struct, but this way the malloc'ed array contains
   all the elements we need to do the pc->obj_section lookup, so we
   can do the search only touching a couple of pages of memory, rather
   than wandering all over the heap.  

   FIXME: We really should merge this array with the to_sections array in
   the target, but that doesn't have back-pointers to the obj_section.  I
   am not sure how hard it would be to get that working.  This is simpler 
   for now.  */

struct ordered_obj_section
{
  struct obj_section *obj_section;
  struct bfd_section *the_bfd_section;
  CORE_ADDR addr;
  CORE_ADDR endaddr;
};

/* This is the table of ordered_sections.  It is malloc'ed to
   ORDERED_SECTIONS_CHUNK_SIZE originally, and then realloc'ed if we
   get more sections. */

static struct ordered_obj_section *ordered_sections;

/* This is the number of entries currently in the ordered_sections table.  */
static int num_ordered_sections = 0;

#define ORDERED_SECTIONS_CHUNK_SIZE 3000

static int max_num_ordered_sections = ORDERED_SECTIONS_CHUNK_SIZE;

static int find_in_ordered_sections_index (CORE_ADDR addr, struct bfd_section *bfd_section);
static int get_insert_index_in_ordered_sections (struct obj_section *section);

/* When we want to add a bunch of obj_sections at a time, we will
   find all the insert points, make an array of type OJB_SECTION_WITH_INDEX
   sort that in reverse order, then sweep the ordered_sections list
   shifting and adding them all.  */

struct obj_section_with_index
{
  int index;
  struct obj_section *section;
};


/* This is the quicksort compare routine for OBJ_SECTION_WITH_INDEX
   objects.  It sorts by section start address in decreasing 
   order.  */

static int
backward_section_compare (const void *left_ptr, 
     const void *right_ptr)
{
  
  struct obj_section_with_index *left 
    = (struct obj_section_with_index *) left_ptr;
  struct obj_section_with_index * right 
    = (struct obj_section_with_index *) right_ptr;

  if (obj_section_addr(left->section) > obj_section_addr(right->section))
    return -1;
  else if (obj_section_addr(left->section) < obj_section_addr(right->section))
    return 1;
  else
      return 0;
}

/* Simple integer compare for quicksort */

static int
forward_int_compare (const void *left_ptr, 
     const void *right_ptr)
{
  int *left = (int *) left_ptr;
  int *right = (int *) right_ptr;

  if (*left < *right)
    return -1;
  else if (*left > *right)
    return 1;
  else
      return 0;
}

/* APPLE LOCAL: The difference between the segment names and the section
   names is the segment names always only have one dot.  Use this to count
   the dots quickly...  */
static int
number_of_dots (const char *s)
{
  int numdots = 0;
  while (*s != '\0')
    {
      if (*s == '.')
  numdots++;
      s++;
    }
  return numdots;
}

/* Delete all the obj_sections in OBJFILE from the ordered_sections
   global list.  N.B. this routine uses the addresses in the sections
   in the objfile to find the entries in the ordered_sections list, 
   so if you are going to relocate the obj_sections in an objfile,
   call this BEFORE you relocate, then relocate, then call 
   objfile_add_to_ordered_sections.  */

#define STATIC_DELETE_LIST_SIZE 256

void 
objfile_delete_from_ordered_sections (struct objfile *objfile)
{
  int i;
  int ndeleted;
  int delete_list_size;
  int static_delete_list[STATIC_DELETE_LIST_SIZE];
  int *delete_list = static_delete_list;
  
  struct obj_section *s;
  
#if 0
  /* Apportable TODO */
  /* APPLE LOCAL: we need to check if this is a separate debug files and try to 
     remove the sections to the ordered list if so. The backlink will not be
     setup when the separate debug objfile is in the process of being created, 
     so a flag was added to make sure we can tell.  */
  if (objfile->separate_debug_objfile_backlink || 
      objfile->flags & OBJF_SEPARATE_DEBUG_FILE)
  return; 
#endif
  /* Do deletion of the sections by building up an array of
     "to be removed" indices, and then block compact the array using
     these indices.  */
  
  delete_list_size = objfile->sections_end - objfile->sections;
  if (delete_list_size > STATIC_DELETE_LIST_SIZE)
    delete_list = (int *) xmalloc (delete_list_size * sizeof (int));

  ndeleted = 0;

  ALL_OBJFILE_OSECTIONS (objfile, s)
    {
      int index;
      /* APPLE LOCAL: Oh, hacky, hacky...  The bfd Mach-O reader makes
         bfd_sections for both the sections & segments (the container of
         the sections).  This would make pc->bfd_section lookup non-unique.
         so we just drop the segments from our list.  */
      if (s->the_bfd_section && s->the_bfd_section->name &&
          number_of_dots (s->the_bfd_section->name) == 1)
        continue;

      if (s->objfile->section_offsets == NULL) continue; 

      index = find_in_ordered_sections_index (obj_section_addr(s), s->the_bfd_section);
      if (index == -1)
  warning ("Trying to remove a section from"
      " the ordered section list that did not exist"
      " at 0x%lx.", obj_section_addr(s));
      else
  {
    delete_list[ndeleted] = index;
    ndeleted++;
  }
    }
  qsort (delete_list, ndeleted, sizeof (int),
   forward_int_compare);

  /* Stick a boundary on the end of the delete list - it makes the block
     shuffle algorithm easier.  I also have to set the boundary to one
     past the list end because it is supposed to be the next element
     deleted, and I don't want to delete the last element.  */

  delete_list[ndeleted] = num_ordered_sections;

  for (i = 0; i < ndeleted; i++)
    {
      struct ordered_obj_section *src, *dest;
      size_t len;

      src = &(ordered_sections[delete_list[i] + 1]);
      dest = &(ordered_sections[delete_list[i] - i]);
      len = (delete_list[i+1] - delete_list[i] - 1)
  * sizeof (struct ordered_obj_section);

      bcopy (src, dest, len);
    }

  
  num_ordered_sections -= ndeleted;
  if (delete_list != static_delete_list)
    xfree (delete_list);
}

/* This returns the index before which the obj_section SECTION
   should be added in the ordered_sections list.  This only sorts
   by addr, and pays no attention to endaddr, or the_bfd_section.  */

static int
get_insert_index_in_ordered_sections (struct obj_section *section)
{
  int insert = -1;

  if (num_ordered_sections > 0)
    {
      int bot = 0;
      int top = num_ordered_sections - 1;

      do
  {
    int mid = (top + bot) / 2;

    if (ordered_sections[mid].addr < obj_section_addr(section))
      {
        if ((mid == num_ordered_sections - 1) 
      || (ordered_sections[mid + 1].addr) >= obj_section_addr(section))
    {
      insert = mid;
      break;
    }
        else if (mid + 1 == num_ordered_sections - 1)
    {
      insert = mid + 1;
      break;
    }
        bot = mid + 1;
      }
    else
      {
        if (mid == 0 || ordered_sections[mid - 1].addr <= obj_section_addr(section))
    {
      insert = mid - 1;
      break;
    }
        top = mid - 1;
      }
  } while (top > bot);
    }

  return insert + 1;
}

/* This adds all the obj_sections for OBJFILE to the ordered_sections
   array */

#define STATIC_INSERT_LIST_SIZE 256

void
objfile_add_to_ordered_sections (struct objfile *objfile)
{
  struct obj_section *s;
  int i, num_left, total;
  struct obj_section_with_index static_insert_list[STATIC_INSERT_LIST_SIZE];
  struct obj_section_with_index *insert_list = static_insert_list;
  int insert_list_size;

  /* APPLE LOCAL: we need to check if this is a separate debug files and not 
     add the sections to the ordered list if so. The backlink will not be setup
     when the separate debug objfile is in the process of being created, so a 
     flag was added to make sure it never gets added.  */
#if 0
  /* Apportable TODO */
  if (objfile->separate_debug_objfile_backlink || 
      objfile->flags & OBJF_SEPARATE_DEBUG_FILE)
  return;

  CHECK_FATAL (objfile != NULL);
#endif

  /* First find the index for insertion of all the sections in
     this objfile.  The sort that array in reverse order by address,
     then go through the ordered list block moving the bits between
     the insert points, then adding the pieces we need to add.  */

  insert_list_size = objfile->sections_end - objfile->sections;
  if (insert_list_size > STATIC_INSERT_LIST_SIZE)
    insert_list = (struct obj_section_with_index *) 
      xmalloc (insert_list_size * sizeof (struct obj_section_with_index));

  total = 0;
  ALL_OBJFILE_OSECTIONS (objfile, s)
    {
      /* APPLE LOCAL: Oh, hacky, hacky...  The bfd Mach-O reader makes
         bfd_sections for both the sections & segments (the container of
         the sections).  This would make pc->bfd_section lookup non-unique.
         so we just drop the segments from our list.  */
      if (s->the_bfd_section && s->the_bfd_section->segment_mark == 1)
        continue;

      insert_list[total].index = get_insert_index_in_ordered_sections (s);
      insert_list[total].section = s;
      total++;
    }

  qsort (insert_list, total, sizeof (struct obj_section_with_index), 
   backward_section_compare);

  /* Grow the array if needed */

  if (ordered_sections == NULL)
    {
      max_num_ordered_sections = ORDERED_SECTIONS_CHUNK_SIZE;
      ordered_sections = (struct ordered_obj_section *)
  xmalloc (max_num_ordered_sections 
     * sizeof (struct ordered_obj_section));
    }
  else if (num_ordered_sections + total >= max_num_ordered_sections)
    {
      /* TOTAL should be small, but on the off chance that it is not, 
   just add it to the allocation as well.  */
      max_num_ordered_sections += ORDERED_SECTIONS_CHUNK_SIZE + total;
      ordered_sections = (struct ordered_obj_section *) 
  xrealloc (ordered_sections, 
      max_num_ordered_sections 
      * sizeof (struct ordered_obj_section));
    }
  
  num_left = total;

  for (i = 0; i < total; i++)
    {
      struct ordered_obj_section *src;
      struct ordered_obj_section *dest;
      struct obj_section *s;
      size_t len;
      int pos;

      src = &(ordered_sections[insert_list[i].index]);
      dest = &(ordered_sections[insert_list[i].index + num_left]);

      if (i == 0)
  len = num_ordered_sections - insert_list[i].index;
      else
  len = insert_list[i - 1].index - insert_list[i].index;
      len *= sizeof (struct ordered_obj_section);
      bcopy (src, dest, len);

      /* Now put the new element in place */
      s = insert_list[i].section;
      pos = insert_list[i].index + num_left - 1;
      ordered_sections[pos].addr = obj_section_addr(s);
      ordered_sections[pos].endaddr = obj_section_endaddr(s);
      ordered_sections[pos].obj_section = s;
      ordered_sections[pos].the_bfd_section = s->the_bfd_section;

      num_left--;
      if (num_left == 0)
  break;
    }

  num_ordered_sections += total;
  if (insert_list != static_insert_list)
    xfree (insert_list);

}


/* This returns the index in the ordered_sections array corresponding
   to the pair ADDR, BFD_SECTION (can be null), or -1 if not found. */

static int
find_in_ordered_sections_index (CORE_ADDR addr, struct bfd_section *bfd_section)
{
  int bot = 0;
  int top = num_ordered_sections;
  struct ordered_obj_section *obj_sect_ptr;

  if (num_ordered_sections == 0)
    return -1;

  do 
    {
      int mid = (top + bot) / 2;

      obj_sect_ptr = &ordered_sections[mid]; 
      if (obj_sect_ptr->addr <= addr)
  {
    if (addr < obj_sect_ptr->endaddr)
      {
        /* It is possible that the sections overlap.  This will
     happen in two cases that I know of.  One is when you
     have not run the app yet, so that a bunch of the
     sections are still mapped at 0, and haven't been.
     relocated yet.  The other is because on MacOS X we (I
     think errantly) make sections both for the segment
     command, and for the sections it contains.  In the former
           case, this becomes a linear search just like the original
           algorithm.  In the latter, we should find it in the
           near neighborhood pretty soon.  */

        if ((bfd_section == NULL )
      || (bfd_section == obj_sect_ptr->the_bfd_section))
    return mid;
        else
    {
      /* So to find the containing element, we can look
         to increasing indices, till the start address
         of our element is above the addr, but we have to go
         all the way to the left to find the address. */

      int pos;
      for (pos = mid + 1; pos < num_ordered_sections; pos++)
        {
          if (ordered_sections[pos].addr > addr)
      break;
          else if ((ordered_sections[pos].endaddr > addr)
             && (ordered_sections[pos].the_bfd_section == bfd_section))
      return pos;
        }
      for (pos = mid - 1; pos >= 0; pos--)
        {
          if ((ordered_sections[pos].endaddr > addr)
        && (ordered_sections[pos].the_bfd_section == bfd_section))
      return pos;
        }

      /* If we don't find one that matches BOTH the address and the 
         section, then return -1.  Callers should be prepared to either
         fail in this case, or to look more broadly.  */
      return -1;
    }
      }
    bot = mid + 1;
  }
      else
  {
    top = mid - 1;
  }
    } while (bot <= top);

  /* If we got here, we didn't find anything, so return -1. */
  return -1;
}

/* This returns the obj_section corresponding to the pair ADDR and
   BFD_SECTION (can be NULL) in the ordered sections array, or NULL
   if not found.  */

struct obj_section *
find_pc_sect_in_ordered_sections (CORE_ADDR addr, struct bfd_section *bfd_section)
{
  int index = find_in_ordered_sections_index (addr, bfd_section);
  
  if (index == -1)
    return NULL;
  else
    return ordered_sections[index].obj_section;
}
/* Apple addition end */

/* Initialize entry point information for this objfile.  */

void
init_entry_point_info (struct objfile *objfile)
{
  /* Save startup file's range of PC addresses to help blockframe.c
     decide where the bottom of the stack is.  */

  if (bfd_get_file_flags (objfile->obfd) & EXEC_P)
    {
      /* Executable file -- record its entry point so we'll recognize
         the startup file because it contains the entry point.  */
      objfile->ei.entry_point = bfd_get_start_address (objfile->obfd);
      objfile->ei.entry_point_p = 1;
    }
  else if (bfd_get_file_flags (objfile->obfd) & DYNAMIC
	   && bfd_get_start_address (objfile->obfd) != 0)
    {
      /* Some shared libraries may have entry points set and be
	 runnable.  There's no clear way to indicate this, so just check
	 for values other than zero.  */
      objfile->ei.entry_point = bfd_get_start_address (objfile->obfd);    
      objfile->ei.entry_point_p = 1;
    }
  else
    {
      /* Examination of non-executable.o files.  Short-circuit this stuff.  */
      objfile->ei.entry_point_p = 0;
    }
}

/* If there is a valid and known entry point, function fills *ENTRY_P with it
   and returns non-zero; otherwise it returns zero.  */

int
entry_point_address_query (CORE_ADDR *entry_p)
{
  struct gdbarch *gdbarch;
  CORE_ADDR entry_point;

  if (symfile_objfile == NULL || !symfile_objfile->ei.entry_point_p)
    return 0;

  gdbarch = get_objfile_arch (symfile_objfile);

  entry_point = symfile_objfile->ei.entry_point;

  /* Make certain that the address points at real code, and not a
     function descriptor.  */
  entry_point = gdbarch_convert_from_func_ptr_addr (gdbarch, entry_point,
						    &current_target);

  /* Remove any ISA markers, so that this matches entries in the
     symbol table.  */
  entry_point = gdbarch_addr_bits_remove (gdbarch, entry_point);

  *entry_p = entry_point;
  return 1;
}

/* Get current entry point address.  Call error if it is not known.  */

CORE_ADDR
entry_point_address (void)
{
  CORE_ADDR retval;

  if (!entry_point_address_query (&retval))
    error (_("Entry point address is not known."));

  return retval;
}

/* Iterator on PARENT and every separate debug objfile of PARENT.
   The usage pattern is:
     for (objfile = parent;
          objfile;
          objfile = objfile_separate_debug_iterate (parent, objfile))
       ...
*/

struct objfile *
objfile_separate_debug_iterate (const struct objfile *parent,
                                const struct objfile *objfile)
{
  struct objfile *res;

  /* If any, return the first child.  */
  res = objfile->separate_debug_objfile;
  if (res)
    return res;

  /* Common case where there is no separate debug objfile.  */
  if (objfile == parent)
    return NULL;

  /* Return the brother if any.  Note that we don't iterate on brothers of
     the parents.  */
  res = objfile->separate_debug_objfile_link;
  if (res)
    return res;

  for (res = objfile->separate_debug_objfile_backlink;
       res != parent;
       res = res->separate_debug_objfile_backlink)
    {
      gdb_assert (res != NULL);
      if (res->separate_debug_objfile_link)
        return res->separate_debug_objfile_link;
    }
  return NULL;
}

/* Put one object file before a specified on in the global list.
   This can be used to make sure an object file is destroyed before
   another when using ALL_OBJFILES_SAFE to free all objfiles.  */
void
put_objfile_before (struct objfile *objfile, struct objfile *before_this)
{
  struct objfile **objp;

  unlink_objfile (objfile);
  
  for (objp = &object_files; *objp != NULL; objp = &((*objp)->next))
    {
      if (*objp == before_this)
	{
	  objfile->next = *objp;
	  *objp = objfile;
	  return;
	}
    }
  
  internal_error (__FILE__, __LINE__,
		  _("put_objfile_before: before objfile not in list"));
}

/* Put OBJFILE at the front of the list.  */

void
objfile_to_front (struct objfile *objfile)
{
  struct objfile **objp;
  for (objp = &object_files; *objp != NULL; objp = &((*objp)->next))
    {
      if (*objp == objfile)
	{
	  /* Unhook it from where it is.  */
	  *objp = objfile->next;
	  /* Put it in the front.  */
	  objfile->next = object_files;
	  object_files = objfile;
	  break;
	}
    }
}

/* Unlink OBJFILE from the list of known objfiles, if it is found in the
   list.

   It is not a bug, or error, to call this function if OBJFILE is not known
   to be in the current list.  This is done in the case of mapped objfiles,
   for example, just to ensure that the mapped objfile doesn't appear twice
   in the list.  Since the list is threaded, linking in a mapped objfile
   twice would create a circular list.

   If OBJFILE turns out to be in the list, we zap it's NEXT pointer after
   unlinking it, just to ensure that we have completely severed any linkages
   between the OBJFILE and the list.  */

void
unlink_objfile (struct objfile *objfile)
{
  struct objfile **objpp;

  for (objpp = &object_files; *objpp != NULL; objpp = &((*objpp)->next))
    {
      if (*objpp == objfile)
	{
	  *objpp = (*objpp)->next;
	  objfile->next = NULL;
	  return;
	}
    }

  internal_error (__FILE__, __LINE__,
		  _("unlink_objfile: objfile already unlinked"));
}

/* Add OBJFILE as a separate debug objfile of PARENT.  */

void
add_separate_debug_objfile (struct objfile *objfile, struct objfile *parent)
{
  gdb_assert (objfile && parent);

  /* Must not be already in a list.  */
  gdb_assert (objfile->separate_debug_objfile_backlink == NULL);
  gdb_assert (objfile->separate_debug_objfile_link == NULL);

  objfile->separate_debug_objfile_backlink = parent;
  objfile->separate_debug_objfile_link = parent->separate_debug_objfile;
  parent->separate_debug_objfile = objfile;

  /* Put the separate debug object before the normal one, this is so that
     usage of the ALL_OBJFILES_SAFE macro will stay safe.  */
  put_objfile_before (objfile, parent);
}

/* Free all separate debug objfile of OBJFILE, but don't free OBJFILE
   itself.  */

void
free_objfile_separate_debug (struct objfile *objfile)
{
  struct objfile *child;

  for (child = objfile->separate_debug_objfile; child;)
    {
      struct objfile *next_child = child->separate_debug_objfile_link;
      free_objfile (child);
      child = next_child;
    }
}

/* Destroy an objfile and all the symtabs and psymtabs under it.  Note
   that as much as possible is allocated on the objfile_obstack 
   so that the memory can be efficiently freed.

   Things which we do NOT free because they are not in malloc'd memory
   or not in memory specific to the objfile include:

   objfile -> sf

   FIXME:  If the objfile is using reusable symbol information (via mmalloc),
   then we need to take into account the fact that more than one process
   may be using the symbol information at the same time (when mmalloc is
   extended to support cooperative locking).  When more than one process
   is using the mapped symbol info, we need to be more careful about when
   we free objects in the reusable area.  */

void
free_objfile (struct objfile *objfile)
{
  /* Free all separate debug objfiles.  */
  free_objfile_separate_debug (objfile);

  if (objfile->separate_debug_objfile_backlink)
    {
      /* We freed the separate debug file, make sure the base objfile
	 doesn't reference it.  */
      struct objfile *child;

      child = objfile->separate_debug_objfile_backlink->separate_debug_objfile;

      if (child == objfile)
        {
          /* OBJFILE is the first child.  */
          objfile->separate_debug_objfile_backlink->separate_debug_objfile =
            objfile->separate_debug_objfile_link;
        }
      else
        {
          /* Find OBJFILE in the list.  */
          while (1)
            {
              if (child->separate_debug_objfile_link == objfile)
                {
                  child->separate_debug_objfile_link =
                    objfile->separate_debug_objfile_link;
                  break;
                }
              child = child->separate_debug_objfile_link;
              gdb_assert (child);
            }
        }
    }
  
  /* Remove any references to this objfile in the global value
     lists.  */
  preserve_values (objfile);

  /* It still may reference data modules have associated with the objfile and
     the symbol file data.  */
  forget_cached_source_info_for_objfile (objfile);

  /* First do any symbol file specific actions required when we are
     finished with a particular symbol file.  Note that if the objfile
     is using reusable symbol information (via mmalloc) then each of
     these routines is responsible for doing the correct thing, either
     freeing things which are valid only during this particular gdb
     execution, or leaving them to be reused during the next one.  */

  if (objfile->sf != NULL)
    {
      (*objfile->sf->sym_finish) (objfile);
    }

  /* Discard any data modules have associated with the objfile.  The function
     still may reference objfile->obfd.  */
  objfile_free_data (objfile);

  gdb_bfd_unref (objfile->obfd);

  /* Remove it from the chain of all objfiles.  */

  unlink_objfile (objfile);

  if (objfile == symfile_objfile)
    symfile_objfile = NULL;

  if (objfile == rt_common_objfile)
    rt_common_objfile = NULL;

  /* Before the symbol table code was redone to make it easier to
     selectively load and remove information particular to a specific
     linkage unit, gdb used to do these things whenever the monolithic
     symbol table was blown away.  How much still needs to be done
     is unknown, but we play it safe for now and keep each action until
     it is shown to be no longer needed.  */

  /* Not all our callers call clear_symtab_users (objfile_purge_solibs,
     for example), so we need to call this here.  */
  clear_pc_function_cache ();

  /* Clear globals which might have pointed into a removed objfile.
     FIXME: It's not clear which of these are supposed to persist
     between expressions and which ought to be reset each time.  */
  expression_context_block = NULL;
  innermost_block = NULL;

  /* Check to see if the current_source_symtab belongs to this objfile,
     and if so, call clear_current_source_symtab_and_line.  */

  {
    struct symtab_and_line cursal = get_current_source_symtab_and_line ();

    if (cursal.symtab && cursal.symtab->objfile == objfile)
      clear_current_source_symtab_and_line ();
  }

  /* The last thing we do is free the objfile struct itself.  */

  xfree (objfile->name);
  if (objfile->global_psymbols.list)
    xfree (objfile->global_psymbols.list);
  if (objfile->static_psymbols.list)
    xfree (objfile->static_psymbols.list);
  /* Free the obstacks for non-reusable objfiles.  */
  psymbol_bcache_free (objfile->psymbol_cache);
  bcache_xfree (objfile->macro_cache);
  bcache_xfree (objfile->filename_cache);
  if (objfile->demangled_names_hash)
    htab_delete (objfile->demangled_names_hash);
  obstack_free (&objfile->objfile_obstack, 0);

  /* Rebuild section map next time we need it.  */
  get_objfile_pspace_data (objfile->pspace)->objfiles_changed_p = 1;

  xfree (objfile);
}

static void
do_free_objfile_cleanup (void *obj)
{
  free_objfile (obj);
}

struct cleanup *
make_cleanup_free_objfile (struct objfile *obj)
{
  return make_cleanup (do_free_objfile_cleanup, obj);
}

/* Free all the object files at once and clean up their users.  */

void
free_all_objfiles (void)
{
  struct objfile *objfile, *temp;
  struct so_list *so;

  /* Any objfile referencewould become stale.  */
  for (so = master_so_list (); so; so = so->next)
    gdb_assert (so->objfile == NULL);

  ALL_OBJFILES_SAFE (objfile, temp)
  {
    free_objfile (objfile);
  }
  clear_symtab_users (0);
}

/* A helper function for objfile_relocate1 that relocates a single
   symbol.  */

static void
relocate_one_symbol (struct symbol *sym, struct objfile *objfile,
		     struct section_offsets *delta)
{
  fixup_symbol_section (sym, objfile);

  /* The RS6000 code from which this was taken skipped
     any symbols in STRUCT_DOMAIN or UNDEF_DOMAIN.
     But I'm leaving out that test, on the theory that
     they can't possibly pass the tests below.  */
  if ((SYMBOL_CLASS (sym) == LOC_LABEL
       || SYMBOL_CLASS (sym) == LOC_STATIC)
      && SYMBOL_SECTION (sym) >= 0)
    {
      SYMBOL_VALUE_ADDRESS (sym) += ANOFFSET (delta, SYMBOL_SECTION (sym));
    }
}

/* Relocate OBJFILE to NEW_OFFSETS.  There should be OBJFILE->NUM_SECTIONS
   entries in new_offsets.  SEPARATE_DEBUG_OBJFILE is not touched here.
   Return non-zero iff any change happened.  */

static int
objfile_relocate1 (struct objfile *objfile, 
		   struct section_offsets *new_offsets)
{
  struct obj_section *s;
  struct section_offsets *delta =
    ((struct section_offsets *) 
     alloca (SIZEOF_N_SECTION_OFFSETS (objfile->num_sections)));

  int i;
  int something_changed = 0;

  for (i = 0; i < objfile->num_sections; ++i)
    {
      delta->offsets[i] =
	ANOFFSET (new_offsets, i) - ANOFFSET (objfile->section_offsets, i);
      if (ANOFFSET (delta, i) != 0)
	something_changed = 1;
    }
  if (!something_changed)
    return 0;

  /* OK, get all the symtabs.  */
  {
    struct symtab *s;

    ALL_OBJFILE_SYMTABS (objfile, s)
    {
      struct linetable *l;
      struct blockvector *bv;
      int i;

      /* First the line table.  */
      l = LINETABLE (s);
      if (l)
	{
	  for (i = 0; i < l->nitems; ++i)
	    l->item[i].pc += ANOFFSET (delta, s->block_line_section);
	}

      /* Don't relocate a shared blockvector more than once.  */
      if (!s->primary)
	continue;

      bv = BLOCKVECTOR (s);
      if (BLOCKVECTOR_MAP (bv))
	addrmap_relocate (BLOCKVECTOR_MAP (bv),
			  ANOFFSET (delta, s->block_line_section));

      for (i = 0; i < BLOCKVECTOR_NBLOCKS (bv); ++i)
	{
	  struct block *b;
	  struct symbol *sym;
	  struct dict_iterator iter;

	  b = BLOCKVECTOR_BLOCK (bv, i);
	  BLOCK_START (b) += ANOFFSET (delta, s->block_line_section);
	  BLOCK_END (b) += ANOFFSET (delta, s->block_line_section);

	  /* We only want to iterate over the local symbols, not any
	     symbols in included symtabs.  */
	  ALL_DICT_SYMBOLS (BLOCK_DICT (b), iter, sym)
	    {
	      relocate_one_symbol (sym, objfile, delta);
	    }
	}
    }
  }

  /* Relocate isolated symbols.  */
  {
    struct symbol *iter;

    for (iter = objfile->template_symbols; iter; iter = iter->hash_next)
      relocate_one_symbol (iter, objfile, delta);
  }

  if (objfile->psymtabs_addrmap)
    addrmap_relocate (objfile->psymtabs_addrmap,
		      ANOFFSET (delta, SECT_OFF_TEXT (objfile)));

  if (objfile->sf)
    objfile->sf->qf->relocate (objfile, new_offsets, delta);

  {
    struct minimal_symbol *msym;

    ALL_OBJFILE_MSYMBOLS (objfile, msym)
      if (SYMBOL_SECTION (msym) >= 0)
      SYMBOL_VALUE_ADDRESS (msym) += ANOFFSET (delta, SYMBOL_SECTION (msym));
  }
  /* Relocating different sections by different amounts may cause the symbols
     to be out of order.  */
  msymbols_sort (objfile);

  if (objfile->ei.entry_point_p)
    {
      /* Relocate ei.entry_point with its section offset, use SECT_OFF_TEXT
	 only as a fallback.  */
      struct obj_section *s;
      s = find_pc_section (objfile->ei.entry_point);
      if (s)
        objfile->ei.entry_point += ANOFFSET (delta, s->the_bfd_section->index);
      else
        objfile->ei.entry_point += ANOFFSET (delta, SECT_OFF_TEXT (objfile));
    }

  {
    int i;

    for (i = 0; i < objfile->num_sections; ++i)
      (objfile->section_offsets)->offsets[i] = ANOFFSET (new_offsets, i);
  }

  /* Rebuild section map next time we need it.  */
  get_objfile_pspace_data (objfile->pspace)->objfiles_changed_p = 1;

  /* Update the table in exec_ops, used to read memory.  */
  ALL_OBJFILE_OSECTIONS (objfile, s)
    {
      int idx = s->the_bfd_section->index;

      exec_set_section_address (bfd_get_filename (objfile->obfd), idx,
				obj_section_addr (s));
    }

  /* Relocating probes.  */
  if (objfile->sf && objfile->sf->sym_probe_fns)
    objfile->sf->sym_probe_fns->sym_relocate_probe (objfile,
						    new_offsets, delta);

  /* Data changed.  */
  return 1;
}

/* Relocate OBJFILE to NEW_OFFSETS.  There should be OBJFILE->NUM_SECTIONS
   entries in new_offsets.  Process also OBJFILE's SEPARATE_DEBUG_OBJFILEs.

   The number and ordering of sections does differ between the two objfiles.
   Only their names match.  Also the file offsets will differ (objfile being
   possibly prelinked but separate_debug_objfile is probably not prelinked) but
   the in-memory absolute address as specified by NEW_OFFSETS must match both
   files.  */

void
objfile_relocate (struct objfile *objfile, struct section_offsets *new_offsets)
{
  struct objfile *debug_objfile;
  int changed = 0;

  changed |= objfile_relocate1 (objfile, new_offsets);

  for (debug_objfile = objfile->separate_debug_objfile;
       debug_objfile;
       debug_objfile = objfile_separate_debug_iterate (objfile, debug_objfile))
    {
      struct section_addr_info *objfile_addrs;
      struct section_offsets *new_debug_offsets;
      struct cleanup *my_cleanups;

      objfile_addrs = build_section_addr_info_from_objfile (objfile);
      my_cleanups = make_cleanup (xfree, objfile_addrs);

      /* Here OBJFILE_ADDRS contain the correct absolute addresses, the
	 relative ones must be already created according to debug_objfile.  */

      addr_info_make_relative (objfile_addrs, debug_objfile->obfd);

      gdb_assert (debug_objfile->num_sections
		  == bfd_count_sections (debug_objfile->obfd));
      new_debug_offsets = 
	xmalloc (SIZEOF_N_SECTION_OFFSETS (debug_objfile->num_sections));
      make_cleanup (xfree, new_debug_offsets);
      relative_addr_info_to_section_offsets (new_debug_offsets,
					     debug_objfile->num_sections,
					     objfile_addrs);

      changed |= objfile_relocate1 (debug_objfile, new_debug_offsets);

      do_cleanups (my_cleanups);
    }

  /* Relocate breakpoints as necessary, after things are relocated.  */
  if (changed)
    breakpoint_re_set ();
}

/* Return non-zero if OBJFILE has partial symbols.  */

int
objfile_has_partial_symbols (struct objfile *objfile)
{
  if (!objfile->sf)
    return 0;

  /* If we have not read psymbols, but we have a function capable of reading
     them, then that is an indication that they are in fact available.  Without
     this function the symbols may have been already read in but they also may
     not be present in this objfile.  */
  if ((objfile->flags & OBJF_PSYMTABS_READ) == 0
      && objfile->sf->sym_read_psymbols != NULL)
    return 1;

  return objfile->sf->qf->has_symbols (objfile);
}

/* Return non-zero if OBJFILE has full symbols.  */

int
objfile_has_full_symbols (struct objfile *objfile)
{
  return objfile->symtabs != NULL;
}

/* Return non-zero if OBJFILE has full or partial symbols, either directly
   or through a separate debug file.  */

int
objfile_has_symbols (struct objfile *objfile)
{
  struct objfile *o;

  for (o = objfile; o; o = objfile_separate_debug_iterate (objfile, o))
    if (objfile_has_partial_symbols (o) || objfile_has_full_symbols (o))
      return 1;
  return 0;
}


/* Many places in gdb want to test just to see if we have any partial
   symbols available.  This function returns zero if none are currently
   available, nonzero otherwise.  */

int
have_partial_symbols (void)
{
  struct objfile *ofp;

  ALL_OBJFILES (ofp)
  {
    if (objfile_has_partial_symbols (ofp))
      return 1;
  }
  return 0;
}

/* Many places in gdb want to test just to see if we have any full
   symbols available.  This function returns zero if none are currently
   available, nonzero otherwise.  */

int
have_full_symbols (void)
{
  struct objfile *ofp;

  ALL_OBJFILES (ofp)
  {
    if (objfile_has_full_symbols (ofp))
      return 1;
  }
  return 0;
}


/* This operations deletes all objfile entries that represent solibs that
   weren't explicitly loaded by the user, via e.g., the add-symbol-file
   command.  */

void
objfile_purge_solibs (void)
{
  struct objfile *objf;
  struct objfile *temp;

  ALL_OBJFILES_SAFE (objf, temp)
  {
    /* We assume that the solib package has been purged already, or will
       be soon.  */

    if (!(objf->flags & OBJF_USERLOADED) && (objf->flags & OBJF_SHARED))
      free_objfile (objf);
  }
}


/* Many places in gdb want to test just to see if we have any minimal
   symbols available.  This function returns zero if none are currently
   available, nonzero otherwise.  */

int
have_minimal_symbols (void)
{
  struct objfile *ofp;

  ALL_OBJFILES (ofp)
  {
    if (ofp->minimal_symbol_count > 0)
      {
	return 1;
      }
  }
  return 0;
}

/* Qsort comparison function.  */

static int
qsort_cmp (const void *a, const void *b)
{
  const struct obj_section *sect1 = *(const struct obj_section **) a;
  const struct obj_section *sect2 = *(const struct obj_section **) b;
  const CORE_ADDR sect1_addr = obj_section_addr (sect1);
  const CORE_ADDR sect2_addr = obj_section_addr (sect2);

  if (sect1_addr < sect2_addr)
    return -1;
  else if (sect1_addr > sect2_addr)
    return 1;
  else
    {
      /* Sections are at the same address.  This could happen if
	 A) we have an objfile and a separate debuginfo.
	 B) we are confused, and have added sections without proper relocation,
	 or something like that.  */

      const struct objfile *const objfile1 = sect1->objfile;
      const struct objfile *const objfile2 = sect2->objfile;

      if (objfile1->separate_debug_objfile == objfile2
	  || objfile2->separate_debug_objfile == objfile1)
	{
	  /* Case A.  The ordering doesn't matter: separate debuginfo files
	     will be filtered out later.  */

	  return 0;
	}

      /* Case B.  Maintain stable sort order, so bugs in GDB are easier to
	 triage.  This section could be slow (since we iterate over all
	 objfiles in each call to qsort_cmp), but this shouldn't happen
	 very often (GDB is already in a confused state; one hopes this
	 doesn't happen at all).  If you discover that significant time is
	 spent in the loops below, do 'set complaints 100' and examine the
	 resulting complaints.  */

      if (objfile1 == objfile2)
	{
	  /* Both sections came from the same objfile.  We are really confused.
	     Sort on sequence order of sections within the objfile.  */

	  const struct obj_section *osect;

	  ALL_OBJFILE_OSECTIONS (objfile1, osect)
	    if (osect == sect1)
	      return -1;
	    else if (osect == sect2)
	      return 1;

	  /* We should have found one of the sections before getting here.  */
	  gdb_assert_not_reached ("section not found");
	}
      else
	{
	  /* Sort on sequence number of the objfile in the chain.  */

	  const struct objfile *objfile;

	  ALL_OBJFILES (objfile)
	    if (objfile == objfile1)
	      return -1;
	    else if (objfile == objfile2)
	      return 1;

	  /* We should have found one of the objfiles before getting here.  */
	  gdb_assert_not_reached ("objfile not found");
	}
    }

  /* Unreachable.  */
  gdb_assert_not_reached ("unexpected code path");
  return 0;
}

/* Select "better" obj_section to keep.  We prefer the one that came from
   the real object, rather than the one from separate debuginfo.
   Most of the time the two sections are exactly identical, but with
   prelinking the .rel.dyn section in the real object may have different
   size.  */

static struct obj_section *
preferred_obj_section (struct obj_section *a, struct obj_section *b)
{
  gdb_assert (obj_section_addr (a) == obj_section_addr (b));
  gdb_assert ((a->objfile->separate_debug_objfile == b->objfile)
	      || (b->objfile->separate_debug_objfile == a->objfile));
  gdb_assert ((a->objfile->separate_debug_objfile_backlink == b->objfile)
	      || (b->objfile->separate_debug_objfile_backlink == a->objfile));

  if (a->objfile->separate_debug_objfile != NULL)
    return a;
  return b;
}

/* Return 1 if SECTION should be inserted into the section map.
   We want to insert only non-overlay and non-TLS section.  */

static int
insert_section_p (const struct bfd *abfd,
		  const struct bfd_section *section)
{
  const bfd_vma lma = bfd_section_lma (abfd, section);

  if (overlay_debugging && lma != 0 && lma != bfd_section_vma (abfd, section)
      && (bfd_get_file_flags (abfd) & BFD_IN_MEMORY) == 0)
    /* This is an overlay section.  IN_MEMORY check is needed to avoid
       discarding sections from the "system supplied DSO" (aka vdso)
       on some Linux systems (e.g. Fedora 11).  */
    return 0;
  if ((bfd_get_section_flags (abfd, section) & SEC_THREAD_LOCAL) != 0)
    /* This is a TLS section.  */
    return 0;

  return 1;
}

/* Filter out overlapping sections where one section came from the real
   objfile, and the other from a separate debuginfo file.
   Return the size of table after redundant sections have been eliminated.  */

static int
filter_debuginfo_sections (struct obj_section **map, int map_size)
{
  int i, j;

  for (i = 0, j = 0; i < map_size - 1; i++)
    {
      struct obj_section *const sect1 = map[i];
      struct obj_section *const sect2 = map[i + 1];
      const struct objfile *const objfile1 = sect1->objfile;
      const struct objfile *const objfile2 = sect2->objfile;
      const CORE_ADDR sect1_addr = obj_section_addr (sect1);
      const CORE_ADDR sect2_addr = obj_section_addr (sect2);

      if (sect1_addr == sect2_addr
	  && (objfile1->separate_debug_objfile == objfile2
	      || objfile2->separate_debug_objfile == objfile1))
	{
	  map[j++] = preferred_obj_section (sect1, sect2);
	  ++i;
	}
      else
	map[j++] = sect1;
    }

  if (i < map_size)
    {
      gdb_assert (i == map_size - 1);
      map[j++] = map[i];
    }

  /* The map should not have shrunk to less than half the original size.  */
  gdb_assert (map_size / 2 <= j);

  return j;
}

/* Filter out overlapping sections, issuing a warning if any are found.
   Overlapping sections could really be overlay sections which we didn't
   classify as such in insert_section_p, or we could be dealing with a
   corrupt binary.  */

static int
filter_overlapping_sections (struct obj_section **map, int map_size)
{
  int i, j;

  for (i = 0, j = 0; i < map_size - 1; )
    {
      int k;

      map[j++] = map[i];
      for (k = i + 1; k < map_size; k++)
	{
	  struct obj_section *const sect1 = map[i];
	  struct obj_section *const sect2 = map[k];
	  const CORE_ADDR sect1_addr = obj_section_addr (sect1);
	  const CORE_ADDR sect2_addr = obj_section_addr (sect2);
	  const CORE_ADDR sect1_endaddr = obj_section_endaddr (sect1);

	  gdb_assert (sect1_addr <= sect2_addr);

	  if (sect1_endaddr <= sect2_addr)
	    break;
	  else
	    {
	      /* We have an overlap.  Report it.  */

	      struct objfile *const objf1 = sect1->objfile;
	      struct objfile *const objf2 = sect2->objfile;

	      const struct bfd *const abfd1 = objf1->obfd;
	      const struct bfd *const abfd2 = objf2->obfd;

	      const struct bfd_section *const bfds1 = sect1->the_bfd_section;
	      const struct bfd_section *const bfds2 = sect2->the_bfd_section;

	      const CORE_ADDR sect2_endaddr = obj_section_endaddr (sect2);

	      struct gdbarch *const gdbarch = get_objfile_arch (objf1);

	      complaint (&symfile_complaints,
			 _("unexpected overlap between:\n"
			   " (A) section `%s' from `%s' [%s, %s)\n"
			   " (B) section `%s' from `%s' [%s, %s).\n"
			   "Will ignore section B"),
			 bfd_section_name (abfd1, bfds1), objf1->name,
			 paddress (gdbarch, sect1_addr),
			 paddress (gdbarch, sect1_endaddr),
			 bfd_section_name (abfd2, bfds2), objf2->name,
			 paddress (gdbarch, sect2_addr),
			 paddress (gdbarch, sect2_endaddr));
	    }
	}
      i = k;
    }

  if (i < map_size)
    {
      gdb_assert (i == map_size - 1);
      map[j++] = map[i];
    }

  return j;
}


/* Update PMAP, PMAP_SIZE with sections from all objfiles, excluding any
   TLS, overlay and overlapping sections.  */

static void
update_section_map (struct program_space *pspace,
		    struct obj_section ***pmap, int *pmap_size)
{
  int alloc_size, map_size, i;
  struct obj_section *s, **map;
  struct objfile *objfile;

  gdb_assert (get_objfile_pspace_data (pspace)->objfiles_changed_p != 0);

  map = *pmap;
  xfree (map);

  alloc_size = 0;
  ALL_PSPACE_OBJFILES (pspace, objfile)
    ALL_OBJFILE_OSECTIONS (objfile, s)
      if (insert_section_p (objfile->obfd, s->the_bfd_section))
	alloc_size += 1;

  /* This happens on detach/attach (e.g. in gdb.base/attach.exp).  */
  if (alloc_size == 0)
    {
      *pmap = NULL;
      *pmap_size = 0;
      return;
    }

  map = xmalloc (alloc_size * sizeof (*map));

  i = 0;
  ALL_PSPACE_OBJFILES (pspace, objfile)
    ALL_OBJFILE_OSECTIONS (objfile, s)
      if (insert_section_p (objfile->obfd, s->the_bfd_section))
	map[i++] = s;

  qsort (map, alloc_size, sizeof (*map), qsort_cmp);
  map_size = filter_debuginfo_sections(map, alloc_size);
  map_size = filter_overlapping_sections(map, map_size);

  if (map_size < alloc_size)
    /* Some sections were eliminated.  Trim excess space.  */
    map = xrealloc (map, map_size * sizeof (*map));
  else
    gdb_assert (alloc_size == map_size);

  *pmap = map;
  *pmap_size = map_size;
}

/* Bsearch comparison function.  */

static int
bsearch_cmp (const void *key, const void *elt)
{
  const CORE_ADDR pc = *(CORE_ADDR *) key;
  const struct obj_section *section = *(const struct obj_section **) elt;

  if (pc < obj_section_addr (section))
    return -1;
  if (pc < obj_section_endaddr (section))
    return 0;
  return 1;
}

/* Returns a section whose range includes PC or NULL if none found.   */

struct obj_section *
find_pc_section (CORE_ADDR pc)
{
  struct objfile_pspace_info *pspace_info;
  struct obj_section *s, **sp;

  /* Check for mapped overlay section first.  */
  s = find_pc_mapped_section (pc);
  if (s)
    return s;

  pspace_info = get_objfile_pspace_data (current_program_space);
  if (pspace_info->objfiles_changed_p != 0)
    {
      update_section_map (current_program_space,
			  &pspace_info->sections,
			  &pspace_info->num_sections);

      /* Don't need updates to section map until objfiles are added,
         removed or relocated.  */
      pspace_info->objfiles_changed_p = 0;
    }

  /* The C standard (ISO/IEC 9899:TC2) requires the BASE argument to
     bsearch be non-NULL.  */
  if (pspace_info->sections == NULL)
    {
      gdb_assert (pspace_info->num_sections == 0);
      return NULL;
    }

  sp = (struct obj_section **) bsearch (&pc,
					pspace_info->sections,
					pspace_info->num_sections,
					sizeof (*pspace_info->sections),
					bsearch_cmp);
  if (sp != NULL)
    return *sp;
  return NULL;
}


/* In SVR4, we recognize a trampoline by it's section name. 
   That is, if the pc is in a section named ".plt" then we are in
   a trampoline.  */

int
in_plt_section (CORE_ADDR pc, char *name)
{
  struct obj_section *s;
  int retval = 0;

  s = find_pc_section (pc);

  retval = (s != NULL
	    && s->the_bfd_section->name != NULL
	    && strcmp (s->the_bfd_section->name, ".plt") == 0);
  return (retval);
}


/* Keep a registry of per-objfile data-pointers required by other GDB
   modules.  */

struct objfile_data
{
  unsigned index;
  void (*save) (struct objfile *, void *);
  void (*free) (struct objfile *, void *);
};

struct objfile_data_registration
{
  struct objfile_data *data;
  struct objfile_data_registration *next;
};
  
struct objfile_data_registry
{
  struct objfile_data_registration *registrations;
  unsigned num_registrations;
};

static struct objfile_data_registry objfile_data_registry = { NULL, 0 };

const struct objfile_data *
register_objfile_data_with_cleanup (void (*save) (struct objfile *, void *),
				    void (*free) (struct objfile *, void *))
{
  struct objfile_data_registration **curr;

  /* Append new registration.  */
  for (curr = &objfile_data_registry.registrations;
       *curr != NULL; curr = &(*curr)->next);

  *curr = XMALLOC (struct objfile_data_registration);
  (*curr)->next = NULL;
  (*curr)->data = XMALLOC (struct objfile_data);
  (*curr)->data->index = objfile_data_registry.num_registrations++;
  (*curr)->data->save = save;
  (*curr)->data->free = free;

  return (*curr)->data;
}

const struct objfile_data *
register_objfile_data (void)
{
  return register_objfile_data_with_cleanup (NULL, NULL);
}

static void
objfile_alloc_data (struct objfile *objfile)
{
  gdb_assert (objfile->data == NULL);
  objfile->num_data = objfile_data_registry.num_registrations;
  objfile->data = XCALLOC (objfile->num_data, void *);
}

static void
objfile_free_data (struct objfile *objfile)
{
  gdb_assert (objfile->data != NULL);
  clear_objfile_data (objfile);
  xfree (objfile->data);
  objfile->data = NULL;
}

void
clear_objfile_data (struct objfile *objfile)
{
  struct objfile_data_registration *registration;
  int i;

  gdb_assert (objfile->data != NULL);

  /* Process all the save handlers.  */

  for (registration = objfile_data_registry.registrations, i = 0;
       i < objfile->num_data;
       registration = registration->next, i++)
    if (objfile->data[i] != NULL && registration->data->save != NULL)
      registration->data->save (objfile, objfile->data[i]);

  /* Now process all the free handlers.  */

  for (registration = objfile_data_registry.registrations, i = 0;
       i < objfile->num_data;
       registration = registration->next, i++)
    if (objfile->data[i] != NULL && registration->data->free != NULL)
      registration->data->free (objfile, objfile->data[i]);

  memset (objfile->data, 0, objfile->num_data * sizeof (void *));
}

void
set_objfile_data (struct objfile *objfile, const struct objfile_data *data,
		  void *value)
{
  gdb_assert (data->index < objfile->num_data);
  objfile->data[data->index] = value;
}

void *
objfile_data (struct objfile *objfile, const struct objfile_data *data)
{
  gdb_assert (data->index < objfile->num_data);
  return objfile->data[data->index];
}

/* Set objfiles_changed_p so section map will be rebuilt next time it
   is used.  Called by reread_symbols.  */

void
objfiles_changed (void)
{
  /* Rebuild section map next time we need it.  */
  get_objfile_pspace_data (current_program_space)->objfiles_changed_p = 1;
}

/* Close ABFD, and warn if that fails.  */

int
gdb_bfd_close_or_warn (struct bfd *abfd)
{
  int ret;
  char *name = bfd_get_filename (abfd);

  ret = bfd_close (abfd);

  if (!ret)
    warning (_("cannot close \"%s\": %s"),
	     name, bfd_errmsg (bfd_get_error ()));

  return ret;
}

/* Add reference to ABFD.  Returns ABFD.  */
struct bfd *
gdb_bfd_ref (struct bfd *abfd)
{
  int *p_refcount;

  if (abfd == NULL)
    return NULL;

  p_refcount = bfd_usrdata (abfd);

  if (p_refcount != NULL)
    {
      *p_refcount += 1;
      return abfd;
    }

  p_refcount = xmalloc (sizeof (*p_refcount));
  *p_refcount = 1;
  bfd_usrdata (abfd) = p_refcount;

  return abfd;
}

/* Unreference and possibly close ABFD.  */
void
gdb_bfd_unref (struct bfd *abfd)
{
  int *p_refcount;
  char *name;

  if (abfd == NULL)
    return;

  p_refcount = bfd_usrdata (abfd);

  /* Valid range for p_refcount: a pointer to int counter, which has a
     value of 1 (single owner) or 2 (shared).  */
  gdb_assert (*p_refcount == 1 || *p_refcount == 2);

  *p_refcount -= 1;
  if (*p_refcount > 0)
    return;

  xfree (p_refcount);
  bfd_usrdata (abfd) = NULL;  /* Paranoia.  */

  name = bfd_get_filename (abfd);
  gdb_bfd_close_or_warn (abfd);
  xfree (name);
}

/* The default implementation for the "iterate_over_objfiles_in_search_order"
   gdbarch method.  It is equivalent to use the ALL_OBJFILES macro,
   searching the objfiles in the order they are stored internally,
   ignoring CURRENT_OBJFILE.

   On most platorms, it should be close enough to doing the best
   we can without some knowledge specific to the architecture.  */

void
default_iterate_over_objfiles_in_search_order
  (struct gdbarch *gdbarch,
   iterate_over_objfiles_in_search_order_cb_ftype *cb,
   void *cb_data, struct objfile *current_objfile)
{
  int stop = 0;
  struct objfile *objfile;

  ALL_OBJFILES (objfile)
    {
       stop = cb (objfile, cb_data);
       if (stop)
	 return;
    }
}

/* Provide a prototype to silence -Wmissing-prototypes.  */
extern initialize_file_ftype _initialize_objfiles;

void
_initialize_objfiles (void)
{
  objfiles_pspace_data
    = register_program_space_data_with_cleanup (objfiles_pspace_data_cleanup);
}
