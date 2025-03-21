#ifndef COMPAT_BUILD_BUG_H
#define COMPAT_BUILD_BUG_H

/* Include the autogenerated header file */
#include "../../compat/config.h"

#ifdef HAVE_BUILD_BUG_H
#include_next <linux/build_bug.h>
#endif

#ifndef BUILD_BUG_ON_MSG
#define BUILD_BUG_ON_MSG(cond, msg) do { } while(0)
#endif

/* Force a compilation error if a constant expression is not a power of 2 */
#ifndef __BUILD_BUG_ON_NOT_POWER_OF_2
#define __BUILD_BUG_ON_NOT_POWER_OF_2(n)        \
        BUILD_BUG_ON(((n) & ((n) - 1)) != 0)
#endif

#endif /* COMPAT_BUILD_BUG_H */
