//falco: utility classes for win32 critical sections

#include <intrin.h>

class Win32Lock {
private:
	CRITICAL_SECTION cCritSec;

public:
	Win32Lock() {
		InitializeCriticalSection(&cCritSec);
	}

	~Win32Lock() {
		DeleteCriticalSection(&cCritSec);
	}

	inline void lock() {
		EnterCriticalSection(&cCritSec);
	}

	inline void unlock() {
		LeaveCriticalSection(&cCritSec);
	}
};

//Experimental TTAS spinlock with re-entrance support. For no/low contention cases.
class FastLock
{
private:
	volatile long threadId;
	volatile unsigned int lockCount;
	bool only1CoreAvailable;

public:
	static bool GetOnly1CoreAvailable()
	{
		DWORD_PTR procMask, sysMask;
		GetProcessAffinityMask(GetCurrentProcess(), &procMask, &sysMask);
		return (procMask & (procMask - 1)) == 0;
	}

	FastLock() {
		threadId = 0;
		lockCount = 0;
		only1CoreAvailable = GetOnly1CoreAvailable();
	}

	void lock() {
		long tmpId = (long)GetCurrentThreadId();
		if (tmpId == threadId) {
			lockCount++;
			return;
		}

		while (true) {
			if (!_InterlockedExchange(&threadId, tmpId)) {
				lockCount++;
				return;
			}

			unsigned int loops = 0;
			while (threadId) {
				loops++;
				if (only1CoreAvailable) { // in case of 1 available core there is no sense in spinning
					SwitchToThread();
				}
				else if (loops & 0x3FF) { //try to get lock 1024 times
					YieldProcessor();
				}
				else { // sleep if lock is not successful
					Sleep(1); 
				}
			}
		}
		lockCount++;
	}

	inline void unlock() {
		lockCount--;
		if (!lockCount) threadId = 0;
	}
};

template <class T>
class ScopeLock
{
private:
	T* _lock;
public:
	inline ScopeLock(T* lockObj) {
		_lock = lockObj;
		_lock->lock();
	}

	inline ~ScopeLock() {
		_lock->unlock();
	}
};
