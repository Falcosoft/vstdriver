//falco: utility classes for various Win32 locks (SRWLock/CriticalSection/custom spin locks)

//#define DEBUG_FASTLOCKS


#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>

#ifdef DEBUG_FASTLOCKS
#include <iostream>
#endif

#define WIN32DEF(f) (WINAPI *f)

//Wrapper class for Win32 SRWLock on Win Vista and later else for standard Win32 CriticalSection 
//Be aware that SRWLock does not support re-entrance. If you need it use alwaysUseCriticalSection in constructor.  
class Win32Lock {
	
	typedef struct _MYSRWLOCK {
		PVOID Ptr;
	} MYSRWLOCK, * PMYSRWLOCK;

private:
	bool useSRWLock;

	CRITICAL_SECTION critSec;	
	MYSRWLOCK srw;	

	void WIN32DEF(InitializeSRWLock)(PMYSRWLOCK SRWLock);
	void WIN32DEF(AcquireSRWLockExclusive)(PMYSRWLOCK SRWLock);
	void WIN32DEF(ReleaseSRWLockExclusive)(PMYSRWLOCK SRWLock);

public:
	Win32Lock(bool alwaysUseCriticalSection = false) {
		InitializeSRWLock = AcquireSRWLockExclusive = ReleaseSRWLockExclusive = NULL;		
		
		if (!alwaysUseCriticalSection) {
			HMODULE kernel32 = GetModuleHandle(_T("kernel32.dll"));
			if (kernel32) {
				*((void**)&InitializeSRWLock) = GetProcAddress(kernel32, "InitializeSRWLock");
				*((void**)&AcquireSRWLockExclusive) = GetProcAddress(kernel32, "AcquireSRWLockExclusive");
				*((void**)&ReleaseSRWLockExclusive) = GetProcAddress(kernel32, "ReleaseSRWLockExclusive");
			}
		}

		useSRWLock = (InitializeSRWLock && AcquireSRWLockExclusive && ReleaseSRWLockExclusive);

		if (useSRWLock)	{
			InitializeSRWLock(&srw);
		}
		else {			
			InitializeCriticalSection(&critSec);
		}
	}

	~Win32Lock() {
		if (!useSRWLock) {
			DeleteCriticalSection(&critSec);			
		}
	}

	inline void lock() {
		if (useSRWLock)
			AcquireSRWLockExclusive(&srw);
		else
			EnterCriticalSection(&critSec);
	}

	inline void unlock() {
		if (useSRWLock)
			ReleaseSRWLockExclusive(&srw);
		else
			LeaveCriticalSection(&critSec);
	}
};


#define MAX_SPIN 0xFF //MAX_SPIN must be pow(2, x) - 1 

//Experimental TTAS spinlock with re-entrance support.
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

	void SetOnly1CoreAvailable(bool value) 
	{
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
#ifdef DEBUG_FASTLOCKS
					std::cout << "SwitchToThread: " << loops << "\n";
#endif
					SwitchToThread();
				}
				else if (loops & MAX_SPIN) { //try to get lock MAX_SPIN times
#ifdef DEBUG_FASTLOCKS
					std::cout << "YieldProcessor: " << loops << "\n";
#endif
					YieldProcessor();
					
				}
				else { //sleep if lock is not successful after trying MAX_SPIN times
#ifdef DEBUG_FASTLOCKS					
					std::cout << "Sleep: " << loops << "\n";
#endif
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

//Experimental TTAS spinlock without re-entrance support. 
class FasterLock
{
private:
	volatile long lockCount;
	bool only1CoreAvailable;

public:
	static bool GetOnly1CoreAvailable()
	{
		DWORD_PTR procMask, sysMask;
		GetProcessAffinityMask(GetCurrentProcess(), &procMask, &sysMask);
		return (procMask & (procMask - 1)) == 0;
	}

	void SetOnly1CoreAvailable(bool value)
	{
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
#ifdef DEBUG_FASTLOCKS
					std::cout << "SwitchToThread: " << loops << "\n";
#endif
					SwitchToThread();
				}
				else if (loops & MAX_SPIN) { //try to get lock MAX_SPIN times
#ifdef DEBUG_FASTLOCKS
					std::cout << "YieldProcessor: " << loops << "\n";
#endif
					YieldProcessor();

				}
				else { //sleep if lock is not successful after trying MAX_SPIN times
#ifdef DEBUG_FASTLOCKS					
					std::cout << "Sleep: " << loops << "\n";
#endif
					Sleep(1);
				}
			}
		}		
	}

	inline void unlock() {
		lockCount = 0;
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
