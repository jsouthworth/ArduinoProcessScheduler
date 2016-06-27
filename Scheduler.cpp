#include "Scheduler.h"
#include "Service.h"


Scheduler::Scheduler()
{
    _head = NULL;
    _lastID = 0;
}

SchedulerAction Scheduler::add(Service &service)
{
    // Lock changes to linked list
    if (!_locked)
        _locked = true;
    else
        return ACTION_NONE;

    // Lock changes to service
    if (!service.locked()) {
        service.lock();

        // Make sure it is not added twice!
        if (!findNode(service) && !isRunningService(service))
        {
            // Empty flag queue
            uint8_t tmp;
            while (service.getFlagQueue()->pull(service.getFlagQueue(), &tmp));
            service.setID(++_lastID);
            appendNode(service);
            service.setup();
            processFlags(service, true);

#ifdef _SERVICE_STATISTICS
            service.setHistRuntime(0);
            service.setHistIterations(0);
            service.setHistLoadPercent(0);
#endif

            _locked = false;
            service.unlock();
            return ACTION_SUCCESS;
        }

    }

    _locked = false;
    return ACTION_NONE;

}


uint32_t Scheduler::getCurrTS()
{
    return TIMESTAMP();
}

Service *Scheduler::getCurrService()
{
    return _active;
}

bool Scheduler::isRunningService(Service &service)
{
    return _active && &service == _active;
}


bool Scheduler::isNotDestroyed(Service &service)
{
    return findNode(service);
}

bool Scheduler::isEnabled(Service &service)
{
    return service.isEnabled();
}

SchedulerAction Scheduler::disable(Service &service)
{
    SchedulerAction ret = ACTION_NONE;
    if (!service.locked()) { // If able to get lock, lock it
        service.lock();
        if (service.isEnabled() && isNotDestroyed(service)) {
            setDisable(service);
            ret = ACTION_SUCCESS;
        }
        service.unlock();
    } else { // If unable to get lock
        uint8_t flag = Service::FLAG_DISABLE;
        if (service.getFlagQueue()->add(service.getFlagQueue(), &flag) >= 0)
            ret = ACTION_QUEUED;
    }
    return ret;
}


SchedulerAction Scheduler::enable(Service &service)
{
    SchedulerAction ret = ACTION_NONE;
    if (!service.locked()) { // If able to get lock, lock it
        service.lock();
        if (!service.isEnabled() && isNotDestroyed(service)) {
            setEnable(service);
            ret = ACTION_SUCCESS;
        }
        service.unlock();
    } else { // If unable to get lock
        uint8_t flag = Service::FLAG_ENABLE;
        if (service.getFlagQueue()->add(service.getFlagQueue(), &flag) >= 0)
            ret = ACTION_QUEUED;
    }
    return ret;
}


SchedulerAction Scheduler::destroy(Service &service)
{
    bool s_lock = false, l_lock = false;
    SchedulerAction ret = ACTION_QUEUED;

    if (!service.locked()) {
        service.lock();
        s_lock = true;
    }

    if (!_locked) {
        _locked = true;
        l_lock = true;
    }
    // Both have been locked successfully
    if (s_lock && l_lock && isNotDestroyed(service)) {
        setDestroy(service);
        ret = ACTION_SUCCESS;
    } else if (isNotDestroyed(service)) {
        uint8_t flag = Service::FLAG_DESTROY;
        ret = service.getFlagQueue()->add(service.getFlagQueue(), &flag) >= 0 ?
            ACTION_QUEUED : ACTION_NONE;
    }

    // now unlock everything
    if (s_lock)
        service.unlock();

    if (l_lock)
        _locked = false;

    return ret;
}

uint8_t Scheduler::getID(Service &service)
{
    return service.getID();
}

uint8_t Scheduler::countServices(bool enabledOnly)
{
    if (!_locked)
        _locked = true;
    else
        return 0;

    uint8_t count=0;
    for (Service *curr = _head; curr != NULL; curr = curr->getNext())
    {
        count += enabledOnly ? curr->isEnabled() : 1;
    }

    _locked = false; // restore
    return count;
}

int Scheduler::run()
{
    // Nothing to run or already running in another call frame
    if (!_head || _active) return 0;
    int count = 0;
    for (_active = _head; _active != NULL ; _active = _active->getNext(), count++)
    {
        _active->lock(); // Lock changes to it!
        processFlags(*_active);
        uint32_t start = getCurrTS();

        if (_active->needsServicing(start))
        {
            bool force = _active->forceSet(); // Store whether it was a forced iteraiton
            _active->willService(start);

            // Handle scheduler warning
            if (_active->getOverSchedThresh() != OVERSCHEDULED_NO_WARNING && _active->isPBehind(start)) {
                _active->incrPBehind();
                if (_active->getCurrPBehind() >= _active->getOverSchedThresh())
                    _active->overScheduledHandler(start - _active->getScheduledTS());
            } else {
                _active->resetSchedulerWarning();
            }

#ifdef _SERVICE_EXCEPTION_HANDLING
            int ret = setjmp(_env);
            if (!ret) {
                _active->service();
            } else {
                eDispatcher(ret);
            }
#else
            _active->service();
#endif


#ifdef _SERVICE_STATISTICS
            uint32_t runTime = getCurrTS() - start;
            // Make sure no overflow happens
            if (_active->statsWillOverflow(1, runTime))
                handleHistOverFlow(HISTORY_DIV_FACTOR);

            _active->setHistIterations(_active->getHistIterations()+1);
            _active->setHistRuntime(_active->getHistRunTime()+runTime);

#endif
            // Is it time to disable?
            if (_active->wasServiced(force)) {
                processFlags(*_active);
                setDisable(*_active);
            }
        }
        processFlags(*_active);

        _active->unlock();
    }

    _active = NULL;
    delay(0); // For future ESP8266 support
    return count;
}

/************ PROTECTED ***************/
// THESE SHOULD ONLY BE CALLED WHEN SERVICE IS LOCKED BY CALLER
void Scheduler::setDisable(Service &service)
{
    service.onDisable();
    service._enabled = false;
}


void Scheduler::setEnable(Service &service)
{
    service.onEnable();
    service._enabled = true;
}


void Scheduler::setDestroy(Service &service)
{
    setDisable(service);
    service.cleanup();
    removeNode(service);
}



//only call when service is locked by caller! callerLock = true is linked list is locked by caller
void Scheduler::processFlags(Service &service, bool callerLock)
{
    if (!_locked)
        _locked = true;
    else if (!callerLock)
        return;
    // Process flags
    RingBuf *queue = service.getFlagQueue();

    uint8_t flag;
    while(!queue->isEmpty(queue)) // Empty Queue
    {
        queue->pull(queue, &flag);
        switch (flag)
        {
            case Service::FLAG_ENABLE:
                if (!service.isEnabled() && isNotDestroyed(service))
                    setEnable(service);
                break;

            case Service::FLAG_DISABLE:
                if (service.isEnabled() && isNotDestroyed(service))
                    setDisable(service);
                break;

            case Service::FLAG_DESTROY:
                if (isNotDestroyed(service))
                {
                    setDestroy(service);
                    while(queue->pull(queue, &flag)); // Empty Queue
                }
                break;

            default:
                break;
        }
    }
    _locked = false;

}

#ifdef _SERVICE_STATISTICS

void Scheduler::updateStats()
{
    if (_locked)
        return;
    uint8_t count = countServices(false);
    HISTORY_TIME_TYPE sTime[count];

    // Thread safe in case of interrupts
    HISTORY_TIME_TYPE totalTime;
    uint8_t i;
    Service *n;
    for(n = _head, i=0, totalTime=0; n != NULL && i < count; n = n->getNext(), i++)
    {
        // to ensure no overflows
        sTime[i] = n->getHistRunTime() / count;
        totalTime += sTime[i];
    }

    for(i=0, n = _head; n != NULL && i < count; n = n->getNext(), i++)
    {
        // to ensure no overflows have to use double
        if (!totalTime) {
            n->setHistLoadPercent(0);
        } else {
            double tmp = 100*((double)sTime[i]/(double)totalTime);
            n->setHistLoadPercent((uint8_t)tmp);
        }
    }

}

void Scheduler::handleHistOverFlow(uint8_t div)
{
    for(Service *n = _head; n != NULL; n = n->getNext())
        n->divStats(div);
}

#endif


#ifdef _SERVICE_EXCEPTION_HANDLING
    void Scheduler::raiseException(int e)
    {
        longjmp(_env, e);
    }


    bool Scheduler::eDispatcher(int e)
    {
        if (e != 0 && _active)
        {
            if (!_active->handleException(e))
                handleException(*_active, e);
            return true;
        }
        return false;

    }
#endif



bool Scheduler::appendNode(Service &node)
{
    node.setNext(NULL);

    if (!_head) {
        _head = &node;
    } else {
        Service *next = _head;
        for(; next->hasNext(); next = next->getNext()); //run through list
        // Update pointers
        next->setNext(&node);
    }
    return true;
}

bool Scheduler::removeNode(Service &node)
{
    if (&node == _head) { // node is head
        _head = node.getNext();
    } else {
        // Find the previous node
        Service *prev = _head;
        for (; prev != NULL && prev->getNext() != &node; prev = prev->getNext());

        if (!prev) return false; // previous node does not exist
        prev->setNext(node.getNext());
    }
    return true;
}


bool Scheduler::findNode(Service &node)
{
    Service *prev = _head;
    for (; prev != NULL && prev != &node; prev = prev->getNext());

    return prev;
}