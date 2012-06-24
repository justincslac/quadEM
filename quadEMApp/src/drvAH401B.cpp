/*
 * drvAH401B.h
 * 
 * Asyn driver that inherits from the asynPortDriver class to control the Ellectra AH401B 4-channel picoammeter
 *
 * Author: Mark Rivers
 *
 * Created June 3, 2012
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <math.h>

#include <epicsTypes.h>
#include <epicsTime.h>
#include <epicsThread.h>
#include <epicsString.h>
#include <epicsTimer.h>
#include <epicsMutex.h>
#include <epicsEvent.h>
#include <asynOctetSyncIO.h>
#include <iocsh.h>

#include "drvAH401B.h"
#include <epicsExport.h>


static const char *driverName="drvAH401B";
static void readThread(void *drvPvt);


/** Constructor for the drvAH401B class.
  * Calls constructor for the asynPortDriver base class.
  * \param[in] portName The name of the asyn port driver to be created.
  * \param[in] QEPortName The name of the asyn communication port to the AH401B 
  *            created with drvAsynIPPortConfigure or drvAsynSerialPortConfigure */
drvAH401B::drvAH401B(const char *portName, const char *QEPortName) 
   : drvQuadEM(portName, 0)
  
{
    asynStatus status;
    const char *functionName = "drvAH401B";
    
    QEPortName_ = epicsStrDup(QEPortName);
    
    readDataEvent_ = epicsEventCreate(epicsEventEmpty);
    
    // Connect to the server
    status = pasynOctetSyncIO->connect(QEPortName, 0, &pasynUserMeter_, NULL);
    if (status) {
        printf("%s:%s: error calling pasynOctetSyncIO->connect, status=%d, error=%s\n", 
               driverName, functionName, status, pasynUserMeter_->errorMessage);
        return;
    }

    setAcquire(0);
    setIntegerParam(P_Acquire, 0);
    strcpy(outString_, "VER ?");
    writeReadMeter();
    firmwareVersion_ = epicsStrDup(inString_);
    
    /* Create the thread that reads the meter */
    status = (asynStatus)(epicsThreadCreate("drvAH401BTask",
                          epicsThreadPriorityMedium,
                          epicsThreadGetStackSize(epicsThreadStackMedium),
                          (EPICSTHREADFUNC)::readThread,
                          this) == NULL);
    if (status) {
        printf("%s:%s: epicsThreadCreate failure, status=%d\n", driverName, functionName, status);
        return;
    }
}



static void readThread(void *drvPvt)
{
    drvAH401B *pPvt = (drvAH401B *)drvPvt;
    
    pPvt->readThread();
}

/** Sends a command to the AH401B and reads the response.
  * If meter is acquiring turns it off and waits for it to stop sending data */ 
asynStatus drvAH401B::sendCommand()
{
  int prevAcquiring;
  asynStatus status;
  const char *functionName="sendCommand";
  
  prevAcquiring = acquiring_;
  if (prevAcquiring) setAcquire(0);
  status = writeReadMeter();
  if (strcmp(inString_, "ACK") != 0) {
      asynPrint(pasynUserSelf, ASYN_TRACE_ERROR, 
          "%s:%s: error, expected ACK, received %s\n",
          driverName, functionName, inString_);
  }
  if (prevAcquiring) setAcquire(1);
  return status;
}


/** Writes a string to the AH401B and reads the response.
  * If meter is acquiring turns it off and waits for it to stop sending data */ 
asynStatus drvAH401B::writeReadMeter()
{
  size_t nread;
  size_t nwrite;
  asynStatus status;
  int eomReason;
  //const char *functionName="writeReadMeter";
  
  status = pasynOctetSyncIO->writeRead(pasynUserMeter_, outString_, strlen(outString_), 
                                       inString_, sizeof(inString_), AH401B_TIMEOUT, &nwrite, &nread, &eomReason);                                      
  return status;
}


/** Read thread to read the data from the electrometer when it is in continuous acquire mode */
void drvAH401B::readThread(void)
{
    /* This thread reads the data, computes the sums and positions, and does callbacks */

    int acquire;
    asynStatus status;
    size_t nRead; 
    int eomReason;
    epicsInt32 raw[QE_MAX_INPUTS];
    char input[MAX_COMMAND_LEN];
    double integrationTime;
    static const char *functionName = "readThread";
    
    /* Loop forever */
    lock();
    while (1) {
        getIntegerParam(P_Acquire, &acquire);
        if (!acquire) {
            unlock();
            epicsEventWait(readDataEvent_);
            lock();
        }
        getDoubleParam(P_IntegrationTime, &integrationTime);
        unlock();
        status = pasynOctetSyncIO->read(pasynUserMeter_, input, sizeof(input), 
                                        AH401B_TIMEOUT, &nRead, &eomReason);
        lock();
        if (nRead == 0) continue;
        if (status != asynSuccess) {
            asynPrint(pasynUserSelf, ASYN_TRACE_ERROR, 
                "%s:%s: unexpected error reading meter status=%d, nRead=%d, eomReason=%d\n", 
                driverName, functionName, status);
            continue;
        }
        sscanf(input, "%d %d %d %d", &raw[0], &raw[1], &raw[2], &raw[3]);
        computePositions(raw);
    }
}

asynStatus drvAH401B::setAcquire(epicsInt32 value) 
{
    size_t nread;
    size_t nwrite;
    asynStatus status=asynSuccess, readStatus;
    int eomReason;
    char dummyIn[MAX_COMMAND_LEN];

    if (value == 0) {
        while (1) {
            // Repeat sending ACQ OFF until we get only an ACK back
            status = pasynOctetSyncIO->writeRead(pasynUserMeter_, "ACQ OFF", strlen("ACQ OFF"), 
                                             dummyIn, MAX_COMMAND_LEN, AH401B_TIMEOUT, &nwrite, &nread, &eomReason);
            // Flush the input buffer because there could be more characters sent after ACK
            nread = 0;
            readStatus = pasynOctetSyncIO->read(pasynUserMeter_, dummyIn, MAX_COMMAND_LEN, .1, &nread, &eomReason);
            if ((readStatus == asynTimeout) && (nread == 0)) break;
        }
        acquiring_ = 0;
    } else {
        status = pasynOctetSyncIO->writeRead(pasynUserMeter_, "ACQ ON", strlen("ACQ ON"), 
                                             dummyIn, MAX_COMMAND_LEN, AH401B_TIMEOUT, &nwrite, &nread, &eomReason);
        // Notify the read thread if acquisition status has started
        epicsEventSignal(readDataEvent_);
        acquiring_ = 1;
    }
    return status;
}

 
asynStatus drvAH401B::setPingPong(epicsInt32 value) 
{
    asynStatus status;
    
    epicsSnprintf(outString_, sizeof(outString_), "HLF %s", value ? "OFF" : "ON");
    status = sendCommand();
    return status;
}

asynStatus drvAH401B::setIntegrationTime(epicsFloat64 value) 
{
    asynStatus status;
    
    /* Make sure the update time is valid. If not change it and put back in parameter library */
    if (value < MIN_INTEGRATION_TIME) {
        value = MIN_INTEGRATION_TIME;
        setDoubleParam(P_IntegrationTime, value);
    }
    epicsSnprintf(outString_, sizeof(outString_), "ITM %d", (int)(value * 10000));
    status = sendCommand();
    return status;
}

asynStatus drvAH401B::setRange(epicsInt32 value) 
{
    asynStatus status;
    
    epicsSnprintf(outString_, sizeof(outString_), "RNG %d", value);
    status = sendCommand();
    return status;
}

asynStatus drvAH401B::setReset() 
{
    // Not supported on AH401B, just ignore
    return asynSuccess;
}

asynStatus drvAH401B::setTrigger(epicsInt32 value) 
{
    asynStatus status;
    
    epicsSnprintf(outString_, sizeof(outString_), "TRG %s", value ? "ON" : "OFF");
    status = sendCommand();
    return status;
}

asynStatus drvAH401B::getSettings() 
{
    // Reads the values of all the meter parameters, sets them in the parameter library
    int pingPong, trigger, range;
    double integrationTime;
    int prevAcquiring;
    static const char *functionName = "getStatus";
    
    prevAcquiring = acquiring_;
    if (prevAcquiring) setAcquire(0);
    strcpy(outString_, "TRG ?");
    writeReadMeter();
    if (strcmp("TRG ON", inString_) == 0) trigger = 1;
    else if (strcmp("TRG OFF", inString_) == 0) trigger = 0;
    else goto error;
    setIntegerParam(P_Trigger, trigger);
    
    strcpy(outString_, "HLF ?");
    writeReadMeter();
    if (strcmp("HLF ON", inString_) == 0) pingPong = 0;
    else if (strcmp("HLF OFF", inString_) == 0) pingPong = 1;
    else goto error;
    setIntegerParam(P_PingPong, pingPong);
    
    strcpy(outString_, "RNG ?");
    writeReadMeter();
    if (sscanf(inString_, "RNG %d", &range) != 1) goto error;
    setIntegerParam(P_Range, range);
    
    strcpy(outString_, "ITM ?");
    writeReadMeter();
    if (sscanf(inString_, "ITM %lf", &integrationTime) != 1) goto error;
    integrationTime = integrationTime/10000.;
    setDoubleParam(P_IntegrationTime, integrationTime);
    
    if (prevAcquiring) setAcquire(1);
    
    return asynSuccess;

    error:
    asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
        "%s:%s: error, outString=%s, inString=%s\n",
        driverName, functionName, outString_, inString_);
    return asynError;
}


void drvAH401B::report(FILE *fp, int details)
{
    fprintf(fp, "%s: port=%s, IP port=%s, firmware version=%s\n",
            driverName, portName, QEPortName_, firmwareVersion_);
    drvQuadEM::report(fp, details);
}


/* Configuration routine.  Called directly, or from the iocsh function below */

extern "C" {

/** EPICS iocsh callable function to call constructor for the drvAH401B class.
  * \param[in] portName The name of the asyn port driver to be created.
  * \param[in] maxPoints The maximum  number of points in the volt and time arrays */
int drvAH401BConfigure(const char *portName, const char *QEPortName)
{
    new drvAH401B(portName, QEPortName);
    return(asynSuccess);
}


/* EPICS iocsh shell commands */

static const iocshArg initArg0 = { "portName",iocshArgString};
static const iocshArg initArg1 = { "QEPortName",iocshArgString};
static const iocshArg * const initArgs[] = {&initArg0,
                                            &initArg1};
static const iocshFuncDef initFuncDef = {"drvAH401BConfigure",2,initArgs};
static void initCallFunc(const iocshArgBuf *args)
{
    drvAH401BConfigure(args[0].sval, args[1].sval);
}

void drvAH401BRegister(void)
{
    iocshRegister(&initFuncDef,initCallFunc);
}

epicsExportRegistrar(drvAH401BRegister);

}
