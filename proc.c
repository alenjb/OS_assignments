#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

# define MAX_UINT 4294967295

struct
{
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

static int weightArr[]= {  //weight 계산 테이블
  /* 0 */		88761,		71755,		56483,		46273,		36291,
  /* 5 */		29154,		23254,		18705,		14949,		11916,
  /* 10 */		9548,		7620,		6100,		4984,		3906,
  /* 15 */		3121,		2501,		1991,		1586,		1277,
  /* 20 */		1024,		820,		655,		526,		423,
  /* 25 */		335,		272,		215,		172,		137,
  /* 30 */		110,		87,		70,		56,		45,
  /* 35 */		36,		29,		23,		18,		15};

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int cpuid()
{
  return mycpu() - cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu *
mycpu(void)
{
  int apicid, i;

  if (readeflags() & FL_IF)
    panic("mycpu called with interrupts enabled\n");

  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i)
  {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc *
myproc(void)
{
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  
  popcli();
  return p;
}

// PAGEBREAK: 32
//  Look in the process table for an UNUSED proc.
//  If found, change state to EMBRYO and initialize
//  state required to run in the kernel.
//  Otherwise return 0.
static struct proc *
allocproc(void)
{
  struct proc *p;
  char *sp;
  acquire(&ptable.lock);

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if (p->state == UNUSED)
      goto found;
  }

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;
  setnice(p->pid, 20); // nice값 20으로 초기화

  release(&ptable.lock);

  // Allocate kernel stack.
  if ((p->kstack = kalloc()) == 0)
  {
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe *)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint *)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context *)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

// PAGEBREAK: 32
//  Set up first user process.
void userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();

  initproc = p;
  if ((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0; // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->weight = weightArr[p->nice - 20]; // weight값을 하드코딩한 어레이에서 가져오기
  p->runtime = 0; //runtime을 0으로
  p->lastRuntime = 0; //최근 실행시간 = 0
  p->timeslice = 0; //timeslice 0으로 초기화
  //weight 계산
  // 1. fork를 통해 생성된 프로세스는 부모의 vruntime을 상속받음
  if (p->parent)
    p->vruntime = p->parent->vruntime; 
  // 2. 이외
  p->vruntime = 1024 / p->weight; // vrumtime을 1tick * 1024 / weight of current process로 초기화

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if (n > 0)
  {
    if ((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  else if (n < 0)
  {
    if ((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if ((np = allocproc()) == 0)
  {
    return -1;
  }

  // Copy process state from proc.
  if ((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0)
  {
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for (i = 0; i < NOFILE; i++)
    if (curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);
  //nice value 복사
  setnice(np->pid, curproc->nice);
  //부모의 vruntime을 상속
  np->vruntime = curproc->vruntime;
  np->runtime = 0;

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if (curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for (fd = 0; fd < NOFILE; fd++)
  {
    if (curproc->ofile[fd])
    {
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->parent == curproc)
    {
      p->parent = initproc;
      if (p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();

  acquire(&ptable.lock);
  for (;;)
  {
    // Scan through table looking for exited children.
    havekids = 0;
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    {
      if (p->parent != curproc)
        continue;
      havekids = 1;
      if (p->state == ZOMBIE)
      {
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if (!havekids || curproc->killed)
    {
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock); // DOC: wait-sleep
  }
}

// PAGEBREAK: 42
//  Per-CPU process scheduler.
//  Each CPU calls scheduler() after setting itself up.
//  Scheduler never returns.  It loops, doing:
//   - choose a process to run
//   - swtch to start running that process
//   - eventually that process transfers control
//       via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;

  for (;;)
  {
    // Enable interrupts on this processor.
    sti();

    // Initialize variables for CFS scheduling
    struct proc *chosen = 0;
    uint min_vruntime = MAX_UINT; // 가장 큰 unsigned int 값으로 초기화
    uint totalWeight = calculate_total_weight();
    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    {
      if (p->state != RUNNABLE)
        continue;
      // Calculate vruntime
      uint vruntime = p->lastRuntime * 1024 / totalWeight;
      
      //제일 최소값의 vruntime을 찾기
      if (vruntime < min_vruntime)
      {
        min_vruntime = vruntime;
        chosen = p;
      }
    }
    //선택한 프로세스를 스케줄링
    if (chosen)
    {
      // Switch to chosen process. It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      c->proc = chosen;
      //현재 프로세스의 lastRuntime=0으로 초기화
      chosen->lastRuntime = 0;
      //p의 타임슬라이스 계산
      chosen->timeslice = 10 * p->weight / totalWeight;

      switchuvm(chosen);
      chosen->state = RUNNING;

      swtch(&(c->scheduler), chosen->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    }
    release(&ptable.lock);
  }
}


// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void sched(void)
{
  int intena;
  struct proc *p = myproc();

  if (!holding(&ptable.lock))
    panic("sched ptable.lock");
  if (mycpu()->ncli != 1)
    panic("sched locks");
  if (p->state == RUNNING)
    panic("sched running");
  if (readeflags() & FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void yield(void)
{
  acquire(&ptable.lock); // DOC: yieldlock
  myproc()->state = RUNNABLE;
  //vruntime 계산
  uint totalWeight = calculate_total_weight();
  uint vruntime = myproc()->lastRuntime * 1024 / totalWeight;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first)
  {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();

  if (p == 0)
    panic("sleep");

  if (lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if (lk != &ptable.lock)
  {                        // DOC: sleeplock0
    acquire(&ptable.lock); // DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if (lk != &ptable.lock)
  { // DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

// PAGEBREAK!
//  Wake up all processes sleeping on chan.
//  The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;
  struct proc *this; //깨어날 프로세스를 가리키는 포인터
  int numRunnable = 0; //runnable인 프로세스의 숫자
  uint minVtime = MAX_UINT; // 레디큐의 minVtime 값
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == RUNNABLE){//runnable한 프로세스가 있으면
      numRunnable ++; //runnable 프로세스 개수 증가
      if(p->vruntime < minVtime){
        minVtime = p->vruntime; //minVtime 업데이트
      }
    }
    else if (p->state == SLEEPING && p->chan == chan){
      p->state = RUNNABLE;
      this =p;
    }    
  }
  //wakeup시 runnable인 프로세스가 없으면 
  if (numRunnable == 0){
    this->vruntime = 0; // vrumtime = 0
  }else{ //wakeup시 runnable한 프로세스가 있으면
    this->vruntime = minVtime - 1; //minimum vruntime of processes in the ready queue – vruntime(1tick)
  }
}

// Wake up all processes sleeping on chan.
void wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
  
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->pid == pid)
    {
      p->killed = 1;
      // Wake process from sleep if necessary.
      if (p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

// PAGEBREAK: 36
//  Print a process listing to console.  For debugging.
//  Runs when user types ^P on console.
//  No lock to avoid wedging a stuck machine further.
void procdump(void)
{
  static char *states[] = {
      [UNUSED] "unused",
      [EMBRYO] "embryo",
      [SLEEPING] "sleep ",
      [RUNNABLE] "runble",
      [RUNNING] "run   ",
      [ZOMBIE] "zombie"};
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->state == UNUSED)
      continue;
    if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if (p->state == SLEEPING)
    {
      getcallerpcs((uint *)p->context->ebp + 2, pc);
      for (i = 0; i < 10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

// getname
int getpname(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->pid == pid)
    {
      cprintf("%s\n", p->name);
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

// getnice
int getnice(int pid)
{
  // 프로세스 구조체 선언
  struct proc *p;

  // ptable 가져오기
  acquire(&ptable.lock);

  // ptable의 모든 프로세스에서 검색
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->pid == pid)
    { // pid가 일치하면
      release(&ptable.lock);
      return p->nice; // nice값을 리턴
    }
  }
  release(&ptable.lock);
  return -1; // 일치하는 pid가 없어서 -1을 리턴
}

// setnice
int setnice(int pid, int value)
{
  // 프로세스 구조체 선언
  struct proc *p;

  // ptable 가져오기
  acquire(&ptable.lock);

  // ptable의 모든 프로세스에서 검색
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->pid == pid)
    { // pid가 일치하면
      if (value >= 0 && value <= 39)
      {                  // nice value 범위: 0~39
        p->nice = value; // nice값을 value로 변경
        release(&ptable.lock);
        return 0; // setnice 성공
      }
      else
        release(&ptable.lock);
      return -1; // nice값이 허용범위를 벗어나서 실패
    }
  }
  release(&ptable.lock);
  return -1; // 일치하는 pid가 없어서 실패
}

// ps
void ps(int pid)
{
  // 프로세스 구조체 선언
  struct proc *p;

  // ptable 가져오기
  acquire(&ptable.lock);
  cprintf("=== TEST START ===");

  // pid가 0이면 모든 프로세스의 정보를 출력
  if (pid == 0)
  {
    cprintf("name\t\t\tpid\t\t\tstate   \t\t\tpriority\t\t\truntime/weight\t\t\truntime\t\t\tvruntime\t\t\t\ttick %llu\n", ticks*1000);
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    {
      unsigned long long runtime = p->runtime * 1000;
      unsigned long long vruntime = p->vruntime * 1000;
      int weight = p->weight;
      unsigned long long rw = runtime / weight;
      //상태를 문자열로 변환
      switch (p->state) {
      case UNUSED:
          break;
      case EMBRYO:
          cprintf("%s\t\t\t%d\t\t\tEMBRYO  \t\t\t%d\t\t\t%d\t\t\t%d\t\t\t%llu\n", p->name, p->pid, p->nice, rw, runtime, vruntime);
          break;
      case SLEEPING:
          cprintf("%s\t\t\t%d\t\t\tSLEEPING\t\t\t%d\t\t\t%d\t\t\t%d\t\t\t%llu\n", p->name, p->pid, p->nice, rw, runtime, vruntime);
          break;
      case RUNNABLE:
          cprintf("%s\t\t\t%d\t\t\tRUNNABLE\t\t\t%d\t\t\t%d\t\t\t%d\t\t\t%llu\n", p->name, p->pid, p->nice, rw, runtime, vruntime);
          break;
      case RUNNING:
          cprintf("%s\t\t\t%d\t\t\tRUNNING \t\t\t%d\t\t\t%d\t\t\t%d\t\t\t%llu\n", p->name, p->pid, p->nice, rw, runtime, vruntime);
          break;
      case ZOMBIE:
          cprintf("%s\t\t\t%d\t\t\tZOMBIE  \t\t\t%d\t\t\t%d\t\t\t%d\t\t\t%llu\n", p->name, p->pid, p->nice, rw, runtime, vruntime);
          break;
      }    
    }
    release(&ptable.lock);
    return;
  }
  else
  {
    // pid가 0이 아니면 해당 프로세스의 정보를 출력
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    {
      
      // pid가 일치하면 해당 프로세스의 정보를 출력
      if (p->pid == pid)
      {
        cprintf("name\t\t\tpid\t\t\tstate   \t\t\tpriority\t\t\truntime/weight\t\t\truntime\t\t\tvruntime\t\t\t\ttick %llu\n", ticks*1000);
        unsigned long long runtime = p->runtime * 1000;
        unsigned long long vruntime = p->vruntime * 1000;
        int weight = p->weight;
        unsigned long long rw = runtime / weight;
        //상태를 문자열로 변환
        switch (p->state) {
        case UNUSED:
            break;
        case EMBRYO:
          cprintf("%s\t\t\t%d\t\t\tEMBRYO  \t\t\t%d\t\t\t%d\t\t\t%d\t\t\t%llu\n", p->name, p->pid, p->nice, rw, runtime, vruntime);
          break;
      case SLEEPING:
          cprintf("%s\t\t\t%d\t\t\tSLEEPING\t\t\t%d\t\t\t%d\t\t\t%d\t\t\t%llu\n", p->name, p->pid, p->nice, rw, runtime, vruntime);
          break;
      case RUNNABLE:
          cprintf("%s\t\t\t%d\t\t\tRUNNABLE\t\t\t%d\t\t\t%d\t\t\t%d\t\t\t%llu\n", p->name, p->pid, p->nice, rw, runtime, vruntime);
          break;
      case RUNNING:
          cprintf("%s\t\t\t%d\t\t\tRUNNING \t\t\t%d\t\t\t%d\t\t\t%d\t\t\t%llu\n", p->name, p->pid, p->nice, rw, runtime, vruntime);
          break;
      case ZOMBIE:
          cprintf("%s\t\t\t%d\t\t\tZOMBIE  \t\t\t%d\t\t\t%d\t\t\t%d\t\t\t%llu\n", p->name, p->pid, p->nice, rw, runtime, vruntime);
          break;
        }    
        release(&ptable.lock);
        return;
      }
    }
    release(&ptable.lock);
    return; // 일치하는 pid가 없으므로 종료
  }
}

//다른 프로세스 중에 runnable한 프로세스가 있는지 확인하여 있으면 1을 리턴하는 함수
static int checkRunnableProcesses(struct proc *proc)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&ptable.lock);
    if (p->state == RUNNABLE) {
      return 1;
    }
    release(&ptable.lock);
  }
  return 0;
}

// Runnable 프로세스들의 weight 합을 계산하는 함수
int
calculate_total_weight(void)
{
  int total_weight = 0;
  struct proc *p;

  acquire(&ptable.lock);
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->state == RUNNABLE)
    {
      total_weight += p->weight;
    }
  }
  release(&ptable.lock);

  return total_weight;
}
