/*************************************************************************\
* Copyright (c) 2002 The University of Chicago, as Operator of Argonne
* National Laboratory.
* Copyright (c) 2002 Berliner Speicherring-Gesellschaft fuer Synchrotron-
* Strahlung mbH (BESSY).
* Copyright (c) 2002 The Regents of the University of California, as
* Operator of Los Alamos National Laboratory.
* This file is distributed subject to a Software License Agreement found
* in the file LICENSE that is included with this distribution. 
\*************************************************************************/
// Author: Jim Kowalkowski
// Date: 7/96

// gateStat: Contains data and CAS interface for one bit of gate
// status info.  Update is done via a gate server's method (setStat)
// calling gateStat::post_data.

// serv       points to the parent gate server of this status bit
// post_data  is a flag that switches posting updates on/off

#define DEBUG_UMR 0
#define DEBUG_POST_DATA 0

#define USE_OSI_TIME 1

#define STAT_DOUBLE

#if !USE_OSI_TIME
#include <time.h>
#endif

#include "gdd.h"
#include "gddApps.h"
#include "gateResources.h"
#include "gateServer.h"
#include "gateAs.h"
#include "gateStat.h"

static struct timespec *timeSpec(void)
	// Gets current time and puts it in a static timespec struct
	// For use by gdd::setTimeStamp, which will copy it
{
#if USE_OSI_TIME
	static struct timespec ts;
        epicsTime osit(epicsTime::getCurrent());
	// EPICS is 20 years ahead of its time
	ts=osit;
#else
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec-=631152000ul;	// EPICS ts start 20 years later ...
#endif
	return &ts;
}

#if statCount

//////// gateStatChan (derived from gate chan derived from casChannel)

gateStatChan::gateStatChan(const casCtx &ctx, casPV *casPvIn, gateAsEntry *asentry,
  const char * const user, const char * const host) :
	gateChan(ctx,casPvIn,asentry,user,host)
{
	gateStat *pStat=(gateStat *)casPv;
	if(pStat) pStat->addChan(this);
}

gateStatChan::~gateStatChan(void)
{
	gateStat *pStat=(gateStat *)casPv;
	if(pStat) pStat->removeChan(this);
}

// This is a new virtual write in CAS that knows the casChannel.  The
// casPV::write() is not called if this one is implemented.  This
// write calls a new, overloaded gateStat::write() that has the
// gateChan as an argument to do what used to be done in the virtual
// gateStat::write().
caStatus gateStatChan::write(const casCtx &ctx, const gdd &value)
{
	gateStat *pStat=(gateStat *)casPv;

	// Trap writes
	if(asclient && asclient->clientPvt()->trapMask) {
		FILE *fp=global_resources->getPutlogFp();
		if(fp) {
			fprintf(fp,"%s %s@%s %s\n",
			  timeStamp(),
			  asclient->user()?asclient->user():"Unknown",
			  asclient->host()?asclient->host():"Unknown",
			  pStat && pStat->getName()?pStat->getName():"Unknown");
			fflush(fp);
		}
	}
	
	// Call the non-virtual-function write() in the gateStat
	if(pStat) return pStat->write(ctx,value,*this);
	else return S_casApp_noSupport;
}

bool gateStatChan::readAccess(void) const
{
	return (asclient->readAccess())?true:false;
}

bool gateStatChan::writeAccess(void) const
{
	return (asclient->writeAccess())?true:false;
}

//////// gateStat (derived from casPV)

gateStat::gateStat(gateServer *s, gateAsEntry *e, const char *n, int t) :
	casPV(*s),
	value(NULL),
	attr(NULL),
	post_data(0),
	type(t),
	serv(s),
	name(strDup(n)),
	asentry(e)
{

// Define the value gdd;
#ifdef STAT_DOUBLE
	value=new gdd(global_resources->appValue,aitEnumFloat64);
	if(value)
	  value->put((aitFloat64)*serv->getStatTable(type)->init_value);
#else
	value=new gdd(global_resources->appValue,aitEnumInt32);
	if(value)
	  value->put((aitInt32)*serv->getStatTable(type)->init_value);
#endif
	value->setTimeStamp(timeSpec());
#if DEBUG_UMR
	fflush(stderr);
	printf("gateStat::gateStat: name=%s\n",name);
	fflush(stdout);
	value->dump();
	fflush(stderr);
#endif

	// Define the attributes gdd
	attr=new gdd(global_resources->appValue,aitEnumFloat64);
	attr = gddApplicationTypeTable::AppTable().getDD(gddAppType_attributes);
	if(attr) {
		attr[gddAppTypeIndex_attributes_units].
		  put(serv->getStatTable(type)->units);
		attr[gddAppTypeIndex_attributes_maxElements]=1;
		attr[gddAppTypeIndex_attributes_precision]=
		  serv->getStatTable(type)->precision;
		attr[gddAppTypeIndex_attributes_graphicLow]=0.0;
		attr[gddAppTypeIndex_attributes_graphicHigh]=0.0;
		attr[gddAppTypeIndex_attributes_controlLow]=0.0;
		attr[gddAppTypeIndex_attributes_controlHigh]=0.0;
		attr[gddAppTypeIndex_attributes_alarmLow]=0.0;
		attr[gddAppTypeIndex_attributes_alarmHigh]=0.0;
		attr[gddAppTypeIndex_attributes_alarmLowWarning]=0.0;
		attr[gddAppTypeIndex_attributes_alarmHighWarning]=0.0;
		attr->setTimeStamp(timeSpec());
	}
}

gateStat::~gateStat(void)
{
	serv->clearStat(type);
	if(value) value->unreference();
	if(attr) attr->unreference();
	if(name) delete [] name;
}

const char *gateStat::getName() const
{
	return name; 
}

casChannel* gateStat::createChannel(const casCtx &ctx,
  const char * const user, const char * const host)
{
	gateStatChan *pChan=new gateStatChan(ctx,this,asentry,user,host);
	return pChan;
}

caStatus gateStat::interestRegister(void)
{
	post_data=1;
	return S_casApp_success;
}

void gateStat::interestDelete(void) { post_data=0; }
unsigned gateStat::maxSimultAsyncOps(void) const { return 5000u; }

aitEnum gateStat::bestExternalType(void) const
{
#ifdef STAT_DOUBLE
	return aitEnumFloat64;
#else
	return aitEnumInt32;
#endif
}

// This is the virtual write function defined in casPV.  It should no
// longer be called if casChannel::write is implemented.
caStatus gateStat::write(const casCtx& ctx, const gdd& dd)
{
	fprintf(stderr,"Virtual gateStat::write called for %s.\n"
	  "  This is an error!\n",getName());
	return S_casApp_noSupport;
}

// This is a non-virtual-function write that allows passing a pointer
// to the gateChannel.  Currently chan is not used.
caStatus gateStat::write(const casCtx& ctx, const gdd& dd, gateChan &/*chan*/)
{
    gddApplicationTypeTable& table=gddApplicationTypeTable::AppTable();
	caStatus retVal=S_casApp_noSupport;
    
    if(value) {
		table.smartCopy(value,&dd);
		double val;
		value->getConvert(val);
		serv->setStat(type,val);
		retVal=serv->processStat(type,val);
    } else {
		retVal=S_casApp_noMemory;
    }
#if DEBUG_UMR
    fflush(stderr);
    print("gateStat::write: name=%s\n",name);
    fflush(stdout);
    dd.dump();
    fflush(stderr);
#endif
    return retVal;
}

caStatus gateStat::read(const casCtx & /*ctx*/, gdd &dd)
{
	static const aitString str = "Gateway Statistics PV";
	gddApplicationTypeTable& table=gddApplicationTypeTable::AppTable();
	caStatus retVal=S_casApp_noSupport;

	// Branch on application type
	unsigned at=dd.applicationType();
	switch(at) {
	case gddAppType_ackt:
	case gddAppType_acks:
	case gddAppType_dbr_stsack_string:
		fprintf(stderr,"%s gateStat::read: "
		  "Got unsupported app type %d for %s\n",
		  timeStamp(),
		  at,name?name:"Unknown Stat PV");
		fflush(stderr);
		retVal=S_casApp_noSupport;
		break;
	case gddAppType_class:
		dd.put(str);
		retVal=S_casApp_success;
		break;
	default:
		// Copy the current state
		if(attr) table.smartCopy(&dd,attr);
		if(value) table.smartCopy(&dd,value);
		retVal=S_casApp_success;
		break;
	}
#if DEBUG_UMR
	fflush(stderr);
	printf("gateStat::read: name=%s\n",name);
	fflush(stdout);
	dd.dump();
	fflush(stderr);
#endif
	return retVal;
}

void gateStat::report(void)
{
	printf("%-30s\n",getName());

	tsDLIter<gateStatChan> iter=chan_list.firstIter();
	while(iter.valid()) {
		iter->report();
		iter++;
	}
}

void gateStat::postData(long val)
{
#ifdef STAT_DOUBLE
	value->put((aitFloat64)val);
#else
	value->put((aitInt32)val);
#endif
	value->setTimeStamp(timeSpec());
	if(post_data) postEvent(serv->select_mask,*value);
#if DEBUG_UMR
	fflush(stderr);
	printf("gateStat::postData(l): name=%s\n",name);
	fflush(stdout);
	value->dump();
	fflush(stderr);
#endif
}

// KE: Could have these next two just call postData((aitFloat64)val)

void gateStat::postData(unsigned long val)
{
#if DEBUG_POST_DATA
	fflush(stdout);
	printf("gateStat::postData(ul): name=%s val=%ld\n",
	  name,val);
	fflush(stdout);
#endif
#ifdef STAT_DOUBLE
	value->put((aitFloat64)val);
#else
	value->put((aitInt32)val);
#endif
	value->setTimeStamp(timeSpec());
	if(post_data) postEvent(serv->select_mask,*value);
#if DEBUG_UMR || DEBUG_POST_DATA
	fflush(stderr);
	printf("gateStat::postData(ul): name=%s\n",name);
	fflush(stdout);
	value->dump();
	fflush(stderr);
#endif
}

void gateStat::postData(double val)
{
#ifdef STAT_DOUBLE
	value->put(val);
#else
	value->put(val);
#endif
	value->setTimeStamp(timeSpec());
	if(post_data) postEvent(serv->select_mask,*value);
#if DEBUG_UMR
	fflush(stderr);
	printf("gateStat::postData(dbl): name=%s\n",name);
	fflush(stdout);
	value->dump();
	fflush(stderr);
#endif
}

#endif    // statCount

/* **************************** Emacs Editing Sequences ***************** */
/* Local Variables: */
/* tab-width: 4 */
/* c-basic-offset: 4 */
/* c-comment-only-line-offset: 0 */
/* c-file-offsets: ((substatement-open . 0) (label . 0)) */
/* End: */
