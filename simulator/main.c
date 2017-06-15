#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>

#define INTERRUPT_MASK_REGISTER 0x3C
#define INTERRUPT_STATUS_REGISTER 0x3E
#define LINK_CHANGE_INT_MASK    (1<<5)

#define PHY_STATUS_REGISTER     0x6C
#define LINK_STATUS_MASK        (1<<1)


int main(int argc, char** argv) {

	int fd;
	char *regmap;
	int sock;
	struct sockaddr_un addr;
	unsigned int filesize;
	struct stat st;
	char trigger;
	int c;
	char iomem_file[200];
	char socket_file[200];

	while ((c = getopt(argc, argv, "i:s:")) != -1)
		switch(c) {
			case 'i':
			strcpy(iomem_file, optarg);
			break;

			case 's':
			strcpy(socket_file, optarg);
			break;

			case '?':
			perror("Something is odd with your parameters");
			return 1;
			break;

			default:
			abort();
		}

	fd = open(iomem_file, O_RDWR, (mode_t)0600);
	if (fd == -1) {
		perror("Error opening the iomem file");
		exit(EXIT_FAILURE);
	}

	stat(iomem_file, &st);
	filesize = st.st_size;

	regmap = mmap(0, filesize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (regmap == MAP_FAILED) {
		perror("Error mapping the iomem file");
		exit(EXIT_FAILURE);
	}
	close(fd);

	sock = socket(PF_LOCAL, SOCK_DGRAM, 0);
	addr.sun_family = AF_LOCAL;
	strcpy(addr.sun_path, socket_file);
	if (0 != connect(sock, (struct sockaddr *)&addr, sizeof(addr))) {
		perror("Could not connect to the unix domain socket");
		exit(EXIT_FAILURE);
	}


	// set the link change status bit
	regmap[INTERRUPT_STATUS_REGISTER] |= LINK_CHANGE_INT_MASK;

	// toggle the link status
	regmap[PHY_STATUS_REGISTER] ^= LINK_STATUS_MASK;

	// trigger the interrupt, if requested by the INT MASK register
	if ((regmap[INTERRUPT_MASK_REGISTER] & LINK_CHANGE_INT_MASK) == LINK_CHANGE_INT_MASK) {
		printf(" Changing Link Status to %s\n", (regmap[PHY_STATUS_REGISTER] & LINK_STATUS_MASK) == LINK_STATUS_MASK ? "UP" : "DOWN");
		send(sock, &trigger, 1, 0);
	} else
		printf("The INT Mask does not allow to trigger an interrupt, aborting\n");

	return 0;
}
