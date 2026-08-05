/*
 * exfat-handler - pure timestamp and UTC-offset helpers
 *
 * Copyright (C) 2026 The AROS Development Team
 *
 * Kept independent of AROS library calls so the arithmetic is executable in
 * the hosted ASan/UBSan suite and identical on little- and big-endian targets.
 */

#ifndef EXFAT_TIME_H
#define EXFAT_TIME_H

#define EXFAT_UTC_VALID 0x80U
#define EXFAT_MINUTES_PER_DAY 1440L
#define EXFAT_DAYS_1978_TO_1980 730L

static inline int exfat_time_leap_year(ULONG year)
{
    return (year % 4U == 0U && year % 100U != 0U) || year % 400U == 0U;
}

static inline ULONG exfat_time_days_in_month(ULONG year, ULONG month)
{
    static const UBYTE days[12] =
        { 31,28,31,30,31,30,31,31,30,31,30,31 };

    if (month < 1U || month > 12U)
        return 0;
    return (ULONG)days[month - 1U]
        + ((month == 2U && exfat_time_leap_year(year)) ? 1U : 0U);
}

/* Adjust an AmigaDOS local DateStamp by a bounded number of minutes without
   multiplying ds_Days, which may be a 32-bit LONG. */
static inline int exfat_time_adjust_minutes(LONG *days, LONG *minute,
    LONG adjustment)
{
    LONG d, m;

    if (days == 0 || minute == 0 || *days < 0
        || *minute < 0 || *minute >= EXFAT_MINUTES_PER_DAY)
        return 0;
    d = *days;
    m = *minute;
    while (adjustment < 0)
    {
        LONG take = adjustment < -EXFAT_MINUTES_PER_DAY
            ? EXFAT_MINUTES_PER_DAY : -adjustment;
        if (m < take)
        {
            if (d == 0)
                return 0;
            d--;
            m += EXFAT_MINUTES_PER_DAY;
        }
        m -= take;
        adjustment += take;
    }
    while (adjustment > 0)
    {
        LONG take = adjustment > EXFAT_MINUTES_PER_DAY
            ? EXFAT_MINUTES_PER_DAY : adjustment;
        if (m >= EXFAT_MINUTES_PER_DAY - take)
        {
            if (d == 0x7fffffffL)
                return 0;
            d++;
            m -= EXFAT_MINUTES_PER_DAY;
        }
        m += take;
        adjustment -= take;
    }
    *days = d;
    *minute = m;
    return 1;
}

/* Pack an AmigaDOS DateStamp, whose epoch is 1978-01-01, as the exFAT local
   Timestamp plus 10msIncrement. */
static inline int exfat_time_pack(LONG days, LONG minute, LONG tick,
    ULONG ticks_per_second, ULONG *packed, UBYTE *ten_ms)
{
    ULONG year = 1978U, month, day, left, seconds, fraction;

    if (packed == 0 || ten_ms == 0 || days < 0 || minute < 0
        || minute >= EXFAT_MINUTES_PER_DAY || tick < 0
        || ticks_per_second == 0U || 100U % ticks_per_second != 0U
        || (ULONG)tick >= 60U * ticks_per_second)
        return 0;

    while (year <= 2107U)
    {
        ULONG year_days = exfat_time_leap_year(year) ? 366U : 365U;
        if ((ULONG)days < year_days)
            break;
        days -= (LONG)year_days;
        year++;
    }
    if (year < 1980U || year > 2107U)
        return 0;

    left = (ULONG)days;
    for (month = 1U; month <= 12U; month++)
    {
        ULONG mdays = exfat_time_days_in_month(year, month);
        if (left < mdays)
            break;
        left -= mdays;
    }
    if (month > 12U)
        return 0;
    day = left + 1U;
    seconds = (ULONG)tick / ticks_per_second;
    fraction = ((ULONG)tick % ticks_per_second)
        * (100U / ticks_per_second);
    *packed = ((year - 1980U) << 25) | (month << 21) | (day << 16)
        | ((ULONG)minute / 60U << 11)
        | ((ULONG)minute % 60U << 5) | (seconds / 2U);
    *ten_ms = (UBYTE)((seconds & 1U) * 100U + fraction);
    return 1;
}

/* Decode and validate every calendar field.  In particular, February 31,
   DoubleSeconds 30/31, and 10msIncrement > 199 are not plausible dates. */
static inline int exfat_time_unpack(ULONG packed, UBYTE ten_ms,
    ULONG ticks_per_second, LONG *days, LONG *minute, LONG *tick)
{
    ULONG year = 1980U + (packed >> 25);
    ULONG month = (packed >> 21) & 15U;
    ULONG day = (packed >> 16) & 31U;
    ULONG hour = (packed >> 11) & 31U;
    ULONG min = (packed >> 5) & 63U;
    ULONG double_seconds = packed & 31U;
    ULONG d = (ULONG)EXFAT_DAYS_1978_TO_1980, y, m;

    if (days == 0 || minute == 0 || tick == 0 || ticks_per_second == 0U
        || 100U % ticks_per_second != 0U || month < 1U || month > 12U
        || day < 1U || day > exfat_time_days_in_month(year, month)
        || hour >= 24U || min >= 60U || double_seconds >= 30U
        || ten_ms > 199U)
        return 0;

    for (y = 1980U; y < year; y++)
        d += exfat_time_leap_year(y) ? 366U : 365U;
    for (m = 1U; m < month; m++)
        d += exfat_time_days_in_month(year, m);
    d += day - 1U;

    *days = (LONG)d;
    *minute = (LONG)(hour * 60U + min);
    *tick = (LONG)(double_seconds * 2U * ticks_per_second
        + (ULONG)ten_ms * ticks_per_second / 100U);
    return 1;
}

/* Decode the signed seven-bit count of 15-minute intervals. */
static inline int exfat_time_decode_utc(UBYTE raw, LONG *local_from_utc)
{
    LONG quarters;

    if ((raw & EXFAT_UTC_VALID) == 0U || local_from_utc == 0)
        return 0;
    quarters = (LONG)(raw & 0x7fU);
    if ((quarters & 0x40L) != 0)
        quarters -= 0x80L;
    *local_from_utc = quarters * 15L;
    return 1;
}

/* AROS stores minutes from local time to GMT; exFAT stores the opposite.
   A non-quarter-hour or out-of-range locale is represented as UTC, as the
   exFAT specification requires, so the caller first adjusts local by the
   returned local_to_stored minutes. */
static inline UBYTE exfat_time_encode_utc(LONG local_to_gmt,
    LONG *local_to_stored)
{
    LONG quarters;

    if (local_to_stored == 0)
        return 0;
    *local_to_stored = 0;
    if (local_to_gmt % 15L != 0)
    {
        *local_to_stored = local_to_gmt;
        return EXFAT_UTC_VALID; /* store the adjusted timestamp as UTC */
    }
    quarters = -local_to_gmt / 15L;
    if (quarters < -64L || quarters > 63L)
    {
        *local_to_stored = local_to_gmt;
        return EXFAT_UTC_VALID;
    }
    return (UBYTE)(EXFAT_UTC_VALID | ((ULONG)quarters & 0x7fU));
}

/* Convert a valid stored local offset to the current AROS local time. */
static inline int exfat_time_display_adjustment(UBYTE raw,
    LONG current_local_to_gmt, LONG *adjustment)
{
    LONG stored_local_from_utc;

    if (adjustment == 0
        || !exfat_time_decode_utc(raw, &stored_local_from_utc))
        return 0;
    *adjustment = -stored_local_from_utc - current_local_to_gmt;
    return 1;
}

#endif /* EXFAT_TIME_H */
