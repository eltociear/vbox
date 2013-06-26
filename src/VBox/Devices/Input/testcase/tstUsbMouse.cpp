/* $Id$ */
/** @file
 * tstUsbMouse.cpp - testcase USB mouse and tablet devices.
 */

/*
 * Copyright (C) 2013 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

/*******************************************************************************
*   Header Files                                                               *
*******************************************************************************/
#include "VBoxDD.h"
#include <VBox/vmm/pdmdrv.h>
#include <iprt/alloc.h>
#include <iprt/stream.h>
#include <iprt/test.h>
#include <iprt/uuid.h>

/** Test mouse driver structure. */
typedef struct DRVTSTMOUSE
{
    /** The USBHID structure. */
    struct USBHID              *pUsbHid;
    /** The base interface for the mouse driver. */
    PDMIBASE                    IBase;
    /** Our mouse connector interface. */
    PDMIMOUSECONNECTOR          IConnector;
    /** The base interface of the attached mouse port. */
    PPDMIBASE                   pDrvBase;
    /** The mouse port interface of the attached mouse port. */
    PPDMIMOUSEPORT              pDrv;
    /** Is relative mode currently supported? */
    bool                        fRel;
    /** Is absolute mode currently supported? */
    bool                        fAbs;
} DRVTSTMOUSE, *PDRVTSTMOUSE;


/** Global mouse driver variable.
 * @todo To be improved some time. */
static DRVTSTMOUSE s_drvTstMouse;


/** @interface_method_impl{PDMUSBHLPR3,pfnVMSetErrorV} */
static DECLCALLBACK(int) tstVMSetErrorV(PPDMUSBINS pUsbIns, int rc,
                                        RT_SRC_POS_DECL, const char *pszFormat,
                                        va_list va)
{
    NOREF(pUsbIns);
    RTPrintf("Error: %s:%u:%s:", RT_SRC_POS_ARGS);
    RTPrintfV(pszFormat, va);
    return rc;
}

/** @interface_method_impl{PDMUSBHLPR3,pfnDriverAttach} */
/** @todo We currently just take the driver interface from the global
 * variable.  This is sufficient for a unit test but still a bit sad. */
static DECLCALLBACK(int) tstDriverAttach(PPDMUSBINS pUsbIns, RTUINT iLun,
                                         PPDMIBASE pBaseInterface,
                                         PPDMIBASE *ppBaseInterface,
                                         const char *pszDesc)
{
    NOREF(iLun);
    NOREF(pszDesc);
    pUsbIns->IBase = *pBaseInterface;
    *ppBaseInterface = &s_drvTstMouse.IBase;
    return VINF_SUCCESS;
}


static PDMUSBHLP s_tstUsbHlp;


/**
 * @interface_method_impl{PDMIBASE,pfnQueryInterface}
 */
static DECLCALLBACK(void *) tstMouseQueryInterface(PPDMIBASE pInterface,
                                                   const char *pszIID)
{
    PDRVTSTMOUSE pThis = RT_FROM_MEMBER(pInterface, DRVTSTMOUSE, IBase);
    PDMIBASE_RETURN_INTERFACE(pszIID, PDMIBASE, &pThis->IBase);
    PDMIBASE_RETURN_INTERFACE(pszIID, PDMIMOUSECONNECTOR, &pThis->IConnector);
    return NULL;
}


/**
 * @interface_method_impl{PDMIMOUSECONNECTOR,pfnReportModes}
 */
static DECLCALLBACK(void) tstMouseReportModes(PPDMIMOUSECONNECTOR pInterface,
                                              bool fRel, bool fAbs)
{
    PDRVTSTMOUSE pDrv = RT_FROM_MEMBER(pInterface, DRVTSTMOUSE, IConnector);
    pDrv->fRel = fRel;
    pDrv->fAbs = fAbs;
}


static int tstMouseConstruct(int iInstance, bool isAbsolute, uint8_t u8CoordShift,
                             PPDMUSBINS *ppThis)
{
    int rc = VERR_NO_MEMORY;
    PPDMUSBINS pThis = (PPDMUSBINS)RTMemAllocZ(  sizeof(*pThis)
                                               + g_UsbHidMou.cbInstance);
    PCFGMNODE pCfg = NULL;
    if (pThis)
    pCfg = CFGMR3CreateTree(NULL);
    if (pCfg)
        rc = CFGMR3InsertInteger(pCfg, "Absolute", isAbsolute);
    if (RT_SUCCESS(rc))
        rc = CFGMR3InsertInteger(pCfg, "CoordShift", u8CoordShift);
    if (RT_SUCCESS(rc))
    {
        pThis->iInstance = iInstance;
        pThis->pHlpR3 = &s_tstUsbHlp;
        rc = g_UsbHidMou.pfnConstruct(pThis, iInstance, pCfg, NULL);
        *ppThis = pThis;
        if (RT_SUCCESS(rc))
           return rc;
    }
    /* Failure */
    if (pCfg)
        CFGMR3DestroyTree(pCfg);
    if (pThis)
        RTMemFree(pThis);
    return rc;
}


static void testConstructAndDestruct(RTTEST hTest)
{
    PPDMUSBINS pThis;
    RTTestSub(hTest, "simple construction and destruction");
    int rc = tstMouseConstruct(0, false, 1, &pThis);
    if (RT_SUCCESS(rc))
        g_UsbHidMou.pfnDestruct(pThis);
    RTTEST_CHECK_RC_OK(hTest, rc);
}


int main()
{
    /*
     * Init the runtime, test and say hello.
     */
    RTTEST hTest;
    PDRVTSTMOUSE pThis;
    int rc = RTTestInitAndCreate("tstUsbMouse", &hTest);
    if (rc)
        return rc;
    RTTestBanner(hTest);
    /* Set up our faked PDMUSBHLP interface. */
    s_tstUsbHlp.pfnVMSetErrorV  = tstVMSetErrorV;
    s_tstUsbHlp.pfnDriverAttach = tstDriverAttach;
    /* Set up our global mouse driver */
    s_drvTstMouse.IBase.pfnQueryInterface = tstMouseQueryInterface;
    s_drvTstMouse.IConnector.pfnReportModes = tstMouseReportModes;

    /*
     * Run the tests.
     */
    testConstructAndDestruct(hTest);
    return RTTestSummaryAndDestroy(hTest);
}