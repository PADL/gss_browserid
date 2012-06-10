/*
 * Copyright (C) 2012 PADL Software Pty Ltd.
 * All rights reserved.
 * Use is subject to license.
 *
 * CONFIDENTIAL
 *
 * msetup flags helper functions
 */

#include "msetup.h"

DWORD
MsQuerySspFlags(HKEY hKey, DWORD *pdwSspFlags)
{
    DWORD lResult;
    DWORD dwType = REG_DWORD;
    DWORD dwSize = sizeof(*pdwSspFlags);

    lResult = RegQueryValueEx(hKey, L"Flags", NULL, &dwType,
                              (PBYTE)pdwSspFlags, &dwSize);
    if (lResult == ERROR_SUCCESS)
        *pdwSspFlags &= GSSP_FLAG_REG_MASK;

    return lResult;
}

DWORD
MsModifySspFlags(
    HKEY hKey,
    SSP_FLAG_OP fOp,
    DWORD dwSspFlags)
{
    DWORD lResult;
    DWORD dwRegSspFlags = 0;

    /*
     * This isn't transaction safe, but using the transaction manager
     * for setting flags seems overkill. TODO for a future version.
     */

    if (fOp != SSP_FLAG_SET) {
        lResult = MsQuerySspFlags(hKey, &dwRegSspFlags);
        if (lResult != ERROR_SUCCESS &&
            lResult != ERROR_FILE_NOT_FOUND)
            return lResult;
    }

    switch (fOp) {
    case SSP_FLAG_SET:
        dwRegSspFlags = dwSspFlags;
        break;
    case SSP_FLAG_ADD:
        dwRegSspFlags |= dwSspFlags;
        break;
    case SSP_FLAG_DELETE:
        dwRegSspFlags &= ~(dwSspFlags);
        break;
    }

    lResult = RegSetValueEx(hKey, L"Flags", 0, REG_DWORD,
                            (PBYTE)&dwRegSspFlags, sizeof(dwRegSspFlags));

    return lResult;
}

static struct {
    DWORD Flag;
    LPCWSTR String;
    LPCWSTR Description;
} sspFlagMap[] = {
    { GSSP_FLAG_DEBUG,                  L"Debug",            L"Enable debugging"            },
    { GSSP_FLAG_DISABLE_SPNEGO,         L"DisableSpnego"     L"Do not advertise mechanism through Negotiate" },
    { GSSP_FLAG_DISABLE_NEGOEX,         L"DisableNegoEx"     L"Do not advertise mechanism through NegoEx" },
    { GSSP_FLAG_S4U_ON_DC,              L"UseS4UOnDC"        L"Use S4U2Self even on domain controllers" },
    { GSSP_FLAG_FORCE_KERB_RPCID,       L"UseKerberosRpcID", L"Masquerade as Kerberos for Exchange compatibility" },
    { GSSP_FLAG_LOGON,                  L"EnableLogonAP",    L"Support interactive logon" },
};

LPCWSTR
MsSspFlagToString(DWORD dwSspFlag)
{
    DWORD i;
    LPCWSTR wszSspFlag = NULL;

    for (i = 0; i < sizeof(sspFlagMap) / sizeof(sspFlagMap[0]); i++) {
        if (dwSspFlag == sspFlagMap[i].Flag) {
            wszSspFlag = sspFlagMap[i].String;
            break;
        }
    }

    return wszSspFlag;
}

DWORD
MsStringToSspFlag(LPCWSTR wszSspFlag)
{
    DWORD i;
    DWORD dwSspFlag = 0;

    for (i = 0; i < sizeof(sspFlagMap) / sizeof(sspFlagMap[0]); i++) {
        if (_wcsicmp(wszSspFlag, sspFlagMap[i].String) == 0) {
            dwSspFlag = sspFlagMap[i].Flag;
            break;
        }
    }

    return dwSspFlag;
}

DWORD
MsListSspFlags(FILE *fp)
{
    DWORD i;

    for (i = 0; i < sizeof(sspFlagMap) / sizeof(sspFlagMap[0]); i++) {
        fwprintf(fp, L"%02x %12s %s", sspFlagMap[i].Flag, sspFlagMap[i].String, sspFlagMap[i].Description);
    }

    return ERROR_SUCCESS;
}

