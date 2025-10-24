//utility classes for various Win32 locks (SRWLock/CriticalSection/custom spin locks)
//Copyright (C) 2023 Zoltan Bacsko - Falcosoft

//#define DEBUG_LOCKS

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>

#ifdef DEBUG_LOCKS
#include <iostream>
#endif

#define WIN32DEF(f) (WINAPI *f)

//Wrapper class for Win32 SRWLock on Win Vista and later else for standard Win32 CriticalSection 
//Be aware that SRWLock does not support re-entrance. If you need it use alwaysUseCriticalSection in constructor.  

#pragma warning(disable:28125) 
class Win32Lock {
	
	typedef struct _MYLOCK {
		PVOID Ptr;
	} MYLOCK, * PMYLOCK;

private:
	bool useSRWLock;
	MYLOCK _lock;
		
	void WIN32DEF(AcquireSRWLockExclusive)(PMYLOCK Lock);
	void WIN32DEF(ReleaseSRWLockExclusive)(PMYLOCK Lock);

public:
	explicit Win32Lock(bool alwaysUseCriticalSection = false) :
		_lock(/* Initializing to 0 means the same as SRWLOCK_INIT or using InitializeSRWLock() */) {
		AcquireSRWLockExclusive = ReleaseSRWLockExclusive = NULL;
		
		if (!alwaysUseCriticalSection) {		
			HMODULE krnl32;
			if ((krnl32 = GetModuleHandle(_T("kernel32.dll"))) != NULL) {
				*((void**)&AcquireSRWLockExclusive) = GetProcAddress(krnl32, "AcquireSRWLockExclusive");
				*((void**)&ReleaseSRWLockExclusive) = GetProcAddress(krnl32, "ReleaseSRWLockExclusive");
			}
		}

		useSRWLock = (AcquireSRWLockExclusive && ReleaseSRWLockExclusive);

		if (!useSRWLock) {
			_lock.Ptr = new CRITICAL_SECTION(); //only allocate CRITICAL_SECTION dynamically if needed.
			InitializeCriticalSection((LPCRITICAL_SECTION)_lock.Ptr);
		}
	}

	~Win32Lock() {
		if (!useSRWLock) {
			DeleteCriticalSection((LPCRITICAL_SECTION)_lock.Ptr);
			delete (LPCRITICAL_SECTION)_lock.Ptr;
		}
	}

	inline void lock() {
		if (useSRWLock)
			AcquireSRWLockExclusive(&_lock);
		else
			EnterCriticalSection((LPCRITICAL_SECTION)_lock.Ptr);
	}

	inline void unlock() {
		if (useSRWLock)
			ReleaseSRWLockExclusive(&_lock);
		else
			LeaveCriticalSection((LPCRITICAL_SECTION)_lock.Ptr);
	}
};


#define MAX_SPIN 0xFFF //MAX_SPIN must be pow(2, x) - 1 

//Experimental TTAS spinlock with re-entrance support.
class FastLock {
private:
	volatile long threadId;
	volatile unsigned int lockCount;
	bool only1CoreAvailable;

public:	
	//Even on multiprocessor systems affinity can be set to only 1 processor. Spinning makes no sense in this case either.
	static bool GetOnly1CoreAvailable()	{
		DWORD_PTR procMask, sysMask;
		GetProcessAffinityMask(GetCurrentProcess(), &procMask, &sysMask);
		return (procMask & (procMask - 1)) == 0;
	}

	void SetOnly1CoreAvailable(bool value) {
		only1CoreAvailable = value;
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
				else if (loops & MAX_SPIN) { //try to get lock MAX_SPIN times
					YieldProcessor();					
				}
				else { //sleep if lock is not successful after trying MAX_SPIN times
#ifdef DEBUG_LOCKS					
					std::cout << "Sleep: " << loops << "\n";
#endif
					Sleep(1); 
				}
			}
		}		
	}

	inline void unlock() {
		_WriteBarrier();
		lockCount--;
		if (!lockCount) threadId = 0;
	}
};

//Experimental TTAS spinlock without re-entrance support. 
class FasterLock {
private:
	volatile long lockCount;
	bool only1CoreAvailable;

public:
	//Even on multiprocessor systems affinity can be set to only 1 processor. Spinning makes no sense in this case either.
	static bool GetOnly1CoreAvailable()	{
		DWORD_PTR procMask, sysMask;
		GetProcessAffinityMask(GetCurrentProcess(), &procMask, &sysMask);
		return (procMask & (procMask - 1)) == 0;
	}

	void SetOnly1CoreAvailable(bool value) {
		only1CoreAvailable = value;
	}

	FasterLock() {
		lockCount = 0;
		only1CoreAvailable = GetOnly1CoreAvailable();
	}

	void lock() {		

		while (true) {
			if (!_InterlockedExchange(&lockCount, 1)) {				
				return;
			}

			unsigned int loops = 0;
			while (lockCount) {
				loops++;
				if (only1CoreAvailable) { // in case of 1 available core there is no sense in spinning
					SwitchToThread();
				}
				else if (loops & MAX_SPIN) { //try to get lock MAX_SPIN times
					YieldProcessor();
				}
				else { //sleep if lock is not successful after trying MAX_SPIN times
#ifdef DEBUG_LOCKS					
					std::cout << "Sleep: " << loops << "\n";
#endif
					Sleep(1);
				}
			}
		}		
	}

	inline void unlock() {
		_WriteBarrier();
		lockCount = 0;
	}
};

template <class T>
class ScopeLock {
private:
	T* _lock;
public:
	inline explicit ScopeLock(T* lockObj) {
		_lock = lockObj;
		_lock->lock();
	}

	inline ~ScopeLock() {
		_lock->unlock();
	}
};
