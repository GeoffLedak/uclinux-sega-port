/* mpq_set_num(dest,num) -- Set the numerator of DEST from NUM.

Copyright (C) 1991, 1994, 1995 Free Software Foundation, Inc.

This file is part of the GNU MP Library.

The GNU MP Library is free software; you can redistribute it and/or modify
it under the terms of the GNU Library General Public License as published by
the Free Software Foundation; either version 2 of the License, or (at your
option) any later version.

The GNU MP Library is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Library General Public
License for more details.

You should have received a copy of the GNU Library General Public License
along with the GNU MP Library; see the file COPYING.LIB.  If not, write to
the Free Software Foundation, Inc., 59 Temple Place - Suite 330, Boston,
MA 02111-1307, USA. */

#include "gmp.h"
#include "gmp-impl.h"

void
#if __STDC__
mpq_set_num (MP_RAT *dest, const MP_INT *num)
#else
mpq_set_num (dest, num)
     MP_RAT *dest;
     const MP_INT *num;
#endif
{
  mp_size_t size = num->_mp_size;
  mp_size_t abs_size = ABS (size);

  if (dest->_mp_num._mp_alloc < abs_size)
    _mpz_realloc (&(dest->_mp_num), abs_size);

  MPN_COPY (dest->_mp_num._mp_d, num->_mp_d, abs_size);
  dest->_mp_num._mp_size = size;
}
