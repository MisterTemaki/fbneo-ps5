/* C-locale entry points required by native FreeBSD headers alongside ps5rt_locale.
 * SPDX-License-Identifier: MIT */
#define __RUNETYPE_INTERNAL 1
#include <ctype.h>
#include <runetype.h>
#include <xlocale.h>

unsigned long ___runetype_l(__ct_rune_t, locale_t);
__ct_rune_t ___tolower_l(__ct_rune_t, locale_t);
__ct_rune_t ___toupper_l(__ct_rune_t, locale_t);
unsigned long ___runetype(__ct_rune_t value) { return ___runetype_l(value, (locale_t)0); }
__ct_rune_t ___tolower(__ct_rune_t value) { return ___tolower_l(value, (locale_t)0); }
__ct_rune_t ___toupper(__ct_rune_t value) { return ___toupper_l(value, (locale_t)0); }
int __tolower_l(int value, locale_t locale) { return ___tolower_l(value, locale); }
int __toupper_l(int value, locale_t locale) { return ___toupper_l(value, locale); }
int ___mb_cur_max(void) { return 1; }
const _RuneLocale* __getCurrentRuneLocale(void) { return &_DefaultRuneLocale; }
locale_t uselocale(locale_t locale) { (void)locale; return LC_GLOBAL_LOCALE; }
