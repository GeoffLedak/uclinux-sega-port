 /* mpq_get_num(num,rat_src) -- Set NUM to the numerator of RAT_SRC.

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
mpq_get_num (MP_INT *num, const MP_RAT *src)
#else
mpq_get_num (num, src)
     MP_INT *num;
     const MP_RAT *src;
#endif
{
  mp_size_t size = src->_mp_num._mp_size;
  mp_size_t abs_size = ABS (size);

  if (num->_mp_alloc < abs_size)
    _mpz_realloc (num, abs_size);

  MPN_COPY (num->_mp_d, src->_mp_num._mp_d, abs_size);
  num->_mp_size = size;
}
