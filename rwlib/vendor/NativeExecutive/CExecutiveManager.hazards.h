/*****************************************************************************
*
*  PROJECT:     Native Executive
*  LICENSE:     See LICENSE in the top level directory
*  FILE:        NativeExecutive/CExecutiveManager.hazard.h
*  PURPOSE:     Deadlock prevention by signaling code paths to continue execution
*  DEVELOPERS:  Martin Turski <quiret@gmx.de>
*
*  Multi Theft Auto is available from http://www.multitheftauto.com/
*
*****************************************************************************/

#ifndef _NATIVE_EXECUTIVE_HAZARD_PREVENTION_
#define _NATIVE_EXECUTIVE_HAZARD_PREVENTION_

BEGIN_NATIVE_EXECUTIVE

struct hazardPreventionInterface abstract
{
    virtual void TerminateHazard( void ) = 0;
};

// Global API for managing hazards.
void PushHazard( CExecutiveManager *manager, hazardPreventionInterface *intf );
void PopHazard( CExecutiveManager *manager );

// Helpers to make things easier for you.
struct hazardousSituation
{
    inline hazardousSituation( CExecutiveManager *manager, hazardPreventionInterface *intf )
    {
        this->manager = manager;
        this->intf = intf;

        PushHazard( manager, intf );
    }

    inline ~hazardousSituation( void )
    {
        PopHazard( manager );
    }

private:
    CExecutiveManager *manager;
    hazardPreventionInterface *intf;
};

END_NATIVE_EXECUTIVE

#endif //_NATIVE_EXECUTIVE_HAZARD_PREVENTION_