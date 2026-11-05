/* MCPX rounding: sample/filters round halfway UP, not ties-to-even.
 * Source: retail Xbox tests by raddad772, xemu-project/xemu#3105
 * commits 06ef59e (filter precision) and 45af4ac (mixbin precision).
 * Use double intermediates so a 1/16-24-bit step survives before the
 * final float conversion; values are not saturated by the filter.
 */
#ifndef HAKUX_MCPX_ROUND_H
#define HAKUX_MCPX_ROUND_H
#include <math.h>
static inline double mcpx_round_units(double value, double units)
{
    return floor(value * units + 0.5) / units;
}
static inline double mcpx_round_filter(double value)
{
    return mcpx_round_units(value, 134217728.0); /* 16 * 2^23 */
}
#endif
