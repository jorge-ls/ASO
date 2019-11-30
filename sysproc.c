#include "types.h"
#include "x86.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"

int
sys_fork(void)
{
  return fork();
}

int
sys_exit(void)
{
  exit();
  return 0;  // not reached
}

int
sys_wait(void)
{
  return wait();
}

int
sys_kill(void)
{
  int pid;

  if(argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

int
sys_getpid(void)
{
  return myproc()->pid;
}

int
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  addr = myproc()->sz;
  //¿Comprobar si el nuevo tamaño llega al kernbase?
  if (myproc()->sz + n >= KERNBASE){
	return -1;
  }
  myproc()->sz += n;
  if (n < 0){ //si n < 0 liberar marcos mapeados
	if(growproc(n) < 0)
    		return -1;
  }
  return addr;
}

int
sys_sleep(void)
{
  int n;
  uint ticks0;

  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

// return how many clock tick interrupts have occurred
// since start.
int
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

int sys_date(void){
	struct rtcdate *r;
	if (argptr(0,(char**)&r,sizeof(struct rtcdate)) != 0){
		return -1;
	}
	cmostime(r);
	return 0;
}

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  if(argint(n, &fd) < 0)
    return -1;
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

int sys_dup2(void){
	struct file *f;
	struct file *f2;
	int newfd;
	int oldfd;
	if(argfd(0, &oldfd, &f) < 0)
   	  return -1;
	if(argint(1, &newfd) < 0)
          return -1;
	if (newfd < 0 || newfd >= NOFILE)
	  return -1;
	if (oldfd == newfd){
		return oldfd; 
	}
	struct proc *curproc = myproc();
	if (curproc->ofile[newfd] != 0){
		f2 = curproc->ofile[newfd];
		curproc->ofile[newfd] = 0;
		fileclose(f2);
	}
	curproc->ofile[newfd] = f;
	filedup(f);
        return newfd;
}


