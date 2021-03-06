/*
 * Ankit More
 *
 * DragonTester.cc
 *
 */
 
#include "base/intmath.hh"
#include "base/misc.hh"
#include "cpu/testers/dragontest/DragonTester.hh"
#include "mem/ruby/eventqueue/RubyEventQueue.hh"
#include "mem/ruby/system/System.hh"
#include "sim/sim_exit.hh"
#include "sim/system.hh"
#include "debug/amTrace.hh"
#include "debug/mutexLogger.hh"
#include "debug/printEvent.hh"
#include "debug/printEventFull.hh"
#include "debug/roi.hh"
#include "debug/cpi.hh"

DragonTester::DragonTester(const Params *p)
  : MemObject(p), dragonStartEvent(this),
    _masterId(p->system->getMasterId(name())),
    m_num_cpus(p->num_cpus), m_num_threads(p->num_threads),
    m_deadlock_threshold(p->deadlock_threshold),
    m_eventDir(p->eventDir), m_outputDir(p->outputDir),
    m_skipLocalRW(p->skipLocal),
    m_master_wakeup_frequency(p->masterWakeupFreq),
    m_directory_shared_frac(p->directory_shared_fraction),
    CPI_IOPS(p->cpi_iops),
    CPI_FLOPS(p->cpi_flops)
{
    // create the ports
    for (int i = 0; i < p->port_cpuPort_connection_count; ++i) {
        ports.push_back(new CpuPort(csprintf("%s-port%d", name(), i),this, i));
    }

    schedule(dragonStartEvent, 1);
    //DPRINTF(amTrace, "Done scheduling dragon tester event...scheduling individual core events for the first time\n");
 
    for (int i = 0; i < num_of_mapped_processors; i++) {
        schedule(*(coreEvents[i]), 1);
    }
    //DPRINTF(amTrace, "Done scheduling core events....starting real work!!\n");

    //Added by Sid to keep track of % of cycles from Shared Reads/Writes and % of cycles from Local Read/Writes
//     sharedEventHist.resize(m_num_threads);
//     localEventHist.resize(m_num_threads);
//     for (int i = 0; i < m_num_threads; i++) {
//       sharedEventHist[i].resize(10);
//       localEventHist[i].resize(10);
//     }

}

 
DragonTester::~DragonTester()
{
    for (int i = 0; i < ports.size(); i++)
        delete ports[i];
 
    for (int i = 0; i < num_of_mapped_processors; i++)
        delete coreEvents[i];

    for (int i = 0; i < m_num_threads; i++)
        delete eventMap[i];
    delete[] eventMap;
}
 
void DragonTester::init() // Initialization in virtual class definitions.
{
 
    assert(isPowerOf2(m_directory_shared_frac));
    m_dir_shared_frac_bits = floorLog2(m_directory_shared_frac);
 
    assert(isPowerOf2(m_num_cpus));
    m_random_add_gen_bits = RubySystem::getMemorySizeBits() - RubySystem::getBlockSizeBits() - floorLog2(m_num_cpus) - m_dir_shared_frac_bits;
    assert(m_random_add_gen_bits >= 0);
 
    DPRINTF(amTrace, "RubySystem::getMemorySizeBits() = %d\n", RubySystem::getMemorySizeBits());
    DPRINTF(amTrace, "RubySystem::getBlockSizeBits() = %d\n", RubySystem::getBlockSizeBits());
    DPRINTF(amTrace, "m_num_cpus_bits = %d\n", floorLog2(m_num_cpus));
    DPRINTF(amTrace, "m_dir_shared_frac_bits = %d\n", m_dir_shared_frac_bits);
    DPRINTF(amTrace, "m_random_add_gen_bits = %d\n", m_random_add_gen_bits);
    DPRINTF(amTrace, "CPI_IOPS = %f\n", CPI_IOPS);
    DPRINTF(amTrace, "CPI_FLOPS = %f\n", CPI_FLOPS);

    // get the total number of cache lines
    m_num_cache_lines = RubySystem::getMemorySizeBytes()/RubySystem::getBlockSizeBytes();

    assert(ports.size() > 0);
    if (m_num_threads < m_num_cpus) {
        num_of_mapped_processors = m_num_threads;
    } else {
        num_of_mapped_processors = m_num_cpus;
    }
 
    // check if the MAX_REQUEST_SIZE is less than the block size
    if (MAX_REQUEST_SIZE > RubySystem::getBlockSizeBytes())
	panic("Error in Dragon Tester!!: MAX_REQUEST_SIZE (%d) is greater than Ruby block size (%d)!!", MAX_REQUEST_SIZE, RubySystem::getBlockSizeBytes());

    //DPRINTF(amTrace, "Number of mapped processors = %d\n", num_of_mapped_processors);
    m_last_progress_vector.resize(num_of_mapped_processors);
    for (int i=0; i<m_last_progress_vector.size(); i++) {
        m_last_progress_vector[i] = 0;
    }
 
    eventMap = new StaticDeque<DragonEvent *>*[m_num_threads];
    int max_events_size = READ_BLOCK + MIN_EVENTS_SIZE;
    for (int i = 0; i < m_num_threads; i++)
      eventMap[i] = new StaticDeque<DragonEvent *>(max_events_size);

    curEventListPos.resize(m_num_threads);
    threadStatistics.resize(m_num_threads);
    lastMsgTriggerTime.resize(m_num_threads);
    roundTripTime.resize(m_num_threads);
    maxRTT.resize(m_num_threads);
    inputFilePointer.resize(m_num_threads);
    outputFilePointer.resize(m_num_threads);
 
    threadMap.resize(num_of_mapped_processors);
    threadStartedMap.resize(m_num_threads);
    for (int i = 0; i < m_num_threads; i++) 
      threadStartedMap[i] = false;
    threadStartedMap[0] = true;

    g_system_ptr->thread_cycles_per_event.resize(m_num_threads);
    for (int i = 0; i < m_num_threads; i++) 
        g_system_ptr->thread_cycles_per_event[i] = 0;

//  Paco (11/6) - Adding a threadMutexMap to check if thread is holding a mutex lock. Initializing.
    threadMutexMap.resize(m_num_threads);
    for (int i = 0; i < m_num_threads; i++)
        threadMutexMap[i] = false;

//  Paco (11/12) - Adding a threadContMap to check if thread is done with a barrier. Initializing.
    threadContMap.resize(m_num_threads);
    for (int i = 0; i < m_num_threads; i++)
        threadContMap[i] = false;


//  Paco - Adding a printThreadEventCounters.
    printThreadEventCounters = 0;

    roiFlag = false;
    worker_thread_count = 0;

    for (int i = 0; i < num_of_mapped_processors; i++)
      coreEvents.push_back(new DragonCoreEvent(this, i));
 
    stopped = false;
 
    for (int i = 0; i < m_num_threads; i++) {
        curEventListPos[i] = 0;
        threadStatistics[i].ThreadID = i;
        lastMsgTriggerTime[i] = 0;
        maxRTT[i]=0;
 
        // initialize pointers to the input files
        char fileName[256];
        sprintf(fileName, "%s/sigil.events.out-%d.gz", m_eventDir.c_str(), i+1);
        inputFilePointer[i] = new igzstream(fileName);
        if (inputFilePointer[i]->fail()) {
            panic("Failed to open file: %s\n", fileName);
        } //else
            //DPRINTF(amTrace, "Opened event file: %s\n", fileName);
 
        // initialize pointer to the output files
        char eventFileName[256];
        sprintf(eventFileName, "%s/eventTimeOutput-%d.csv.gz", m_outputDir.c_str(), i+1);
        outputFilePointer[i] = new ogzstream(eventFileName);
 
        if (outputFilePointer[i]->fail()) {
            panic("ERROR!: Not able to create event file %s. Aborting!!\n", eventFileName);
        }
    }

    // Vasil - parser for the sigil.pthread.out

    char fileName[256];
    sprintf(fileName, "%s/sigil.pthread.out", m_eventDir.c_str());

    pthreadFilePointer.open(fileName);

    if (!pthreadFilePointer.is_open()) {
      exit(EXIT_FAILURE);
    }

    else {

      while (pthreadFilePointer.good() && !pthreadFilePointer.eof()) {

      string thisVasil;
      if (getline(pthreadFilePointer, thisVasil)) {
            size_t hashPos = thisVasil.rfind('#');
            size_t starPos = thisVasil.rfind('*');

            if (hashPos != string::npos)
                processAddressToID(thisVasil, hashPos);
            else
                processBarrierEvent(thisVasil, starPos);
        }
        else break;
     }
    pthreadFilePointer.close();
    }

    //vasil - parser for the sigil.pthread.out



    pthreadFilePointer.close();

    initialThreadMapping();
 
    generateEventQueue();
 
    //DPRINTF(amTrace, "Done generating event queues...scheduling dragon tester event\n");    
}
 
MasterPort* DragonTester::getCpuPort(int idx)
{
    assert(idx >= 0 && idx < ports.size());
 
    return ports[idx];
}
 
MasterPort & DragonTester::getMasterPort(const std::string &if_name, int idx)
{
    if (if_name != "cpuPort") {
        // pass it along to our super class
        return MemObject::getMasterPort(if_name, idx);
    } else {
        if (idx >= static_cast<int>(ports.size())) {
            panic("DragonTester::getMasterPort: unknown index %d\n", idx);
        }
 
        return *ports[idx];
    }
}

bool DragonTester::CpuPort::recvTimingResp(PacketPtr pkt)
{
 
    tester->hitCallback(id, pkt);
 
    // Now that the tester has completed, delete the 
    // the packet, then return
    delete pkt->req;
    delete pkt;
    return true;
}

void DragonTester::wakeup()
{
 
    //DPRINTF(amTrace, "Master thread woken up\n");
    // if the event list has been depleted read the file again
    replenishEvents();
 
    // Calling this again to avoid cases where the individual core events are 
    // not going to be scheduled again and the master thread is woken up later
    createSubEvents();
 
    // Terminates the simulation after checking if all the events are done
    checkCompletion();
 
    schedule(dragonStartEvent, curTick() + m_master_wakeup_frequency);
}
 
void DragonTester::wakeup(int procID)
{
 
//     Paco - For every 50,000 wakeup counts, print all the thread's EventID#
    printThreadEvents();

//    DPRINTF(amTrace, "Core thread number %d woken up\n", procID);
    // create subevents for the thread at the head of the core list
    // it is not necessary that this will happen always.
    // If the subevents have been created, simply skip
    createSubEvents(procID);
 
    // progress events further
    progressEvents(procID);
 
    // Swap threads in cores if allowed
    swapStalledThreads(procID);
 
    // check if there are any deadlocks in the events
    //checkForDeadlock(procID);
}

void DragonTester::printThreadEvents() //Paco
{
        if (printThreadEventCounters == PRINT_EVENT_WAKE_COUNT) {
             for(int i = 0; i<m_num_threads; i++) {
                  if(!eventMap[i]->empty()) {
                       DPRINTF(printEventFull, "Thread %d is on Event %d\n", i, eventMap[i]->front()->EventID);
                  }
             }
             for(int i = 0; i<m_num_cpus; i++) {
                 if(i<m_num_threads) {
                     DPRINTF(printEventFull, "Thread %d is on top Core %d\n", threadMap[i][0], i);
                 }
                 else {
                     DPRINTF(printEventFull, "No Thread is on top Core %d\n", i);
                 }
             }
             printThreadEventCounters = 0;
        }
        else {
             printThreadEventCounters++;
        }
}

void DragonTester::printEvent(int threadID, bool start_end) //Paco - (8/14) Used to instrument thread/Events # before/after event completion
{
    if(!eventMap[threadID]->empty()) {
        if(!start_end) {
            DPRINTF(printEvent, "Starting %d, %d\n", threadID, eventMap[threadID]->front()->EventID);
        }
        else {
            DPRINTF(printEvent, "Finished %d, %d\n", threadID, eventMap[threadID]->front()->EventID);
        }
    }
}

void DragonTester::checkCompletion()
{
    bool terminate = true;
    for (int i=0; i < m_num_threads; i++) {
        if (!eventMap[i]->empty()) {
            terminate = false;
            break;
        }
    }
 
    if (terminate && !stopped) {
        outputRTT_hist();
        for (int i = 0; i < m_num_threads; i++) {
            if (inputFilePointer[i] != NULL) {
                inputFilePointer[i]->close();
                delete inputFilePointer[i];
            }
            if (outputFilePointer[i] != NULL) {
                outputFilePointer[i]->close();
                delete outputFilePointer[i];
            }
        }
        //Added by Sid to keep track of % of cycles from Shared Reads/Writes and % of cycles from Local Read/Writes
        DPRINTF(amTrace, "Shared Event Histogram: %d, %d, %d, %d, %d, %d, %d\n",sharedEventHist[0],sharedEventHist[1],sharedEventHist[2],sharedEventHist[3],sharedEventHist[4],sharedEventHist[5],sharedEventHist[6]);
        DPRINTF(amTrace, "Local Event Histogram: %d, %d, %d, %d, %d, %d, %d\n",localEventHist[0],localEventHist[1],localEventHist[2],localEventHist[3],localEventHist[4],localEventHist[5],localEventHist[6]);
        //Done additions by Sid

        //Print out memory events for last barrier to completion
        g_system_ptr->printMemoryEventsInBarrier(); //Paco (8/20) Print out memory events after each barrier

        stopped = true;
        exitSimLoop("DragonTester completed");
    }
}
 
// Swapping of threads is allowed if:
//      1) the head event in the currently allocated thread has 
//         finished and one of the other threads is ready
//      2) if the head event is a communication event and has been 
//         waiting for its dependency for more than a predefined number 
//         of cycles then check if one of the other threads is ready
//
// One of the other threads is ready when:
//      1) the head event is a computation event
//      2) the head event is a communication event and all the dependecies have been statisfied
//
// The swapThreads() function is called everytime an event finishes
// The swapStalledThreads() function is called everytime the driver is woken up
void DragonTester::swapThreads(int procID)
{

    int numThreads = threadMap[procID].size();
//  int oldThread = threadMap[procID][0];
    if(numThreads > 1) {
        for (int i = 1; i < numThreads; i++) {
            int topThreadID = threadMap[procID][i];
            if (!threadStartedMap[topThreadID]) continue;
 
            //if (eventMap[topThreadID].size() == 0)
            if (eventMap[topThreadID]->empty())
                continue;
 
            DragonEvent* topEvent = eventMap[topThreadID]->front();
            if (topEvent->Type == TYPE_COMPUTATION) {   // no dependencies in computation events
                //DPRINTF(amTrace, "%d: Swapping threads: Core %d; newThread %d; oldThread %d\n", g_eventQueue_ptr->getTime(), procID, threadMap[procID][i], oldThread);
                moveThreadToHead(procID, i);
                break;
            } 
            else if (topEvent->Type == TYPE_PTHREAD_API) {
                //DPRINTF(amTrace, "%d: Swapping threads: Core %d; newThread %d; oldThread %d\n", g_eventQueue_ptr->getTime(), procID, threadMap[procID][i], oldThread);
                moveThreadToHead(procID, i);
                break;
            }
            else {
                // check all the dependencies for this communication event - Paco (11/6) Allow swap for comm event if thread has a mutx
                if (checkAllCommDependencies(topEvent) || (threadMutexMap[topEvent->ThreadID] == true)) {
                    //DPRINTF(amTrace, "%d: Swapping threads: Core %d; newThread %d; oldThread %d\n", g_eventQueue_ptr->getTime(), procID, threadMap[procID][i], oldThread);
                    moveThreadToHead(procID, i);
                    break;
                }
            }
        }
    }
}
 
void DragonTester::swapStalledThreads(int procID)
{
    int eventThreadID = threadMap[procID].front();
    if (threadMap[procID].size() > 1) {

        // Paco - Adding a check to swapStalledThreads to only swap if the other threads have started.
        if (eventMap[eventThreadID]->empty() || !threadStartedMap[eventThreadID]) {
            int numThreads = threadMap[procID].size();
            for (int i = 1; i < numThreads; i++) {
                int topThreadID = threadMap[procID][i];
                if(!threadStartedMap[topThreadID]) continue;
                //DPRINTF(amTrace, "%d: Swapping stalled threads: Core %d; newThread %d; oldThread %d\n", g_eventQueue_ptr->getTime(), procID, threadMap[procID][i], eventThreadID );
                swapThreads(procID);
                break;
            }
//    Paco - 11/5 - Moved Scheduling Catch. If you are trying to swap nonstarted or ended threads, this prevents the core from stalling.
            if (!(coreEvents[procID]->scheduled())) {
                 schedule(*(coreEvents[procID]), g_eventQueue_ptr->getTime() + 1); //wait 10k cycles in corner case
            }
            return;
        }
 
        DragonEvent *thisEvent = eventMap[eventThreadID]->front();

	if (!thisEvent->subEventsCreated)
	    createSubEvents(procID, true, eventThreadID);
 
        if (thisEvent->Type == TYPE_COMMUNICATION) {    // need to check only communication events
            subEvent* topSubEvent = &(thisEvent->subEventList->front()); //XXX TODO
 
            // only if the topSubEvent has not triggered a message. If it has then the deadlock would be detected by the deadlock detection code 
            // because of the message not being delivered back in time
            if (!(topSubEvent->msgTriggered)) {
                if ((g_eventQueue_ptr->getTime() - topSubEvent->initialTriggerTime) > MAX_COMM_WAIT) {
                    swapThreads(procID);
//    Paco - 10/24 - Added Scheduling Catch
                    if (!(coreEvents[procID]->scheduled())) {
                         schedule(*(coreEvents[procID]), g_eventQueue_ptr->getTime() + 1);
                    }
                }
            }
        }
//    Paco - Added Pthread JOIN swapping (Swap if join event and waiting thread isn't completed)
        if ((thisEvent->Type == TYPE_PTHREAD_API) && (thisEvent->PthreadType == P_JOIN) && (curEventListPos[AddresstoIdMap[thisEvent->pth_addr]] != -1)) {
//    Paco - 10/23: Fixing bug where thread swaps in another thread waiting for a pthread joins but doesn't schedule.
//        moveThreadToHead(procID, 1);
            swapThreads(procID);
//    Paco - 10/24 - Added Scheduling Catch
             if (!(coreEvents[procID]->scheduled())) {
                  schedule(*(coreEvents[procID]), g_eventQueue_ptr->getTime() + 1);
             }
        }

//   Paco (11/5) - Added Pthread MUTEX swapping (Swap if thread is waiting on a mutex)
        if ((thisEvent->Type == TYPE_PTHREAD_API) && (thisEvent->PthreadType == P_MUTEX_LK) && (threadMutexMap[thisEvent->ThreadID] == false)) {
            swapThreads(procID);
            if (!(coreEvents[procID]->scheduled())) {
                schedule(*(coreEvents[procID]), g_eventQueue_ptr->getTime() + 1);
            }
        }

    } else if(threadMap[procID].size() == 1) { //Paco (11/12) - Fixed scheduling when thread is stalled on barrier but not scheduled again **TEST COMMENTING OUT**
        if((!eventMap[eventThreadID]->empty())) {
            DragonEvent *thisEvent = eventMap[eventThreadID]->front();
            if ((thisEvent->Type == TYPE_PTHREAD_API) && (thisEvent->PthreadType == P_BARRIER_WT)) {
                if (!(coreEvents[procID]->scheduled())) {
                    schedule(*(coreEvents[procID]), g_eventQueue_ptr->getTime() + 1);
                }
           }

        }
    }
}
 
void DragonTester::moveThreadToHead(int procID, int threadID)
{
    for (int i = 0; i < threadID; i++) {
        // move the thread at the head to the end
        threadMap[procID].push_back(threadMap[procID][0]);      // copy to the end
        threadMap[procID].erase(threadMap[procID].begin());     // delete from the front        
    }
}
 
bool DragonTester::checkAllCommDependencies(DragonEvent* thisEvent)
{
    assert(thisEvent->Type == TYPE_COMMUNICATION);
    bool check = true;
    for(unsigned long i = 0; i < thisEvent->comm_preRequisiteEvents.size(); i++) {
        check &= checkCommDependency(thisEvent->comm_preRequisiteEvents[i], thisEvent->ThreadID);    }
 
    return check;}
 
bool DragonTester::checkCommDependency(sharedInfo* commEvent, int ThreadID){
    // this is the workaround for the 30001 thread issue
    // if the producer thread ID is 30001 then add the addresses to the shared map and return true
    if (commEvent->prodThreadID == 30000) {// 30000 because threadID - 1 while reading it in
        return true;
    }
 
    // if the producer event queues top event has an eventID greater that the prodEventID 
    // then it implies that the dependency is satisfied
    if (!eventMap[commEvent->prodThreadID]->empty()) {
        if (eventMap[commEvent->prodThreadID]->front()->EventID > commEvent->prodEventID)
            return true;
        else
            return false;
    } else {
        return true;
    }
}
 
void DragonTester::createSubEvents()
{
    for (int i=0; i < num_of_mapped_processors; i++) {
//        int eventThreadID = threadMap[i].front();
        createSubEvents(i);
    }
}

void DragonTester::initialThreadMapping() {
    // initialize it in a simple round robin fashion
    // in the future if any other mapping is to be used changes should be made in this function
    for (int i = 0; i < m_num_threads; i++) {
        threadMap[i%num_of_mapped_processors].push_back(i);
    }
}
 
void DragonTester::replenishEvents() {
    for (int i=0; i < m_num_threads; i++) {
        // the curEventListPos will be equal to -1 if it is at the end of a file
        // we need to skip these files to avoid un-necessary i/o operations.
        if ((eventMap[i]->size() < MIN_EVENTS_SIZE) && (curEventListPos[i] != -1))
            extendEventQueue(i);
    }
}

void DragonTester::replenishEvents(unsigned int threadID) {
  // the curEventListPos will be equal to -1 if it is at the end of a file
  // we need to skip these files to avoid un-necessary i/o operations.
  if ((eventMap[threadID]->size() < MIN_EVENTS_SIZE) && (curEventListPos[threadID] != -1))
    extendEventQueue(threadID);
}
 
void DragonTester::generateEventQueue(){
    for (int i=0; i < m_num_threads; i++) {
        readEventFile(i);
    }
}
 
void DragonTester::extendEventQueue(unsigned int threadID)
{
    readEventFile(threadID);
}

void DragonTester::checkForDeadlock(int procID)
{
    Time current_time = g_eventQueue_ptr->getTime();
    for (int i = 0; i < threadMap[procID].size(); i++) {
        if (!eventMap[threadMap[procID][i]]->empty()) {
            if (eventMap[threadMap[procID][i]]->front()->Type == TYPE_PTHREAD_API)
                continue;
            if ((current_time - m_last_progress_vector[procID]) > m_deadlock_threshold) {
                warn("Thread event type: %d\n", eventMap[threadMap[procID][i]]->front()->Type);
                if (eventMap[threadMap[procID][i]]->front()->Type == TYPE_PTHREAD_API)
                  warn("  Pthread event type: %d\n", eventMap[threadMap[procID][i]]->front()->PthreadType);
                warn("Current time: %d\n", current_time);
                warn("Last progress: %d\n", m_last_progress_vector[procID]);
                warn("No activity for: %d cycles\n", current_time - m_last_progress_vector[procID]);
                warn("Thread number: %d\n", procID);
                panic("Deadlock detected.");
            }
            break;  // if we have tested it once for a particular processor we dont need ot do it again
        }
    }
}

void DragonTester::outputRTT_hist()
{
    for (int i=0; i < m_num_threads; i++) {
        FILE *eventFile;
        char eventFileName[256];
        sprintf(eventFileName, "%s/rttHistOutput-%d.csv", m_outputDir.c_str(), i+1);
        eventFile = fopen(eventFileName,"a");
 
        if (eventFile != NULL) {
            for (int lat = 0; lat <= maxRTT[i]; lat ++) {
                m5::hash_map<int, int>::iterator rttIt = roundTripTime[i].find(lat);
                // check if the hash exists
                if (rttIt != roundTripTime[i].end()) {
                    char thisEntry[128];
                    sprintf(thisEntry,"%d,%d\n", lat, rttIt->second);
                    fputs(thisEntry, eventFile);
                }
            }
            fclose(eventFile);
        } else {
            //DPRINTF(amTrace, "WARNING!: Event time output file not created or opened (%s)\n", eventFileName);
        }
    }
}

void DragonTester::print(ostream& out) const
{
   out << "[Tester]" << endl;
}
 
DragonTester* DragonTesterParams::create()
{
    return new DragonTester(this);
}

void DragonTester::hitCallback(NodeID proc, PacketPtr pkt)
{
 
    assert(proc < num_of_mapped_processors);
    int eventThreadID = threadMap[proc].front();
    DragonEvent *thisEvent = eventMap[eventThreadID]->front();
 
    if (!(thisEvent->subEventList->front().msgTriggered)) {
       // DPRINTF(amTrace, "%d: Message not triggered but received hitCallback: nodeID: %d Event: \n", g_eventQueue_ptr->getTime(), proc);
        //DPRINTF(amTrace, *thisEvent);
        //DPRINTF(amTrace, "\nsubEventSize: %d\n", thisEvent->subEventList->size());
        return;
    }
 
//    assert(thisEvent->subEventList->front().msgTriggered);
    m_last_progress_vector[proc] = g_eventQueue_ptr->getTime();
 
    // round trip time (rtt) statistics calculation
    int thisMsgRTT = g_eventQueue_ptr->getTime() - lastMsgTriggerTime[eventThreadID];

    //Added by Sid to keep track of % of cycles from Shared Reads/Writes and % of cycles from Local Read/Writes
    if(thisEvent->Type == TYPE_COMMUNICATION){ //Divide into logarithmic buckets because latencies increase by orders of magnitude.
      if(thisMsgRTT < 10)
        sharedEventHist[0]++;
      else if(thisMsgRTT < 50)
        sharedEventHist[1]++;
      else if(thisMsgRTT < 100)
        sharedEventHist[2]++;
      else if(thisMsgRTT < 300)
        sharedEventHist[3]++;
      else if(thisMsgRTT < 600)
        sharedEventHist[4]++;
      else if(thisMsgRTT < 1000)
        sharedEventHist[5]++;
      else if(thisMsgRTT < 10000)
        sharedEventHist[6]++;
    }
    else if(thisEvent->Type == TYPE_COMPUTATION){
      if(thisMsgRTT < 10)
        localEventHist[0]++;
      else if(thisMsgRTT < 50)
        localEventHist[1]++;
      else if(thisMsgRTT < 100)
        localEventHist[2]++;
      else if(thisMsgRTT < 300)
        localEventHist[3]++;
      else if(thisMsgRTT < 600)
        localEventHist[4]++;
      else if(thisMsgRTT < 1000)
        localEventHist[5]++;
      else if(thisMsgRTT < 10000)
        localEventHist[6]++;
    }
    //Done additions by Sid
    m5::hash_map<int, int>::iterator rttIt = roundTripTime[eventThreadID].find(thisMsgRTT);
    // check if the hash exists, if yes increment it, if not add it
    if (rttIt != roundTripTime[eventThreadID].end()) {
        // found it --> so increment it
        (rttIt->second)++;
    } else {
        // insert it
        roundTripTime[eventThreadID].insert(m5::hash_map<int, int>::value_type(thisMsgRTT, 1));
    }
    // update the max RTT
    if (thisMsgRTT > maxRTT[eventThreadID])
        maxRTT[eventThreadID] = thisMsgRTT;
 
    // update the thread stats
    threadStatistics[eventThreadID].totalRTT += thisMsgRTT;
//    DPRINTF(amTrace, "%d: hitCallback: nodeID: %d Event: \n", g_eventQueue_ptr->getTime(), proc);
//    DPRINTF(amTrace, "\nsubEventSize: %d\n", thisEvent->subEventList->size());
 
    threadStatistics[eventThreadID].numSubEvents++;
    thisEvent->subEventList->pop_front();
    // then check if all subevents are done-- if yes then the event has finished and we can delete it
    if (thisEvent->subEventList->empty()) {
        printEvent(eventThreadID, true); //Paco (8/14) - Event was just completed -> Print Event # on Thread #

        //Cycles Per 1k Events per thread:
        if ((thisEvent->EventID % 1000) == 0){
            DPRINTF(roi,"%d,%1.2f\n",thisEvent->ThreadID,(g_eventQueue_ptr->getTime() - g_system_ptr->thread_cycles_per_event[thisEvent->ThreadID])/1000.0);
            g_system_ptr->thread_cycles_per_event[thisEvent->ThreadID] = g_eventQueue_ptr->getTime();
        }

        delete eventMap[eventThreadID]->pop_front();
        swapThreads(proc);
        if(!eventMap[eventThreadID]->empty()) {
            //DPRINTF(amTrace, "Testing thisEvent->ThreadID:%d\n",thisEvent->ThreadID);
            //if(thisEvent->ThreadID >m_num_threads || thisEvent->ThreadID < 0) {
            //     DPRINTF(amTrace, "ThreadIDBad\n");
            printEvent(eventThreadID, false); //Paco (8/14) - Event is just starting -> Print Event # on Thread #
            thisEvent = eventMap[eventThreadID]->front();
            //}
	    replenishEvents(thisEvent->ThreadID);
        }
        schedule(*(coreEvents[proc]), g_eventQueue_ptr->getTime() + 1);
//      DPRINTF(amTrace, "Core %d scheduled for %d\n", proc, g_eventQueue_ptr->getTime() + 1);
    } else {        // if not pull up the next subevent and schedule based on its trigger time
        subEvent* newSubEvent = &(thisEvent->subEventList->front());
        newSubEvent->triggerTime = CPI_IOPS * newSubEvent->numIOPS + CPI_FLOPS * newSubEvent->numFLOPS + MEM_ACCESS_TIME * newSubEvent->numMEM_OPS + g_eventQueue_ptr->getTime() + 1;
        newSubEvent->initialTriggerTime = newSubEvent->triggerTime;
        schedule(*(coreEvents[proc]), newSubEvent->triggerTime);
//      DPRINTF(amTrace, "Core %d scheduled for %d\n", proc, newSubEvent->triggerTime);
    }
}

void DragonTester::processCommEvent(string thisEvent, size_t hashPos)
{
    string eventInfo;
 
    eventInfo = thisEvent.substr(0, hashPos-1);
    DragonEvent *newEvent = new DragonEvent();
    newEvent->subEventList = NULL;
    //newEvent->subEventList = new StaticDeque<subEvent>(); //XXX TODO
    newEvent->Type = TYPE_COMMUNICATION;
 
    size_t curPos = -1;
    for (int i = 0; i < COMM_ENTRIES; i++) {
        size_t nextPos = eventInfo.find(',', curPos + 1);
        string thisEntry = eventInfo.substr(curPos+1, nextPos - (curPos+1));
        curPos = nextPos;
 
        switch(i) {
            case 0:
            {
                newEvent->EventID = strtoul(thisEntry.c_str(), NULL, 0);
                break;
            }
            case 1:
            {
                newEvent->ThreadID = atoi(thisEntry.c_str()) - 1;
                break;
            }
            default:
                assert(false);
        }
    }
 
    string dependencyInfo = thisEvent.substr(hashPos + 2);
 
    size_t curHash_pos = -2;
    do {
        size_t nextHash_Pos = dependencyInfo.find('#', curHash_pos+2);
        string thisDependencyInfo = dependencyInfo.substr(curHash_pos+2, (nextHash_Pos - 1) - (curHash_pos+2));
        curHash_pos = nextHash_Pos;
 
        unsigned int thisProdThreadID=0;
        unsigned long thisProdEventID=0;
        unsigned long this_mem_start_range=0;
        unsigned long this_mem_end_range=0;
        size_t curSpace_pos = -1;
        for (int i = 0; i < COMM_SHARED_INFO_ENTRIES; i++) {
            size_t nextSpace_pos = thisDependencyInfo.find(' ', curSpace_pos + 1);
            string thisEntry = thisDependencyInfo.substr(curSpace_pos + 1, nextSpace_pos - (curSpace_pos + 1));
            curSpace_pos = nextSpace_pos;
 
            switch(i) {
                case 0:
                {
                    thisProdThreadID = atoi(thisEntry.c_str()) - 1;
                    break;
                }
                case 1:
                {
                    thisProdEventID = strtoul(thisEntry.c_str(), NULL, 0);
                    break;
                }
                case 2:
                {
                    this_mem_start_range = strtoul(thisEntry.c_str(), NULL, 0);
                    break;
                }
                case 3:
                {
                    this_mem_end_range = strtoul(thisEntry.c_str(), NULL, 0);
                    break;
                }
                default:
                    assert(false);
            }
        }

	uint64 numBytes = (uint64)(this_mem_end_range - this_mem_start_range + 1);    // +1 to offset the count of the start_range

	uint64 lineStart = (uint64)((this_mem_start_range/RubySystem::getBlockSizeBytes()) % m_num_cache_lines);
	uint64 startOffset = (uint64)(this_mem_start_range % RubySystem::getBlockSizeBytes());
	uint64 curAdd = (lineStart << RubySystem::getBlockSizeBits()) | startOffset;

	uint64 prev_req_size = 0;
	do {
            uint64 this_req_size;

	    if (numBytes >= MAX_REQUEST_SIZE)
                this_req_size = MAX_REQUEST_SIZE;
            else
                this_req_size = numBytes;
//bug fix for the ruby assert crash (ankit);
            curAdd = (curAdd + prev_req_size) % RubySystem::getMemorySizeBytes();

	    int residual = this_req_size - (int)(RubySystem::getBlockSizeBytes() - (int)(curAdd % RubySystem::getBlockSizeBytes()));

	    if (residual > 0) { // split it between 2 lines
		sharedInfo* thisLine = new sharedInfo(thisProdThreadID, thisProdEventID, curAdd, this_req_size - residual);
		newEvent->comm_preRequisiteEvents.push_back(thisLine);
		prev_req_size = (uint64)(this_req_size - residual);
	    } else {
		sharedInfo* thisLine = new sharedInfo(thisProdThreadID, thisProdEventID, curAdd, this_req_size);
		newEvent->comm_preRequisiteEvents.push_back(thisLine);
		prev_req_size = this_req_size;
	    }
	    numBytes -= prev_req_size;
	} while(numBytes > 0);
    } while(curHash_pos != string::npos);


    if(newEvent->ThreadID > m_num_threads || newEvent->ThreadID < 0) {
        DPRINTF(amTrace, "ThreadIDBad\n");
    }      
    eventMap[newEvent->ThreadID]->push_back(newEvent);
}

void DragonTester::processCompMainEvent(string thisEvent, DragonEvent *newEvent)
{
    int elementCount = count(thisEvent.begin(), thisEvent.end(), ',');
 
    if(elementCount < (COMP_ENTRIES - 1)) {
        panic("Incorrect element count in computation event. Number of elements: %d\n", elementCount);
    } else {
        size_t curPos = -1;
        for (int i = 0; i < COMP_ENTRIES; i++) {
            size_t nextPos = thisEvent.find(',', curPos + 1);
            string thisEntry = thisEvent.substr(curPos+1, nextPos - (curPos+1));
            curPos = nextPos;
 
            switch(i) {
                case 0:
                    newEvent->EventID = strtoul(thisEntry.c_str(), NULL, 0);
                    break;
                case 1:
                    newEvent->ThreadID = atoi(thisEntry.c_str()) - 1;
                    break;
                case 2:
                    newEvent->compIOPS = strtoul(thisEntry.c_str(), NULL, 0);
                    break;
                case 3:
                    newEvent->compFLOPS = strtoul(thisEntry.c_str(), NULL, 0);
                    break;
                case 4:
                    newEvent->compMem_reads = strtoul(thisEntry.c_str(), NULL, 0);
                    break;
                case 5:
                    newEvent->compMem_writes = strtoul(thisEntry.c_str(), NULL, 0);
                    break;
                default:
                    assert(false);
                    break;
            }
        }
    }
}

void DragonTester::processCompWriteEvent(string dependencyInfo, DragonEvent *newEvent)
{
    size_t curDollar_pos = -2;
    do {
        size_t nextDollar_Pos = dependencyInfo.find('$', curDollar_pos+2);
        string thisDependencyInfo = dependencyInfo.substr(curDollar_pos+2, (nextDollar_Pos - 1) - (curDollar_pos+2));
        curDollar_pos = nextDollar_Pos;
 
        unsigned long this_mem_start_range;
        unsigned long this_mem_end_range;
        size_t curSpace_pos = -1;
        for (int i = 0; i < COMP_WRITE_ENTRIES; i++) {
            size_t nextSpace_pos = thisDependencyInfo.find(' ', curSpace_pos + 1);
            string thisEntry = thisDependencyInfo.substr(curSpace_pos + 1, nextSpace_pos - (curSpace_pos + 1));
            curSpace_pos = nextSpace_pos;
 
            switch(i) {
                case 0:
                    this_mem_start_range = strtoul(thisEntry.c_str(), NULL, 0);
                    break;
                case 1:
                    this_mem_end_range = strtoul(thisEntry.c_str(), NULL, 0);
                    break;
                default:
                    assert(false);
                    break;
            }
        }
 
        vector< sharedInfo* > this_comp_writeEvent;

        uint64 numBytes = (uint64)(this_mem_end_range - this_mem_start_range + 1);    // +1 to offset the count of the start_range

        uint64 lineStart = (uint64)((this_mem_start_range/RubySystem::getBlockSizeBytes()) % m_num_cache_lines);
        uint64 startOffset = (uint64)(this_mem_start_range % RubySystem::getBlockSizeBytes());
        uint64 curAdd = (lineStart << RubySystem::getBlockSizeBits()) | startOffset;

        uint64 prev_req_size = 0;
        do {
            uint64 this_req_size;

            if (numBytes >= MAX_REQUEST_SIZE)
                this_req_size = MAX_REQUEST_SIZE;
            else
                this_req_size = numBytes;
//bug fix for ruby assert crash (ankit)
            curAdd = (curAdd + prev_req_size) % RubySystem::getMemorySizeBytes();

            int residual = this_req_size - (int)(RubySystem::getBlockSizeBytes() - (int)(curAdd % RubySystem::getBlockSizeBytes()));

            if (residual > 0) { // split it between 2 lines
                sharedInfo* thisLine = new sharedInfo(0, 0, curAdd, this_req_size - residual);
		newEvent->comp_writeEvents.push_back(thisLine); //XXX TODO
                prev_req_size = (uint64)(this_req_size - residual);
            } else {
                sharedInfo* thisLine = new sharedInfo(0, 0, curAdd, this_req_size);
		newEvent->comp_writeEvents.push_back(thisLine); //XXX TODO
                prev_req_size = this_req_size;
            }
            numBytes -= prev_req_size;
        } while(numBytes > 0);
 
    } while(curDollar_pos != string::npos);
}

void DragonTester::processCompReadEvent(string dependencyInfo, DragonEvent *newEvent)
{
    size_t curStar_pos = -2;
    do {
        size_t nextStar_Pos = dependencyInfo.find('*', curStar_pos+2);
        string thisDependencyInfo = dependencyInfo.substr(curStar_pos+2, (nextStar_Pos - 1) - (curStar_pos+2));
        curStar_pos = nextStar_Pos;
 
        unsigned long this_mem_start_range;
        unsigned long this_mem_end_range;
        size_t curSpace_pos = -1;
        for (int i = 0; i < COMP_READ_ENTRIES; i++) {
            size_t nextSpace_pos = thisDependencyInfo.find(' ', curSpace_pos + 1);
            string thisEntry = thisDependencyInfo.substr(curSpace_pos + 1, nextSpace_pos - (curSpace_pos + 1));
            curSpace_pos = nextSpace_pos;
 
            switch(i) {
                case 0:
                    this_mem_start_range = strtoul(thisEntry.c_str(), NULL, 0);
                    break;
                case 1:
                    this_mem_end_range = strtoul(thisEntry.c_str(), NULL, 0);
                    break;
                default:
                    assert(false);
                    break;
            }
        }
 
        vector< sharedInfo* > this_comp_readEvent;

        uint64 numBytes = (uint64)(this_mem_end_range - this_mem_start_range + 1);    // +1 to offset the count of the start_range

        uint64 lineStart = (uint64)((this_mem_start_range/RubySystem::getBlockSizeBytes()) % m_num_cache_lines);
        uint64 startOffset = (uint64)(this_mem_start_range % RubySystem::getBlockSizeBytes());
        uint64 curAdd = (lineStart << RubySystem::getBlockSizeBits()) | startOffset;

        uint64 prev_req_size = 0;
        do {
            uint64 this_req_size;

            if (numBytes >= MAX_REQUEST_SIZE)
                this_req_size = MAX_REQUEST_SIZE;
            else
                this_req_size = numBytes;
//bug fix for ruby assert crash (ankit)
            curAdd = (curAdd + prev_req_size) % RubySystem::getMemorySizeBytes();

            int residual = this_req_size - (int)(RubySystem::getBlockSizeBytes() - (int)(curAdd % RubySystem::getBlockSizeBytes()));

            if (residual > 0) { // split it between 2 lines
                sharedInfo* thisLine = new sharedInfo(0, 0, curAdd, this_req_size - residual);
		newEvent->comp_readEvents.push_back(thisLine); //XXX TODO
                prev_req_size = (uint64)(this_req_size - residual);
            } else {
                sharedInfo* thisLine = new sharedInfo(0, 0, curAdd, this_req_size);
		newEvent->comp_readEvents.push_back(thisLine); //XXX TODO
                prev_req_size = this_req_size;
            }
            numBytes -= prev_req_size;
        } while(numBytes > 0);
    } while(curStar_pos != string::npos);
}

void DragonTester::processCompEvent(string thisEvent)
{
    size_t dollarPos = thisEvent.find('$');
    size_t starPos = thisEvent.find('*');
 
    DragonEvent *newEvent = new DragonEvent();
    //newEvent->subEventList = new StaticDeque<subEvent>();
    newEvent->subEventList = NULL;
    newEvent->Type = TYPE_COMPUTATION;
 
    string eventInfo;
    string writeEventInfo;
    string readEventInfo;
 
    string remainder_str = thisEvent;
 
    // start from the star
    if (starPos != string::npos) { // if a star exists
        readEventInfo = remainder_str.substr(starPos + 2);
        remainder_str = remainder_str.substr(0, starPos - 1);
    }
 
    if (dollarPos != string::npos) {
        writeEventInfo = remainder_str.substr(dollarPos + 2);
        remainder_str = remainder_str.substr(0, dollarPos - 1);
    }
 
    eventInfo = remainder_str;
    processCompMainEvent(eventInfo, newEvent);
 
    if (dollarPos != string::npos)
        processCompWriteEvent(writeEventInfo, newEvent);
 
    if (starPos != string::npos)
        processCompReadEvent(readEventInfo, newEvent);

    if(newEvent->ThreadID > m_num_threads || newEvent->ThreadID < 0) {
        DPRINTF(amTrace, "ThreadIDBad\n");
    }  
    eventMap[newEvent->ThreadID]->push_back(newEvent);
}

void DragonTester::processPthreadEvent(string thisEvent, size_t caretPos) {
  string eventInfo = thisEvent.substr(0, caretPos - 1);
  DragonEvent *newEvent = new DragonEvent();
  newEvent->subEventList = NULL;
  newEvent->Type = TYPE_PTHREAD_API;

  string tmpStr;
  size_t curPos = -1, nextPos = -1;

  for (int i = 0; i < PTHREAD_ENTRIES; i++) {
    nextPos = eventInfo.find(',', curPos + 1);
    tmpStr = eventInfo.substr(curPos + 1, nextPos - (curPos + 1));  
    curPos = nextPos;

    switch (i) {
      case 0:
        newEvent->EventID = strtoul(tmpStr.c_str(), NULL, 0);
        break;
      case 1:
        newEvent->ThreadID = strtoul(tmpStr.c_str(), NULL, 0) - 1;
        break;
      case 2:
        assert(nextPos == string::npos);
        nextPos = tmpStr.find(':');
        assert(tmpStr.substr(0, nextPos) == PTHREAD_TAG);
        newEvent->PthreadType =
            strtol(tmpStr.substr(nextPos + 1, string::npos).c_str(), NULL, 0);
        break;
      default:
        assert(false);
    }
  }

  newEvent->pth_addr = 
      strtoul(thisEvent.substr(caretPos + 2, string::npos).c_str(), NULL, 0);
// Paco - Address from sigil pthread.out are now used instead of pth_addr--
//  if (newEvent->Type == P_CREATE || newEvent->Type == P_JOIN)
//    newEvent->pth_addr--;

  if(newEvent->ThreadID > m_num_threads || newEvent->ThreadID < 0) {
      DPRINTF(amTrace, "ThreadIDBad\n");
  }
  eventMap[newEvent->ThreadID]->push_back(newEvent);
}

// Vasil - processing address to ID

void DragonTester::processAddressToID(string line, size_t hashPos){

  string eventfulVasil = line.substr(hashPos+1);
  size_t commaPos = eventfulVasil.find(',');
// Paco - Fixed threadAddressString commaPos-1 to commaPos
  string threadAddressString = eventfulVasil.substr(0, commaPos);
  string threadIdString = eventfulVasil.substr(commaPos+1);
// Paco - Changed threadId uint64_t to int REMINDER --decrement threadId
  uint64_t threadAddress = strtoul(threadAddressString.c_str(), NULL, 0);
  int threadId = strtol(threadIdString.c_str(), NULL, 0);

// Paco - Changed threadId to threadId -1. Maps sigil.pthread.out thread#(1-21) to (0-20)
  AddresstoIdMap[threadAddress] = threadId-1;
}

// Vasil - processing address to ID

// Vasil - processing barrier events

void DragonTester::processBarrierEvent(string line, size_t starPos){

  string eventfulVasil = line.substr(starPos+1);
  size_t commaPos = eventfulVasil.find(',');
  size_t oldcomma=0;
// Paco - Fixed barrierAddressString commaPos-1 to commaPos
  string barrierAddressString = eventfulVasil.substr(0, commaPos);
  string threadIdList = eventfulVasil.substr(commaPos+1);

  commaPos = threadIdList.find(',');

  set<int> threadIds, threadWaitIds;

  while (commaPos != string::npos) {
// Paco - Fixed threadId commaPos-1 to commaPos
    int threadId = strtol(threadIdList.substr(oldcomma, commaPos).c_str(),NULL,0);
    threadIds.insert(threadId-1); // Paco (11/5) - Changed threadId to threadId-1
    oldcomma=commaPos+1;
    commaPos = threadIdList.find(',', commaPos+1);
  }

  uint64_t barrierAddress = strtoul(barrierAddressString.c_str(), NULL, 0);
  BarrierMap[barrierAddress] = threadIds;
  threadWaitMap[barrierAddress] = threadWaitIds;
}

// Vasil - processing barrier events




void DragonTester::readEventFile(int threadID)
{
 
//    DPRINTF(amTrace, "Reading event file for thread number %d\n", threadID);
    string thisEvent;
 
    for (int count = 0; count < READ_BLOCK; count++) {
        if (getline(*(inputFilePointer[threadID]), thisEvent)) {
            size_t hashPos = thisEvent.find('#');
            size_t caretPos = thisEvent.find('^');
 
            if (hashPos != string::npos)
                processCommEvent(thisEvent, hashPos);
            else if (caretPos != string::npos)
                processPthreadEvent(thisEvent, caretPos);
            else
                processCompEvent(thisEvent); 
        }
        else break;
    }
}

void DragonTester::triggerCommReadMsg(const int procID, const int threadID, subEvent* thisSubEvent)
{
    assert (!eventMap[threadID]->empty());
 
    Address addr;
    addr.setAddress((physical_address_t)thisSubEvent->thisMsg->addr);
 
    Request::Flags flags;
 
    // For simplicity, requests are assumed to be 1 byte-sized
    Request *req = new Request(addr.getAddress(), thisSubEvent->thisMsg->numBytes, flags, _masterId);
    req->setThreadContext(procID,0);
 
    Packet::Command cmd;
    cmd = MemCmd::ReadReq;
    PacketPtr pkt = new Packet(req, cmd);
    uint8_t* dummyData = new uint8_t;
    *dummyData = 0;
    pkt->dataDynamic(dummyData);
 
//    DPRINTF(amTrace, "Trying to access Addr 0x%x\n", pkt->getAddr()); 
    if (ports[procID]->sendTimingReq(pkt)) {
        thisSubEvent->msgTriggered = true;
        //DPRINTF(amTrace, "%d: Communication Read Message Triggered: Core %d; Thread %d; Event %d; Subevents size %d; Addr 0x%x\n", g_eventQueue_ptr->getTime(), procID, threadID, eventMap[threadID]->front()->EventID, eventMap[threadID]->front()->subEventList->size(), addr);
 
        // increment the respective count when the message is triggered
        threadStatistics[threadID].sharedReads++;
 
        lastMsgTriggerTime[threadID] = g_eventQueue_ptr->getTime();
    } else {
        // If the packet did not issue, must delete
        // Note: No need to delete the data, the packet destructor
        // will delete it
        delete pkt->req;
        delete pkt;
    }
}

void DragonTester::triggerCompWriteMsg(const int procID, const int threadID, subEvent* thisSubEvent)
{
    assert (!eventMap[threadID]->empty());
 
    Address addr;
    addr.setAddress((physical_address_t)thisSubEvent->thisMsg->addr);
 
    Request::Flags flags;
 
    // For simplicity, requests are assumed to be 1 byte-sized
    Request *req = new Request(addr.getAddress(), thisSubEvent->thisMsg->numBytes, flags, _masterId);
    req->setThreadContext(procID,0);
 
    Packet::Command cmd;
    cmd = MemCmd::WriteReq;
    PacketPtr pkt = new Packet(req, cmd);
    uint8_t* dummyData = new uint8_t;
    *dummyData = 0;
    pkt->dataDynamic(dummyData);
 
   // DPRINTF(amTrace, "Trying to access Addr 0x%x\n", pkt->getAddr());
    if (ports[procID]->sendTimingReq(pkt)) {
        thisSubEvent->msgTriggered = true;
 
        //DPRINTF(amTrace, "%d: Computation Write Message Triggered: Core %d; Thread %d; Event %d; Subevents size %d; Addr 0x%x\n", g_eventQueue_ptr->getTime(), procID, threadID, eventMap[threadID]->front()->EventID, eventMap[threadID]->front()->subEventList->size(), addr);
 
        // increment the respective count whent the message is triggered
        threadStatistics[threadID].localWrites++;
 
        lastMsgTriggerTime[threadID] = g_eventQueue_ptr->getTime();
    } else {
        // If the packet did not issue, must delete
        // Note: No need to delete the data, the packet destructor
        // will delete it
        delete pkt->req;
        delete pkt;
    }
}

void DragonTester::triggerCompReadMsg(const int procID, const int threadID, subEvent* thisSubEvent)
{
    assert (!eventMap[threadID]->empty());
 
    Address addr;
    addr.setAddress((physical_address_t)thisSubEvent->thisMsg->addr);
 
    Request::Flags flags;
 
    // For simplicity, requests are assumed to be 1 byte-sized
    Request *req = new Request(addr.getAddress(), thisSubEvent->thisMsg->numBytes, flags, _masterId);
    req->setThreadContext(procID,0);
 
    Packet::Command cmd;
    cmd = MemCmd::ReadReq;
    PacketPtr pkt = new Packet(req, cmd);
    uint8_t* dummyData = new uint8_t;
    *dummyData = 0;
    pkt->dataDynamic(dummyData);
 
//    DPRINTF(amTrace, "Trying to access Addr 0x%x\n", pkt->getAddr()); 
    if (ports[procID]->sendTimingReq(pkt)) {
        thisSubEvent->msgTriggered = true;
 
        //DPRINTF(amTrace, "%d: Computation Read Message Triggered: Core %d; Thread %d; Event %d; Subevents size %d; Addr 0x%x\n", g_eventQueue_ptr->getTime(), procID, threadID, eventMap[threadID]->front()->EventID, eventMap[threadID]->front()->subEventList->size(), addr);
 
        // increment the respective count whent the message is triggered
        threadStatistics[threadID].localReads++;
 
        lastMsgTriggerTime[threadID] = g_eventQueue_ptr->getTime();
    } else {
        // If the packet did not issue, must delete
        // Note: No need to delete the data, the packet destructor 
        // will delete it
        delete pkt->req;
        delete pkt;
    }
}

void DragonTester::progressEvents(int procID)
{
    int eventThreadID = threadMap[procID].front();
    if (!threadStartedMap[eventThreadID]) return;
 
    // if all the events are completed then skip
    if (eventMap[eventThreadID]->empty())
        return;
 
    DragonEvent *thisEvent = eventMap[eventThreadID]->front();
 
    subEvent* topSubEvent = &(thisEvent->subEventList->front());
 
    if(topSubEvent->triggerTime <= g_eventQueue_ptr->getTime() && !topSubEvent->msgTriggered) {     //if message has been triggered then hitCallback will advance things forward
        threadStatistics[eventThreadID].numIOPS += topSubEvent->numIOPS;
        threadStatistics[eventThreadID].numFLOPS += topSubEvent->numFLOPS;
 
        if (!topSubEvent->containsMsg) {    // if the subevent does not contain any messages then its finished and we can pop it from the list
 
          if (thisEvent->Type != TYPE_PTHREAD_API) {
            threadStatistics[eventThreadID].numSubEvents++;
            thisEvent->subEventList->pop_front();
            m_last_progress_vector[procID] = g_eventQueue_ptr->getTime();
          }
          else handlePthreadEvent(thisEvent, procID);

            // then check if all subevents are done-- if yes then the event has finished and we can pop it from the events list
            if (thisEvent->subEventList->empty()) {
                printEvent(eventThreadID, true); //Paco (8/14) - Event was just completed -> Print Event # on Thread #
         //Cycles Per 1k Events per thread:
                if ((thisEvent->EventID % 1000) == 0){
                    DPRINTF(roi,"%d,%1.2f\n",thisEvent->ThreadID,(g_eventQueue_ptr->getTime() - g_system_ptr->thread_cycles_per_event[thisEvent->ThreadID])/1000.0);
                    g_system_ptr->thread_cycles_per_event[thisEvent->ThreadID] = g_eventQueue_ptr->getTime();
                }

               delete eventMap[eventThreadID]->pop_front();

                swapThreads(procID);
                if(!eventMap[eventThreadID]->empty()) {
                //    DPRINTF(amTrace, "Testing thisEvent->ThreadID:%d\n",thisEvent->ThreadID);
                //    if(thisEvent->ThreadID >m_num_threads || thisEvent->ThreadID < 0) {
                //        DPRINTF(amTrace, "ThreadIDBad\n");
                    thisEvent = eventMap[eventThreadID]->front();
                    printEvent(eventThreadID, false); //Paco (8/14) - Event is just starting -> Print Event # on Thread #
   	            replenishEvents(thisEvent->ThreadID);
                }
                schedule(*(coreEvents[procID]), g_eventQueue_ptr->getTime() + 1);
//              DPRINTF(amTrace, "Core %d scheduled for %d\n", procID, g_eventQueue_ptr->getTime() + 1);
//              DPRINTF(amTrace, "Event %d completed for thread %d on core %d \n", eventMap[eventThreadID]->front()->EventID, eventThreadID, procID);

            } else {        // if not pull up the next subevent and schedule based on its trigger time
                subEvent* newSubEvent = &(thisEvent->subEventList->front());
                newSubEvent->triggerTime = CPI_IOPS * newSubEvent->numIOPS + CPI_FLOPS * newSubEvent->numFLOPS + MEM_ACCESS_TIME * newSubEvent->numMEM_OPS + g_eventQueue_ptr->getTime() + 1;
                newSubEvent->initialTriggerTime = newSubEvent->triggerTime;
                schedule(*(coreEvents[procID]), newSubEvent->triggerTime);
//              DPRINTF(amTrace, "Core %d scheduled for %d\n", procID, newSubEvent->triggerTime);
            }
        } else {
            if (thisEvent->Type == TYPE_COMMUNICATION) {    //if its a communication event
                if(checkCommDependency(topSubEvent->thisMsg, eventThreadID)) {    //check if dependecies have been met if yes trigger the msg if not reschedule wakeup for next cycle              
                    triggerCommReadMsg(procID, eventThreadID, topSubEvent);
                } else {
                    topSubEvent->triggerTime = g_eventQueue_ptr->getTime() + 1;
                    schedule(*(coreEvents[procID]), g_eventQueue_ptr->getTime() + 1);
//                  DPRINTF(amTrace, "Core %d scheduled for %d\n", procID, g_eventQueue_ptr->getTime() + 1);
                    m_last_progress_vector[procID] = g_eventQueue_ptr->getTime();
                }
            } else {        //if its not a communication event
                if (topSubEvent->msgType == DT_REQ_WRITE) {
                    triggerCompWriteMsg(procID, eventThreadID, topSubEvent);
                } else {
                    triggerCompReadMsg(procID, eventThreadID, topSubEvent);
                }
            }
        }
    } else {
        if (!(coreEvents[procID]->scheduled())) {
            schedule(*(coreEvents[procID]), topSubEvent->triggerTime);
        }
    }
}

void DragonTester::handlePthreadEvent(DragonEvent *thisEvent, int procID) {
  assert(thisEvent);
  bool consumedEvent = false;
  set<uint64_t>::iterator mutexIttr, spinIttr;
//  set<int> threadWaitMapSet;
//  set<int> BarrierMapSet;
//  set<int> differenceSet

  switch (thisEvent->PthreadType) {
    case P_MUTEX_LK:
      if (mutexLocks.find(thisEvent->pth_addr) == mutexLocks.end()) {
        mutexLocks.insert(thisEvent->pth_addr);
        threadMutexMap[thisEvent->ThreadID] = true; //Paco (11/6) - Thread holding mutex lock status.
        DPRINTF(mutexLogger,"Thread %d locked mutex %d\n", thisEvent->ThreadID, thisEvent->pth_addr); //Paco (8/14) - Log when mutex locks are received.
        consumedEvent = true;
      }
      break;

    case P_MUTEX_ULK:
      mutexIttr = mutexLocks.find(thisEvent->pth_addr);
      assert(mutexIttr != mutexLocks.end());
      mutexLocks.erase(mutexIttr);
      threadMutexMap[thisEvent->ThreadID] = false; //Paco (11/6) - Returned mutex lock status.
      DPRINTF(mutexLogger,"Thread %d unlocked mutex %d\n", thisEvent->ThreadID, thisEvent->pth_addr); //Paco (8/14) - Log when mutex unlocks are received.
      consumedEvent = true;
      break;
    
    case P_CREATE:
      if(!roiFlag){
          DPRINTF(roi,"Reached parallel region.\n");
          roiFlag = true;
      }

      worker_thread_count++;
      threadStartedMap[AddresstoIdMap[thisEvent->pth_addr]] = true;
      consumedEvent = true;
      //DPRINTF(amTrace, "Thread %d created \n", AddresstoIdMap[thisEvent->pth_addr]);
      break;
    
    case P_JOIN:
      //assert(threadStartedMap[AddresstoIdMap[thisEvent->pth_addr]]);
      if (eventMap[AddresstoIdMap[thisEvent->pth_addr]]->empty())
        consumedEvent = true;
        worker_thread_count--;
        if(worker_thread_count == 0) DPRINTF(roi,"Last Thread Joined.\n");
      break;
    
    case P_BARRIER_WT:
//Paco 11/12 - Fixing barriers
        if(threadContMap[thisEvent->ThreadID]== false) {

            //if not in waitmap, put in waitmap
            if (threadWaitMap[thisEvent->pth_addr].find(thisEvent->ThreadID)==threadWaitMap[thisEvent->pth_addr].end()) {
                threadWaitMap[thisEvent->pth_addr].insert(thisEvent->ThreadID);
                threadStartedMap[thisEvent->ThreadID] = false;
            }

            if(checkBarriers(thisEvent)) {
                set<int>::iterator barrIttr = BarrierMap[thisEvent->pth_addr].begin();
                for( ; barrIttr != BarrierMap[thisEvent->pth_addr].end(); barrIttr++) {
                    threadStartedMap[*barrIttr] = true;
                    threadContMap[*barrIttr] = true;
                }
                threadWaitMap[thisEvent->pth_addr].clear();
                g_system_ptr->printMemoryEventsInBarrier(); //Paco (8/20) Print out memory events after each barrier
            }
        } else {//if(threadContMap[thisEvent->ThreadID] == true){
            threadContMap[thisEvent->ThreadID] = false;
            consumedEvent = true;
        }

    break;
    
    case P_COND_WT:
      thisEvent->subEventList->pop_front();
      break;
    
    case P_COND_SG:
      thisEvent->subEventList->pop_front();
      break;
    
    case P_SPIN_LK:
      if (spinLocks.find(thisEvent->pth_addr) == spinLocks.end()) {
        spinLocks.insert(thisEvent->pth_addr);
        consumedEvent = true;
      }
      break;
    
    case P_SPIN_ULK:
      spinIttr = spinLocks.find(thisEvent->pth_addr);
      assert(spinIttr != spinLocks.end());
      spinLocks.erase(spinIttr);
      consumedEvent = true;
      break;
    
    case P_SEM_INIT:
      //break;
    case P_SEM_WAIT:
      //break;
    case P_SEM_POST:
      //break;
    case P_SEM_GETV:
      //break;
    case P_SEM_DEST:
      //break;
    default:
      assert(false && "Invalid pthread event.");
  }

  if (consumedEvent) {
    thisEvent->subEventList->pop_front();
    threadStatistics[thisEvent->ThreadID].numSubEvents++;
    m_last_progress_vector[procID] = g_eventQueue_ptr->getTime();
  }
}

bool DragonTester::checkBarriers(DragonEvent *thisEvent) {
     set<int> threadWaitMapSet;
     set<int> BarrierMapSet;
     set<int> differenceSet;

     threadWaitMapSet = threadWaitMap[thisEvent->pth_addr];
     BarrierMapSet = BarrierMap[thisEvent->pth_addr];
     set_difference(BarrierMapSet.begin(), BarrierMapSet.end(), threadWaitMapSet.begin(), threadWaitMapSet.end(), inserter(differenceSet,differenceSet.begin()));

     if(differenceSet.empty()){
          return 1;
     } else {
          return 0;
     }
}

int DragonTester::memTypeToInsert(const unsigned long loc_reads, const unsigned long loc_writes, const unsigned long max_loc_reads, const unsigned max_loc_writes, const int read_or_write)
{
    bool selected = false;
 
    int returnType;
    int selector = read_or_write; 
    while(!selected) {
        //int r_int = rand() % MEM_ACTION_TYPES; Paco (8/20) - Remove random selection of read/write. First write then read.
        switch(selector) {
            case MEM_LOCAL_READ:
            {
                if(loc_reads < max_loc_reads)  {
                    returnType = MEM_LOCAL_READ;
                    selected = true;
                }
                else {
                    selector = (selector == 1) ? 0 : 1; // Paco (8/20) - Flip selector if no reads left
                }
                break;
            }
            case MEM_LOCAL_WRITE:
            {
                if(loc_writes < max_loc_writes) {
                    returnType = MEM_LOCAL_WRITE;
                    selected = true;
                }
                else {
                    selector = (selector == 1) ? 0 : 1; // Paco (8/20) - Flip selector if no writes left
                }
                break;
            }
            default:
                assert(false);
        }
    }
    return returnType;
}

void DragonTester::createSubEvents(int procID, bool eventIDPassed, int eventThreadID)
{
    if(!eventIDPassed)
      eventThreadID = threadMap[procID].front();
    
    if (!threadStartedMap[eventThreadID]) return;
 
    // if there are no events just return
    if (eventMap[eventThreadID]->empty())
        return;
 
    DragonEvent *thisEvent = eventMap[eventThreadID]->front();
    if (!thisEvent->subEventsCreated) {
        if (thisEvent->Type == TYPE_PTHREAD_API) {
          thisEvent->subEventList = new StaticDeque<subEvent>(1);
          subEvent thisSubEvent(0,0,0,false,false);
          thisEvent->subEventList->push_back(thisSubEvent);
        }
        else if (thisEvent->Type == TYPE_COMMUNICATION) {

        //Paco (11/6) Bypass creating COMM subevents if thread holds mutex. Create dummy event so COMM Event will end at the next progressEvents function call.
            if (threadMutexMap[thisEvent->ThreadID] == true) { //If thread has mutex, create dummy subevent
                thisEvent->subEventList = new StaticDeque<subEvent>(1);
                subEvent thisSubEvent(0,0,0,false,false);
                thisEvent->subEventList->push_back(thisSubEvent);
            }
            else { //If thread does not have mutex, continue as normal with COMM subevent creation.
                thisEvent->subEventList = new StaticDeque<subEvent>(thisEvent->comm_preRequisiteEvents.size());
                for (unsigned long j=0; j < thisEvent->comm_preRequisiteEvents.size(); j++) {
                    subEvent thisSubEvent(0,0,0,DT_REQ_READ,false,true,thisEvent->comm_preRequisiteEvents[j]);
                    thisEvent->subEventList->push_back(thisSubEvent);
                }
            }
        } else {
	    unsigned long max_loc_reads;
	    unsigned long max_loc_writes;

	    if (thisEvent->compMem_reads >= thisEvent->comp_readEvents.size())
		max_loc_reads = thisEvent->compMem_reads;
	    else
		max_loc_reads = thisEvent->comp_readEvents.size();

            if (thisEvent->compMem_writes >= thisEvent->comp_writeEvents.size())
                max_loc_writes = thisEvent->compMem_writes;
            else
                max_loc_writes = thisEvent->comp_writeEvents.size();

            unsigned long totalMemOps = max_loc_reads + max_loc_writes;
            if (totalMemOps == 0) { // numMEM_OPS = 0 
                thisEvent->subEventList = new StaticDeque<subEvent>(1);
                subEvent thisSubEvent(thisEvent->compIOPS, thisEvent->compFLOPS, 0, false, false);
                thisEvent->subEventList->push_back(thisSubEvent);
            } else {
                unsigned long IOPS_div = thisEvent->compIOPS/totalMemOps;
                unsigned int IOPS_rem = thisEvent->compIOPS % totalMemOps;
                unsigned long FLOPS_div = thisEvent->compFLOPS/totalMemOps;
                unsigned int FLOPS_rem = thisEvent->compFLOPS % totalMemOps;
 
                // setting up the numMEM_OPS
                unsigned long MEM_div = 0, MEM_rem = 0;
 
                unsigned long mem_reads_inserted = 0;
                unsigned long mem_writes_inserted = 0;
 
                // Mark off memory accesses and distribute the IOPS and FLOPS evenly
                thisEvent->subEventList = new StaticDeque<subEvent>(totalMemOps);
		int read_or_write = 1; //Removing randomness. Write first then read
                for (unsigned long j = 0; j < totalMemOps; j++) {
                    switch(memTypeToInsert(mem_reads_inserted, mem_writes_inserted, max_loc_reads, max_loc_writes,read_or_write)) {
                        case MEM_LOCAL_READ:
                            {
                                subEvent thisSubEvent(IOPS_div,FLOPS_div,MEM_div,DT_REQ_READ,false,true,thisEvent->comp_readEvents[mem_reads_inserted % thisEvent->comp_readEvents.size()]);
                                thisEvent->subEventList->push_back(thisSubEvent);
                                mem_reads_inserted++;
                                break;
                            }
                        case MEM_LOCAL_WRITE:
                            {
                                subEvent thisSubEvent(IOPS_div,FLOPS_div,MEM_div,DT_REQ_WRITE,false,true,thisEvent->comp_writeEvents[mem_writes_inserted % thisEvent->comp_writeEvents.size()]);
//                              subEvent thisSubEvent(IOPS_div,FLOPS_div,DT_REQ_ATOMIC,false,true,false);
                                thisEvent->subEventList->push_back(thisSubEvent);
                                mem_writes_inserted++;
                                break;
                            }
                        default:
                                assert(false);
                    }
                    read_or_write = (read_or_write == 1) ? 0 : 1; // Paco (8/20) - Flip read/write
                }
 
                // distribute the others randomly
                for (unsigned int j = 0; j < IOPS_rem; j++) {
                    (*(thisEvent->subEventList))[rand()%totalMemOps].numIOPS++;
                }
 
                for (unsigned int j = 0; j < FLOPS_rem; j++) {
                    (*(thisEvent->subEventList))[rand()%totalMemOps].numFLOPS++;
                }
 
                for (unsigned int j = 0; j < MEM_rem; j++) {
                    (*(thisEvent->subEventList))[rand()%totalMemOps].numMEM_OPS++;
                }
            }
        }
        subEvent* topSubEvent = &(thisEvent->subEventList->front());
        topSubEvent->triggerTime = CPI_IOPS * topSubEvent->numIOPS + CPI_FLOPS * topSubEvent->numFLOPS + MEM_ACCESS_TIME * topSubEvent->numMEM_OPS + g_eventQueue_ptr->getTime() + 1;
        topSubEvent->initialTriggerTime = topSubEvent->triggerTime;
        if (!(coreEvents[procID]->scheduled())) {
            schedule(*(coreEvents[procID]), topSubEvent->triggerTime);
        }
//      DPRINTF(amTrace, "Core %d scheduled for %d\n", procID, topSubEvent->triggerTime);
 
        thisEvent->subEventsCreated = true;
    }
}

