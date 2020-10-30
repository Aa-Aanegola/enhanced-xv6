#include "types.h"
#include "stat.h"
#include "user.h"
#include "fs.h"

int main(int argc, char **argv)
{
	int wtime, rtime = 0;

	int pid = fork();

	if(pid < 0)
	{
		printf(2, "Fork Failed\n");
		exit();
	}

	if(pid == 0)
	{
		if(argc == 1)
		{
			printf(1, "Running simple for loop\n");
			// Simulate a process by simply running an empty for loop
			sleep(200);

			int c = 1245;
			for(long long int i = 0; i<1000000000000; i++)
				c = (c*2)%9887564;
			exit();
		}
		else
		{
			if(exec(argv[1], argv+1) < 0)
			{
				printf(2, "Exec failed\n");
				exit();
			}
		}
	}

	int ret = waitx(&wtime, &rtime);

	printf(1, "Running the command took : \nWait : %d \tRun : %d\t Ret : %d\n", wtime, rtime, ret);

	exit();
}