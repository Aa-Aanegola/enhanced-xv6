#include "types.h"
#include "stat.h"
#include "param.h"
#include "mmu.h"
#include "proc.h"
#include "user.h"
#include "fs.h"


int main(int argc, char **argv)
{
	if(argc < 3)
	{
		printf(2, "setPriority needs new_priority and pid to be passed as arguments\n");
		exit();
	}

	int new_priority = atoi(argv[1]);
	int pid = atoi(argv[2]);

	int priority;
	if((priority = set_priority(new_priority, pid)) == -1)
		printf(2, "Process with specified pid not found\n");
	else
		printf(1, "Process with pid %d changed priority from %d to %d", pid, priority, new_priority);
	exit();
}