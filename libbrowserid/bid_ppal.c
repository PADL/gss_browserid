/*
 * Copyright (C) 2013 PADL Software Pty Ltd.
 * All rights reserved.
 * Use is subject to license.
 */

#include "bid_private.h"

BIDError
_BIDGetCurrentJsonTimestamp(
    BIDContext context BID_UNUSED,
    json_t **pTs)
{
    struct timeval tv;
    json_int_t ms;

    gettimeofday(&tv, NULL);

    ms = tv.tv_sec * 1000;
    ms += tv.tv_usec / 1000;

    *pTs = json_integer(ms);

    return (*pTs == NULL) ? BID_S_NO_MEMORY : BID_S_OK;
}


