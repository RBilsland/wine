/*
 * System-dependent scheduler support
 *
 * Copyright 1998 Alexandre Julliard
 */

#include "config.h"
#include "wine/port.h"

#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/resource.h>
#ifdef HAVE_SYS_SYSCALL_H
# include <sys/syscall.h>
#endif
#ifdef HAVE_SYS_LWP_H
# include <sys/lwp.h>
#endif
#ifdef HAVE_UCONTEXT_H
# include <ucontext.h>
#endif
#ifdef HAVE_SYS_MMAN_H
#include <sys/mman.h>
#endif
#include "thread.h"
#include "wine/server.h"
#include "winbase.h"
#include "wine/winbase16.h"
#include "wine/exception.h"
#include "debugtools.h"

DEFAULT_DEBUG_CHANNEL(thread);

#if defined(linux) || defined(HAVE_CLONE)
# ifdef HAVE_SCHED_H
#  include <sched.h>
# endif
# ifndef CLONE_VM
#  define CLONE_VM      0x00000100
#  define CLONE_FS      0x00000200
#  define CLONE_FILES   0x00000400
#  define CLONE_SIGHAND 0x00000800
#  define CLONE_PID     0x00001000
# endif  /* CLONE_VM */
#endif  /* linux || HAVE_CLONE */

extern void SELECTOR_FreeFs(void);

struct thread_cleanup_info
{
    void *stack_base;
    int   stack_size;
    int   status;
};

/* temporary stacks used on thread exit */
#define TEMP_STACK_SIZE 1024
#define NB_TEMP_STACKS  8
static char temp_stacks[NB_TEMP_STACKS][TEMP_STACK_SIZE];
static LONG next_temp_stack;  /* next temp stack to use */

/***********************************************************************
 *           SYSDEPS_SetCurThread
 *
 * Make 'thread' the current thread.
 */
void SYSDEPS_SetCurThread( TEB *teb )
{
#if defined(__i386__)
    /* On the i386, the current thread is in the %fs register */
    __set_fs( teb->teb_sel );
#elif defined(HAVE__LWP_CREATE)
    /* On non-i386 Solaris, we use the LWP private pointer */
    _lwp_setprivate( teb );
#endif
}


/***********************************************************************
 *           call_on_thread_stack
 *
 * Call a function once we switched to the thread stack.
 */
static void call_on_thread_stack( void *func )
{
    __TRY
    {
        void (*funcptr)(void) = func;
        funcptr();
    }
    __EXCEPT(UnhandledExceptionFilter)
    {
        TerminateThread( GetCurrentThread(), GetExceptionCode() );
    }
    __ENDTRY
    SYSDEPS_ExitThread(0);  /* should never get here */
}


/***********************************************************************
 *           get_temp_stack
 *
 * Get a temporary stack address to run the thread exit code on.
 */
inline static char *get_temp_stack(void)
{
    unsigned int next = InterlockedExchangeAdd( &next_temp_stack, 1 );
    return temp_stacks[next % NB_TEMP_STACKS];
}


/***********************************************************************
 *           cleanup_thread
 *
 * Cleanup the remains of a thread. Runs on a temporary stack.
 */
static void cleanup_thread( void *ptr )
{
    /* copy the info structure since it is on the stack we will free */
    struct thread_cleanup_info info = *(struct thread_cleanup_info *)ptr;
    munmap( info.stack_base, info.stack_size );
    SELECTOR_FreeFs();
#ifdef HAVE__LWP_CREATE
    _lwp_exit();
#endif
    _exit( info.status );
}


/***********************************************************************
 *           SYSDEPS_StartThread
 *
 * Startup routine for a new thread.
 */
static void SYSDEPS_StartThread( TEB *teb )
{
    SYSDEPS_SetCurThread( teb );
    CLIENT_InitThread();
    SIGNAL_Init();
    __TRY
    {
        teb->startup();
    }
    __EXCEPT(UnhandledExceptionFilter)
    {
        TerminateThread( GetCurrentThread(), GetExceptionCode() );
    }
    __ENDTRY
    SYSDEPS_ExitThread(0);  /* should never get here */
}


/***********************************************************************
 *           SYSDEPS_SpawnThread
 *
 * Start running a new thread.
 * Return -1 on error, 0 if OK.
 */
int SYSDEPS_SpawnThread( TEB *teb )
{
#ifdef ERRNO_LOCATION

#if defined(linux) || defined(HAVE_CLONE)
    const int flags = CLONE_VM | CLONE_FS | CLONE_FILES | SIGCHLD;
    if (clone( (int (*)(void *))SYSDEPS_StartThread, teb->stack_top, flags, teb ) < 0)
        return -1;
    if (!(flags & CLONE_FILES)) close( teb->request_fd );  /* close the child socket in the parent */
    return 0;
#endif

#ifdef HAVE_RFORK
    const int flags = RFPROC | RFMEM; /*|RFFDG*/
    void **sp = (void **)teb->stack_top;
    *--sp = teb;
    *--sp = 0;
    *--sp = SYSDEPS_StartThread;
    __asm__ __volatile__(
    "pushl %2;\n\t"		/* flags */
    "pushl $0;\n\t"		/* 0 ? */
    "movl %1,%%eax;\n\t"	/* SYS_rfork */
    ".byte 0x9a; .long 0; .word 7;\n\t"	/* lcall 7:0... FreeBSD syscall */
    "cmpl $0, %%edx;\n\t"
    "je 1f;\n\t"
    "movl %0,%%esp;\n\t"	/* child -> new thread */
    "ret;\n"
    "1:\n\t"		/* parent -> caller thread */
    "addl $8,%%esp" :
    : "r" (sp), "g" (SYS_rfork), "g" (flags)
    : "eax", "edx");
    if (flags & RFFDG) close( teb->request_fd );  /* close the child socket in the parent */
    return 0;
#endif

#ifdef HAVE__LWP_CREATE
    ucontext_t context;
    _lwp_makecontext( &context, (void(*)(void *))SYSDEPS_StartThread, teb,
                      NULL, teb->stack_base, (char *)teb->stack_top - (char *)teb->stack_base );
    if ( _lwp_create( &context, 0, NULL ) )
        return -1;
    return 0;
#endif

#endif /* ERRNO_LOCATION */

    FIXME("CreateThread: stub\n" );
    return 0;
}


/***********************************************************************
 *           SYSDEPS_CallOnStack
 */
void SYSDEPS_CallOnStack( void (*func)(LPVOID), LPVOID arg ) WINE_NORETURN;
#ifdef __i386__
__ASM_GLOBAL_FUNC( SYSDEPS_CallOnStack,
                   "movl 4(%esp),%ecx\n\t"  /* func */
                   "movl 8(%esp),%edx\n\t"  /* arg */
                   ".byte 0x64\n\tmovl 0x04,%esp\n\t"  /* teb->stack_top */
                   "pushl %edx\n\t"
                   "xorl %ebp,%ebp\n\t"
                   "call *%ecx\n\t"
                   "int $3" /* we never return here */ );
#else
void SYSDEPS_CallOnStack( void (*func)(LPVOID), LPVOID arg )
{
    func( arg );
    while(1); /* avoid warning */
}
#endif


/***********************************************************************
 *           SYSDEPS_SwitchToThreadStack
 */
void SYSDEPS_SwitchToThreadStack( void (*func)(void) )
{
    SYSDEPS_CallOnStack( call_on_thread_stack, func );
}


/***********************************************************************
 *           SYSDEPS_ExitThread
 *
 * Exit a running thread; must not return.
 */
void SYSDEPS_ExitThread( int status )
{
    TEB *teb = NtCurrentTeb();
    struct thread_cleanup_info info;
    MEMORY_BASIC_INFORMATION meminfo;

    FreeSelector16( teb->stack_sel );
    VirtualQuery( teb->stack_top, &meminfo, sizeof(meminfo) );
    info.stack_base = meminfo.AllocationBase;
    info.stack_size = meminfo.RegionSize + ((char *)teb->stack_top - (char *)meminfo.AllocationBase);
    info.status     = status;

    SIGNAL_Reset();

    VirtualFree( teb->stack_base, 0, MEM_RELEASE | MEM_SYSTEM );
    close( teb->wait_fd[0] );
    close( teb->wait_fd[1] );
    close( teb->reply_fd );
    close( teb->request_fd );
    teb->stack_low = get_temp_stack();
    teb->stack_top = teb->stack_low + TEMP_STACK_SIZE;
    SYSDEPS_CallOnStack( cleanup_thread, &info );
}


/***********************************************************************
 *           SYSDEPS_AbortThread
 *
 * Same as SYSDEPS_ExitThread, but must not do anything that requires a server call.
 */
void SYSDEPS_AbortThread( int status )
{
    SIGNAL_Reset();
    close( NtCurrentTeb()->wait_fd[0] );
    close( NtCurrentTeb()->wait_fd[1] );
    close( NtCurrentTeb()->reply_fd );
    close( NtCurrentTeb()->request_fd );
#ifdef HAVE__LWP_CREATE
    _lwp_exit();
#endif
    for (;;)  /* avoid warning */
        _exit( status );
}


/**********************************************************************
 *           NtCurrentTeb   (NTDLL.@)
 *
 * This will crash and burn if called before threading is initialized
 */
#ifdef __i386__
__ASM_GLOBAL_FUNC( NtCurrentTeb, ".byte 0x64\n\tmovl 0x18,%eax\n\tret" );
#elif defined(HAVE__LWP_CREATE)
/***********************************************************************
 *		NtCurrentTeb (NTDLL.@)
 */
struct _TEB * WINAPI NtCurrentTeb(void)
{
    extern void *_lwp_getprivate(void);
    return (struct _TEB *)_lwp_getprivate();
}
#else
# error NtCurrentTeb not defined for this architecture
#endif  /* __i386__ */
