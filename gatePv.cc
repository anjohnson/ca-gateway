// Author: Jim Kowalkowski
// Date: 2/96
//
// $Id$
//
// $Log$
// Revision 1.4  1996/09/06 11:56:21  jbk
// little fixes
//
// Revision 1.3  1996/08/14 21:10:32  jbk
// next wave of updates, menus stopped working, units working, value not
// working correctly sometimes, can't delete the channels
//
// Revision 1.2  1996/07/26 02:34:43  jbk
// Interum step.
//
// Revision 1.1  1996/07/23 16:32:35  jbk
// new gateway that actually runs
//
//

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/types.h>
#include <sys/time.h>

#include "gdd.h"
#include "gddApps.h"
#include "gddAppTable.h"
#include "dbMapper.h"

#include "gateResources.h"
#include "gateServer.h"
#include "gatePv.h"
#include "gateVc.h"

// quick access to global_resources
#define GR global_resources
#define GETDD(ap) gddApplicationTypeTable::AppTable().getDD(GR->ap)

// ------------------------- menu array of string destructor ----------------

class gateStringDestruct : public gddDestructor
{
public:
	gateStringDestruct(void) { }
	void run(void*);
};

void gateStringDestruct::run(void* v)
{
	gateDebug1(5,"void gateStringDestruct::run(void* %8.8x)\n",(int)v);
	aitFixedString* buf = (aitFixedString*)v;
	delete [] buf;
}

// ------------------------- pv data methods ------------------------

gatePvData::gatePvData(gateServer* m,const char* name)
{
	gateDebug2(5,"gatePvData(gateServer=%8.8x,name=%s)\n",(int)m,name);
	initClear();
	init(m,name);
}

gatePvData::gatePvData(gateServer* m,gateVcData* d,const char* name)
{
	gateDebug3(5,"gatePvData(gateServer=%8.8x,gateVcData=%8.8x,name=%s)\n",
		(int)m,(int)d,name);
	initClear();
	setVC(d);
	markAddRemoveNeeded();
	init(m,name);
}

gatePvData::gatePvData(gateServer* m,gateExistData* d,const char* name)
{
	gateDebug3(5,"gatePvData(gateServer=%8.8x,gateExistData=%8.8x,name=%s)\n",
		(int)m,(int)d,name);
	initClear();
	markAckNakNeeded();
	addET(d);
	init(m,name);
}

gatePvData::~gatePvData(void)
{
	gateDebug1(5,"~gatePvData() name=%s\n",name());
	unmonitor();
	status=ca_clear_channel(chan);
	SEVCHK(status,"clear channel");
	delete [] pv_name;
}

void gatePvData::initClear(void)
{
	setVC(NULL);
	status=0;
	markNotMonitored();
	markNoGetPending();
	markNoAbort();
	markAckNakNotNeeded();
	markAddRemoveNotNeeded();
	setState(gatePvDead);
}

void gatePvData::init(gateServer* m,const char* n)
{
	gateDebug2(5,"gatePvData::init(gateServer=%8.8x,name=%s)\n",(int)m,n);
	mrg=m;
	pv_name=strdup(n);
	setTimes();

	setState(gatePvConnect);

	status=ca_search_and_connect(pv_name,&chan,connectCB,this);
	SEVCHK(status,"gatePvData::init() - search and connect");

	if(status==ECA_NORMAL)
	{
		status=ca_replace_access_rights_event(chan,accessCB);
		if(status==ECA_NORMAL)
			status=0;
		else
			status=-1;
	}
	else
	{
		gateDebug0(5,"gatePvData::init() search and connect bad!\n");
		setState(gatePvDead);
		status=-1;
	}

	if(status)
	{
		// what do I do here? Nothing for now, let creator fix trouble
	}
	else
		mrg->conAdd(pv_name,*this); // put into connecting PV list

	checkEvent(); // only check the ca pend event thing here
}

aitEnum gatePvData::nativeType(void)
{
	return gddDbrToAit[fieldType()].type;
}

int gatePvData::activate(gateVcData* vcd)
{
	gateDebug2(5,"gatePvData::activate(gateVcData=%8.8x) name=%s\n",
		(int)vcd,name());

	int rc=0;

	switch(getState())
	{
	case gatePvInactive:
		gateDebug0(3,"gatePvData::activate() Inactive PV\n");
		markAddRemoveNeeded();
		vc=vcd;
		setState(gatePvActive);
		setActiveTime();
		get();
		break;
	case gatePvDead:
		gateDebug0(3,"gatePvData::activate() PV is dead\n");
		vc=NULL; // NOTE: be sure vc does not response
		rc=-1;
		break;
	case gatePvActive:
		gateDebug0(2,"gatePvData::activate() an active PV?\n");
		rc=-1;
		break;
	case gatePvConnect:
		// already pending, just return
		gateDebug0(3,"gatePvData::activate() connect pending PV?\n");
		markAddRemoveNeeded();
		rc=-1;
		break;
	}
	return rc;
}

int gatePvData::deactivate(void)
{
	gateDebug1(5,"gatePvData::deactivate() name=%s\n",name());

	int rc=0;

	switch(getState())
	{
	case gatePvActive:
		gateDebug0(20,"gatePvData::deactivate() active PV\n");
		unmonitor();
		setState(gatePvInactive);
		vc=NULL;
		setInactiveTime();
		break;
	case gatePvConnect:
		// delete from the connect pending list
		gateDebug0(20,"gatePvData::deactivate() connecting PV?\n");
		markAckNakNotNeeded();
		markAddRemoveNotNeeded();
		vc=NULL;
		break;
	case gatePvInactive:
		// error - should not get request to deactive an inactive PV
		gateDebug0(2,"gatePvData::deactivate() inactive PV?\n");
		rc=-1;
		break;
	case gatePvDead:
		gateDebug0(3,"gatePvData::deactivate() dead PV?\n");
		rc=-1;
		break;
	default: break;
	}

	return rc;
}

int gatePvData::life(void)
{
	gateDebug1(5,"gatePvData::life() name=%s\n",name());

	gateExistData* et;
	int rc=0;

	switch(getState())
	{
	case gatePvConnect:
		gateDebug0(3,"gatePvData::life() connecting PV\n");
		setTimes();

		// move from the connect pending list to PV list
		// need to index quickly into the PV connect pending list here
		// probably using the hash list
		// * No, don't use hash list, just add the PV to real PV list and
		// let the ConnectCleanup() routine just delete active PVs from 
		// the connecting PV list
		// mrg->CDeletePV(pv_name,x);

		mrg->pvAdd(pv_name,*this);

		if(needAddRemove())
		{
			if(vc)
			{
				setState(gatePvActive);
				get();
			}
		}
		else
		{
			setState(gatePvInactive);
			markNoAbort();
		}

		if(needAckNak())
		{
			while((et=et_list.head()))
			{
				et->ack();
				et_list.remove(*et);
			}
			markAckNakNotNeeded();
		}
		break;
	case gatePvDead:
		gateDebug0(3,"gatePvData::life() dead PV\n");
		setAliveTime();
		setState(gatePvInactive);
		break;
	case gatePvInactive:
		gateDebug0(3,"gatePvData::life() inactive PV\n");
		rc=-1;
		break;
	case gatePvActive:
		gateDebug0(3,"gatePvData::life() active PV\n");
		rc=-1;
		break;
	default: break;
	}
	return rc;
}

int gatePvData::death(void)
{
	gateDebug1(5,"gatePvData::death() name=%s\n",name());

	gateExistData* et;
	int rc=0;

	switch(getState())
	{
	case gatePvInactive:
		gateDebug0(3,"gatePvData::death() inactive PV\n");
		break;
	case gatePvActive:
		gateDebug0(3,"gatePvData::death() active PV\n");
		if(vc) delete vc; // get rid of VC
		break;
	case gatePvConnect:
		gateDebug0(3,"gatePvData::death() connecting PV\n");
		// still on connecting list, add to the PV list as dead
		if(needAckNak())
		{
			while((et=et_list.head()))
			{
				et->nak();
				et_list.remove(*et);
			}
		}
		if(needAddRemove() && vc) delete vc; // should never be the case
		mrg->pvAdd(pv_name,*this);
		break;
	case gatePvDead:
		gateDebug0(3,"gatePvData::death() dead PV\n");
		rc=-1;
		break;
	}

	vc=NULL;
	setState(gatePvDead);
	setDeathTime();
	markNoAbort();
	markAckNakNotNeeded();
	markAddRemoveNotNeeded();
	markNoGetPending();
	unmonitor();

	return rc;
}

int gatePvData::unmonitor(void)
{
	gateDebug1(5,"gatePvData::unmonitor() name=%s\n",name());
	int rc=0;

	if(monitored())
	{
		rc=ca_clear_event(event);
		SEVCHK(rc,"gatePvData::Unmonitor(): clear event");
		if(rc==ECA_NORMAL) rc=0;
		markNotMonitored();
	}
	return rc;
}

int gatePvData::monitor(void)
{
	gateDebug1(5,"gatePvData::monitor() name=%s\n",name());
	int rc=0;

	if(!monitored())
	{
		rc=ca_add_event(eventType(),chan,eventCB,this,&event);
		SEVCHK(rc,"gatePvData::Monitor() add event");

		if(rc==ECA_NORMAL)
		{
			rc=0;
			markMonitored();
			checkEvent();
		}
		else
			rc=-1;
	}
	return rc;
}

int gatePvData::get(void)
{
	gateDebug1(5,"gatePvData::get() name=%s\n",name());
	int rc=ECA_NORMAL;
	
	// only one active get allowed at once
	switch(getState())
	{
	case gatePvActive:
		gateDebug0(3,"gatePvData::get() active PV\n");
		if(!pendingGet())
		{
			gateDebug0(3,"gatePvData::get() issuing CA get cb\n");
			setTransTime();
			markGetPending();
			// always get only one element, the monitor will get
			// all the rest of the elements
			rc=ca_array_get_callback(dataType(),1 /*totalElements()*/,
				chan,getCB,this);
			SEVCHK(rc,"get with callback bad");
			checkEvent();
		}
		break;
	case gatePvInactive:
		gateDebug0(2,"gatePvData::get() inactive PV?\n");
		break;
	case gatePvConnect:
		gateDebug0(2,"gatePvData::get() connecting PV?\n");
		break;
	case gatePvDead:
		gateDebug0(2,"gatePvData::get() dead PV?\n");
		break;
	}
	return (rc==ECA_NORMAL)?0:-1;
}

int gatePvData::put(gdd* dd)
{
	gateDebug2(5,"gatePvData::put(gdd=%8.8x) name=%s\n",(int)dd,name());
	int rc=ECA_NORMAL;
	chtype cht;
	long sz;

	gateDebug1(6,"gatePvData::put() - Field type=%d\n",(int)fieldType());
	// dd->dump();

	switch(getState())
	{
	case gatePvActive:
		gateDebug0(2,"gatePvData::put() active PV\n");
		setTransTime();

		if(dd->isScalar())
		{
			gateDebug0(6,"gatePvData::put() ca put before\n");
			rc=ca_array_put_callback(fieldType(),
				1,chan,dd->dataAddress(),putCB,this);
			gateDebug0(6,"gatePvData::put() ca put after\n");
		}
		else
		{
			// hopefully this is only temporary and we will get a string ait
			if(fieldType()==DBF_STRING && dd->primitiveType()==aitEnumInt8)
			{
				sz=1;
				cht=DBF_STRING;
			}
			else
			{
				sz=dd->getDataSizeElements();
				cht=gddAitToDbr[dd->primitiveType()];
			}
			rc=ca_array_put_callback(cht,sz,chan,dd->dataPointer(),putCB,this);
		}

		SEVCHK(rc,"put callback bad");
		markAckNakNeeded();
		checkEvent();
		break;
	case gatePvInactive:
		gateDebug0(2,"gatePvData::put() inactive PV\n");
		break;
	case gatePvConnect:
		gateDebug0(2,"gatePvData::put() connecting PV\n");
		break;
	case gatePvDead:
		gateDebug0(2,"gatePvData::put() dead PV\n");
		break;
	}
	return (rc==ECA_NORMAL)?0:-1;
}

int gatePvData::putDumb(gdd* dd)
{
	gateDebug2(5,"gatePvData::putDumb(gdd=%8.8x) name=%s\n",(int)dd,name());
	chtype cht=gddAitToDbr[dd->primitiveType()];
	int rc=ECA_NORMAL;
	aitString* str;
	aitFixedString* fstr;

	switch(getState())
	{
	case gatePvActive:
		gateDebug0(2,"gatePvData::putDumb() active PV\n");
		setTransTime();
		switch(dd->primitiveType())
		{
		case aitEnumString:
			if(dd->isScalar())
				str=(aitString*)dd->dataAddress();
			else
				str=(aitString*)dd->dataPointer();

			// can only put one of these - arrays not valid to CA client
			gateDebug1(5," putting String <%s>\n",str->string());
			rc=ca_array_put(cht,1,chan,(void*)str->string());
			break;
		case aitEnumFixedString:
			fstr=(aitFixedString*)dd->dataPointer();
			gateDebug1(5," putting FString <%s>\n",fstr->fixed_string);
			rc=ca_array_put(cht,dd->getDataSizeElements(),chan,(void*)fstr);
			break;
		default:
			if(dd->isScalar())
				rc=ca_array_put(cht,1,chan,dd->dataAddress());
			else
				rc=ca_array_put(cht,dd->getDataSizeElements(),
					chan, dd->dataPointer());
			break;
		}
		SEVCHK(rc,"put dumb bad");
		checkEvent();
		break;
	case gatePvInactive:
		gateDebug0(2,"gatePvData::putDumb() inactive PV\n");
		break;
	case gatePvConnect:
		gateDebug0(2,"gatePvData::putDumb() connecting PV\n");
		break;
	case gatePvDead:
		gateDebug0(2,"gatePvData::putDumb() dead PV\n");
		break;
	default: break;
	}

	return (rc==ECA_NORMAL)?0:-1;
}

void gatePvData::connectCB(CONNECT_ARGS args)
{
	gatePvData* pv=(gatePvData*)ca_puser(args.chid);
	gateDebug1(5,"gatePvData::connectCB(gatePvData=%8.8x)\n",(int)pv);

	gateDebug0(9,"conCB: -------------------------------\n");
	gateDebug1(9,"conCB: name=%s\n",ca_name(args.chid));
	gateDebug1(9,"conCB: type=%d\n",ca_field_type(args.chid));
	gateDebug1(9,"conCB: number of elements=%d\n",ca_element_count(args.chid));
	gateDebug1(9,"conCB: host name=%s\n",ca_host_name(args.chid));
	gateDebug1(9,"conCB: read access=%d\n",ca_read_access(args.chid));
	gateDebug1(9,"conCB: write access=%d\n",ca_write_access(args.chid));
	gateDebug1(9,"conCB: state=%d\n",ca_state(args.chid));

	// send message to user concerning connection
	if(ca_state(args.chid)==cs_conn)
	{
		gateDebug0(9,"gatePvData::connectCB() connection ok\n");

		switch(ca_field_type(args.chid))
		{
		case DBF_STRING:
			pv->data_type=DBR_STS_STRING;
			pv->event_type=DBR_TIME_STRING;
			pv->event_func=eventStringCB;
			pv->data_func=dataStringCB;
			break;
		case DBF_ENUM:
			pv->data_type=DBR_CTRL_ENUM;
			pv->event_type=DBR_TIME_ENUM;
			pv->event_func=eventEnumCB;
			pv->data_func=dataEnumCB;
			break;
		case DBF_SHORT: // DBF_INT is same as DBF_SHORT
			pv->data_type=DBR_CTRL_SHORT;
			pv->event_type=DBR_TIME_SHORT;
			pv->event_func=eventShortCB;
			pv->data_func=dataShortCB;
			break;
		case DBF_FLOAT:
			pv->data_type=DBR_CTRL_FLOAT;
			pv->event_type=DBR_TIME_FLOAT;
			pv->event_func=eventFloatCB;
			pv->data_func=dataFloatCB;
			break;
		case DBF_CHAR:
			pv->data_type=DBR_CTRL_CHAR;
			pv->event_type=DBR_TIME_CHAR;
			pv->event_func=eventCharCB;
			pv->data_func=dataCharCB;
			break;
		case DBF_LONG:
			pv->data_type=DBR_CTRL_LONG;
			pv->event_type=DBR_TIME_LONG;
			pv->event_func=eventLongCB;
			pv->data_func=dataLongCB;
			break;
		case DBF_DOUBLE:
			pv->data_type=DBR_CTRL_DOUBLE;
			pv->event_type=DBR_TIME_DOUBLE;
			pv->event_func=eventDoubleCB;
			pv->data_func=dataDoubleCB;
			break;
		default:
			pv->event_type=(chtype)-1;
			pv->data_type=(chtype)-1;
			pv->event_func=NULL;
			break;
		}

		pv->max_elements=pv->totalElements();
		pv->life();
	}
	else
	{
		gateDebug0(9,"gatePvData::connectCB() connection dead\n");
		pv->death();
	}
}

void gatePvData::putCB(EVENT_ARGS args)
{
	gatePvData* pv=(gatePvData*)ca_puser(args.chid);
	gateDebug1(5,"gatePvData::putCB(gatePvData=%8.8x)\n",pv);
	// notice that put with callback never fails here (always ack'ed)
	pv->vc->ack(); // inform the VC
}

void gatePvData::eventCB(EVENT_ARGS args)
{
	gatePvData* pv=(gatePvData*)ca_puser(args.chid);
	gateDebug1(5,"gatePvData::eventCB(gatePvData=%8.8x)\n",pv);
	gdd* dd;

	if(args.status==ECA_NORMAL)
	{
		// only sends PV event data (attributes) and ADD transactions
		if(pv->active())
		{
			gateDebug0(5,"gatePvData::eventCB() active pv\n");
			if(dd=pv->runEventCB((void*)(args.dbr)))
			{
				if(pv->needAddRemove())
				{
					gateDebug0(5,"gatePvData::eventCB() need add/remove\n");
					pv->markAddRemoveNotNeeded();
					pv->vc->add(dd);
				}
				else
					pv->vc->eventData(dd);
			}
		}
	}
	// hopefully more monitors will come in that are successful
}

void gatePvData::getCB(EVENT_ARGS args)
{
	gatePvData* pv=(gatePvData*)ca_puser(args.chid);
	gateDebug1(5,"gatePvData::getCB(gatePvData=%8.8x)\n",pv);
	gdd* dd;

	pv->markNoGetPending();
	if(args.status==ECA_NORMAL)
	{
		// get only sends PV data (attributes)
		if(pv->active())
		{
			gateDebug0(5,"gatePvData::getCB() pv active\n");
			if(dd=pv->runDataCB((void*)(args.dbr))) pv->vc->pvData(dd);
			pv->monitor();
		}
	}
	else
	{
		// problems with the PV if status code not normal - attempt monitor
		// should check if Monitor() fails and send remove trans if
		// needed
		if(pv->active()) pv->monitor();
	}
}

void gatePvData::accessCB(ACCESS_ARGS args)
{
	// not implemented yet

	gateDebug0(9,"accCB: -------------------------------\n");
	gateDebug1(9,"accCB: name=%s\n",ca_name(args.chid));
	gateDebug1(9,"accCB: type=%d\n",ca_field_type(args.chid));
	gateDebug1(9,"accCB: number of elements=%d\n",ca_element_count(args.chid));
	gateDebug1(9,"accCB: host name=%s\n",ca_host_name(args.chid));
	gateDebug1(9,"accCB: read access=%d\n",ca_read_access(args.chid));
	gateDebug1(9,"accCB: write access=%d\n",ca_write_access(args.chid));
	gateDebug1(9,"accCB: state=%d\n",ca_state(args.chid));
}

// one function for each of the different data that come from gets:
//  DBR_STS_STRING
//  DBR_CTRL_ENUM
//  DBR_CTRL_CHAR
//  DBR_CTRL_DOUBLE
//  DBR_CTRL_FLOAT
//  DBR_CTRL_LONG
//  DBR_CTRL_SHORT (DBR_CTRL_INT)

gdd* gatePvData::dataStringCB(void* /*dbr*/)
{
	gateDebug0(4,"gatePvData::dataStringCB\n");
	// no useful attributes returned by this function
	return NULL;
}

gdd* gatePvData::dataEnumCB(void* dbr)
{
	gateDebug0(4,"gatePvData::dataEnumCB\n");
	int i;
	dbr_ctrl_enum* ts = (dbr_ctrl_enum*)dbr;
	aitFixedString* items = new aitFixedString[ts->no_str];
	gddAtomic* menu=new gddAtomic(GR->appEnum,aitEnumFixedString,1,ts->no_str);

	// DBR_CTRL_ENUM response
	for(i=0;i<ts->no_str;i++)
	{
		strcpy(items[i].fixed_string,&(ts->strs[i][0]));
		gateDebug2(5," enum %d=%s \n",i,&(ts->strs[i][0]));
	}

	menu->putRef(items,new gateStringDestruct);
	return menu;
}

gdd* gatePvData::dataDoubleCB(void* dbr)
{
	gateDebug0(10,"gatePvData::dataDoubleCB\n");
	dbr_ctrl_double* ts = (dbr_ctrl_double*)dbr;
	gdd* attr = GETDD(appAttributes);

	// DBR_CTRL_DOUBLE response
	attr[gddAppTypeIndex_attributes_units].put(ts->units);
	attr[gddAppTypeIndex_attributes_maxElements]=maxElements();
	attr[gddAppTypeIndex_attributes_precision]=ts->precision;
	attr[gddAppTypeIndex_attributes_graphicLow]=ts->lower_disp_limit;
	attr[gddAppTypeIndex_attributes_graphicHigh]=ts->upper_disp_limit;
	attr[gddAppTypeIndex_attributes_controlLow]=ts->lower_ctrl_limit;
	attr[gddAppTypeIndex_attributes_controlHigh]=ts->upper_ctrl_limit;
	attr[gddAppTypeIndex_attributes_alarmLow]=ts->lower_alarm_limit;
	attr[gddAppTypeIndex_attributes_alarmHigh]=ts->upper_alarm_limit;
	attr[gddAppTypeIndex_attributes_alarmLowWarning]=ts->lower_warning_limit;
	attr[gddAppTypeIndex_attributes_alarmHighWarning]=ts->upper_warning_limit;
	return attr;
}

gdd* gatePvData::dataShortCB(void* dbr)
{
	gateDebug0(10,"gatePvData::dataShortCB\n");
	dbr_ctrl_short* ts = (dbr_ctrl_short*)dbr;
	gdd* attr = GETDD(appAttributes);

	// DBR_CTRL_SHORT DBT_CTRL_INT response
	attr[gddAppTypeIndex_attributes_units].put(ts->units);
	attr[gddAppTypeIndex_attributes_maxElements]=maxElements();
	attr[gddAppTypeIndex_attributes_precision]=0;
	attr[gddAppTypeIndex_attributes_graphicLow]=ts->lower_disp_limit;
	attr[gddAppTypeIndex_attributes_graphicHigh]=ts->upper_disp_limit;
	attr[gddAppTypeIndex_attributes_controlLow]=ts->lower_ctrl_limit;
	attr[gddAppTypeIndex_attributes_controlHigh]=ts->upper_ctrl_limit;
	attr[gddAppTypeIndex_attributes_alarmLow]=ts->lower_alarm_limit;
	attr[gddAppTypeIndex_attributes_alarmHigh]=ts->upper_alarm_limit;
	attr[gddAppTypeIndex_attributes_alarmLowWarning]=ts->lower_warning_limit;
	attr[gddAppTypeIndex_attributes_alarmHighWarning]=ts->upper_warning_limit;
	return attr;
}

gdd* gatePvData::dataFloatCB(void* dbr)
{
	gateDebug0(10,"gatePvData::dataFloatCB\n");
	dbr_ctrl_float* ts = (dbr_ctrl_float*)dbr;
	gdd* attr = GETDD(appAttributes);

	// DBR_CTRL_FLOAT response
	attr[gddAppTypeIndex_attributes_units].put(ts->units);
	attr[gddAppTypeIndex_attributes_maxElements]=maxElements();
	attr[gddAppTypeIndex_attributes_precision]=ts->precision;
	attr[gddAppTypeIndex_attributes_graphicLow]=ts->lower_disp_limit;
	attr[gddAppTypeIndex_attributes_graphicHigh]=ts->upper_disp_limit;
	attr[gddAppTypeIndex_attributes_controlLow]=ts->lower_ctrl_limit;
	attr[gddAppTypeIndex_attributes_controlHigh]=ts->upper_ctrl_limit;
	attr[gddAppTypeIndex_attributes_alarmLow]=ts->lower_alarm_limit;
	attr[gddAppTypeIndex_attributes_alarmHigh]=ts->upper_alarm_limit;
	attr[gddAppTypeIndex_attributes_alarmLowWarning]=ts->lower_warning_limit;
	attr[gddAppTypeIndex_attributes_alarmHighWarning]=ts->upper_warning_limit;
	return attr;
}

gdd* gatePvData::dataCharCB(void* dbr)
{
	gateDebug0(10,"gatePvData::dataCharCB\n");
	dbr_ctrl_char* ts = (dbr_ctrl_char*)dbr;
	gdd* attr = GETDD(appAttributes);

	// DBR_CTRL_CHAR response
	attr[gddAppTypeIndex_attributes_units].put(ts->units);
	attr[gddAppTypeIndex_attributes_maxElements]=maxElements();
	attr[gddAppTypeIndex_attributes_precision]=0;
	attr[gddAppTypeIndex_attributes_graphicLow]=ts->lower_disp_limit;
	attr[gddAppTypeIndex_attributes_graphicHigh]=ts->upper_disp_limit;
	attr[gddAppTypeIndex_attributes_controlLow]=ts->lower_ctrl_limit;
	attr[gddAppTypeIndex_attributes_controlHigh]=ts->upper_ctrl_limit;
	attr[gddAppTypeIndex_attributes_alarmLow]=ts->lower_alarm_limit;
	attr[gddAppTypeIndex_attributes_alarmHigh]=ts->upper_alarm_limit;
	attr[gddAppTypeIndex_attributes_alarmLowWarning]=ts->lower_warning_limit;
	attr[gddAppTypeIndex_attributes_alarmHighWarning]=ts->upper_warning_limit;
	return attr;
}

gdd* gatePvData::dataLongCB(void* dbr)
{
	gateDebug0(10,"gatePvData::dataLongCB\n");
	dbr_ctrl_long* ts = (dbr_ctrl_long*)dbr;
	gdd* attr = GETDD(appAttributes);

	// DBR_CTRL_LONG response
	attr[gddAppTypeIndex_attributes_units].put(ts->units);
	attr[gddAppTypeIndex_attributes_maxElements]=maxElements();
	attr[gddAppTypeIndex_attributes_precision]=0;
	attr[gddAppTypeIndex_attributes_graphicLow]=ts->lower_disp_limit;
	attr[gddAppTypeIndex_attributes_graphicHigh]=ts->upper_disp_limit;
	attr[gddAppTypeIndex_attributes_controlLow]=ts->lower_ctrl_limit;
	attr[gddAppTypeIndex_attributes_controlHigh]=ts->upper_ctrl_limit;
	attr[gddAppTypeIndex_attributes_alarmLow]=ts->lower_alarm_limit;
	attr[gddAppTypeIndex_attributes_alarmHigh]=ts->upper_alarm_limit;
	attr[gddAppTypeIndex_attributes_alarmLowWarning]=ts->lower_warning_limit;
	attr[gddAppTypeIndex_attributes_alarmHighWarning]=ts->upper_warning_limit;
	return attr;
}

// one function for each of the different events that come from monitors:
//  DBR_TIME_STRING
//  DBR_TIME_ENUM
//  DBR_TIME_CHAR
//  DBR_TIME_DOUBLE
//  DBR_TIME_FLOAT
//  DBR_TIME_LONG
//  DBR_TIME_SHORT (DBR_TIME_INT)

gdd* gatePvData::eventStringCB(void* dbr)
{
	gateDebug0(10,"gatePvData::eventStringCB\n");
	dbr_time_string* ts = (dbr_time_string*)dbr;
	gddScalar* value=new gddScalar(GR->appValue, aitEnumFixedString);

	aitString* str = (aitString*)value->dataAddress();

	// DBR_TIME_STRING response
	str->installString(ts->value);
	value->setStatSevr(ts->status,ts->severity);
	value->setTimeStamp((aitTimeStamp*)&ts->stamp);
	return value;
}

gdd* gatePvData::eventEnumCB(void* dbr)
{
	gateDebug0(10,"gatePvData::eventEnumCB\n");
	dbr_time_enum* ts = (dbr_time_enum*)dbr;
	gddScalar* value = new gddScalar(GR->appValue,aitEnumEnum16);

	// DBR_TIME_ENUM response
	*value=ts->value;
	value->setStatSevr(ts->status,ts->severity);
	value->setTimeStamp((aitTimeStamp*)&ts->stamp);

	return value;
}

gdd* gatePvData::eventLongCB(void* dbr)
{
	gateDebug0(10,"gatePvData::eventLongCB\n");
	dbr_time_long* ts = (dbr_time_long*)dbr;
	aitIndex count = totalElements();
	gdd* value;
	aitInt32* d;

	// DBR_TIME_LONG response
	// set up the value
	if(count>1)
	{
		value=new gddAtomic(GR->appValue,aitEnumInt32,1,&count);
		d=(aitInt32*)ts->value;
		value->putRef(d);
	}
	else
	{
		value = new gddScalar(GR->appValue,aitEnumInt32);
		*value=ts->value;
	}
	value->setStatSevr(ts->status,ts->severity);
	value->setTimeStamp((aitTimeStamp*)&ts->stamp);
	return value;
}

gdd* gatePvData::eventCharCB(void* dbr)
{
	gateDebug0(10,"gatePvData::eventCharCB\n");
	dbr_time_char* ts = (dbr_time_char*)dbr;
	aitIndex count = totalElements();
	gdd* value;
	aitInt8* d;

	// DBR_TIME_CHAR response
	// set up the value
	if(count>1)
	{
		value = new gddAtomic(GR->appValue,aitEnumInt8,1,&count);
		d=(aitInt8*)&(ts->value);
		value->putRef(d);
	}
	else
	{
		value = new gddScalar(GR->appValue,aitEnumInt8);
		*value=ts->value;
	}
	value->setStatSevr(ts->status,ts->severity);
	value->setTimeStamp((aitTimeStamp*)&ts->stamp);
	return value;
}

gdd* gatePvData::eventFloatCB(void* dbr)
{
	gateDebug0(10,"gatePvData::eventFloatCB\n");
	dbr_time_float* ts = (dbr_time_float*)dbr;
	aitIndex count = totalElements();
	gdd* value;
	aitFloat32* d;

	// DBR_TIME_FLOAT response
	// set up the value
	if(count>1)
	{
		value= new gddAtomic(GR->appValue,aitEnumFloat32,1,&count);
		d=(aitFloat32*)&(ts->value);
		value->putRef(d);
	}
	else
	{
		value = new gddScalar(GR->appValue,aitEnumFloat32);
		*value=ts->value;
	}
	value->setStatSevr(ts->status,ts->severity);
	value->setTimeStamp((aitTimeStamp*)&ts->stamp);
	return value;
}

gdd* gatePvData::eventDoubleCB(void* dbr)
{
	gateDebug0(10,"gatePvData::eventDoubleCB\n");
	dbr_time_double* ts = (dbr_time_double*)dbr;
	aitIndex count = totalElements();
	gdd* value;
	aitFloat64* d;

	// DBR_TIME_FLOAT response
	// set up the value
	if(count>1)
	{
		value= new gddAtomic(GR->appValue,aitEnumFloat64,1,&count);
		d=(aitFloat64*)&(ts->value);
		value->putRef(d);
	}
	else
	{
		value = new gddScalar(GR->appValue,aitEnumFloat64);
		*value=ts->value;
	}
	value->setStatSevr(ts->status,ts->severity);
	value->setTimeStamp((aitTimeStamp*)&ts->stamp);
	return value;
}

gdd* gatePvData::eventShortCB(void* dbr)
{
	gateDebug0(10,"gatePvData::eventShortCB\n");
	dbr_time_short* ts = (dbr_time_short*)dbr;
	aitIndex count = totalElements();
	gdd* value;
	aitInt16* d;

	// DBR_TIME_FLOAT response
	// set up the value
	if(count>1)
	{
		value=new gddAtomic(GR->appValue,aitEnumInt16,1,&count);
		d=(aitInt16*)&(ts->value);
		value->putRef(d);
	}
	else
	{
		value = new gddScalar(GR->appValue,aitEnumInt16);
		*value=ts->value;
	}
	value->setStatSevr(ts->status,ts->severity);
	value->setTimeStamp((aitTimeStamp*)&ts->stamp);
	return value;
}
