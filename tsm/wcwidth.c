/* wcwidth from musl libc
 *
 * Copyright © 2005-2014 Rich Felker, et al.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <wchar.h>

static const unsigned char table[] = {
#include "nonspacing.h"
};

static const unsigned char wtable[] = {
#include "wide.h"
};

int tsm_wcwidth(wchar_t wc)
{
	if ((unsigned)wc < 0xffU)
		return (wc+1 & 0x7f) >= 0x21 ? 1 : wc ? -1 : 0;
	if ((wc & 0xfffeffffU) < 0xfffe) {
		if ((table[table[wc>>8]*32+((wc&255)>>3)]>>(wc&7))&1)
			return 0;
		if ((wtable[wtable[wc>>8]*32+((wc&255)>>3)]>>(wc&7))&1)
			return 2;
		return 1;
	}
	if ((wc & 0xfffe) == 0xfffe)
		return -1;
	if (wc-0x20000U < 0x20000)
		return 2;
	if (wc == 0xe0001 || wc-0xe0020U < 0x5f || wc-0xe0100 < 0xef)
		return 0;
	return 1;
}
