#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h> // header POSIX
#include <string.h>
#include <fcntl.h>

#include "parser.h"
#include "datacenter.h"
#include "constants.h"
#include "filesystem.h"

static int compare_names(const void *a, const void *b){
	return strcmp((const char *)a, (const char *)b);
}

static void process_commands(DataCenter *dc, int fd){
	while(1){
		switch (get_next_command(fd)){
			case CMD_DEFINE: {
				VMType vmtype;

				if (parse_define(fd, &vmtype) != 0) {
					fprintf(stderr, "Invalid define command. See H (help) for usage.\n");
					continue;
				}

				if(datacenter_define_VM(dc, &vmtype) != 0){
					fprintf(stderr, "Failed to define VM.\n");
					continue;
				}

				printf("VM successfully defined!\n");

				break;
			}

			case CMD_RESERVE: {
				Reservation reservation = {0};

				size_t num_items = parse_reserve(fd, &reservation, MAX_RESERVATIONS_ITEMS);

				if (num_items == 0) {
					fprintf(stderr, "Invalid reserve command. See H (help) for usage.\n");
					continue;
				}

				if (datacenter_reserve(dc, &reservation) != 0) {
					fprintf(stderr, "Failed to reserve VMs.\n");
					continue;
				}

				printf("Reservation made successfully!\n");

				break;
			}

			case CMD_EXECUTE:
				char id[MAX_STRING_SIZE];

				if(parse_execute(fd, id) != 0){
					fprintf(stderr, "Invalid execute command. See H (help) for usage.\n");
					continue;
				}

				if (datacenter_execute(dc, id) != 0) {
					fprintf(stderr, "Failed to execute reservation.\n");
					continue;
				}

				printf("Finished reservation execution!\n");

				break;

			case CMD_LIST:
				if (datacenter_list(dc) != 0) {
					fprintf(stderr, "Failed to list VMs.\n");
					continue;
				}

				break;

			case CMD_WAIT:
				unsigned int delay;

				if(parse_wait(fd, &delay) != 0){
					fprintf(stderr, "Invalid wait command. See H (help) for usage.\n");
					continue;
				}

				datacenter_wait(delay);
				break;

			case CMD_INVALID:
				fprintf(stderr, "Invalid Command. See H (help) for usage.\n");
				break;

			case CMD_HELP:
				printf(
					"Spaces between arguments are allowed, but not after command end.\n"
					"Available commands:\n"
					" D <VM_TYPE_ID> <INPUT_FOLDER> <EXECUTABLE_PATH> <RAM_NEEDED> <DISK_NEEDED> <VCPU_NEEDED_COUNT>\n"
					" R <RESERVATION_ID> [<VM_TYPE_ID> <COUNT> <SERVER_ID>]+\n"
					" A <RESERVATION_ID>\n"
					" L\n"
					" E <DELAY_MS>\n"
					" H\n"
				);
				break;

			case CMD_EMPTY:
				break;

			case EOC:
				return;
		}
	}

}

int main(int argc, char **argv){
	DataCenter dc;
	datacenter_init(&dc);

	if (argc != 7 && argc != 8) {
    fprintf(stderr, "Usage: %s <servers> <ram> <disk> <cpus> <selected_cpus> <dir>\n", argv[0]);
    return 1;
  }

	size_t servers;
	size_t ram;
	size_t disk;
	double cpu;
	size_t selected_cpus;
	const char *input_dir = argv[6];

	if (parse_size_t_arg(argv[1], &servers) != 0 ||
			parse_size_t_arg(argv[2], &ram) != 0 ||
			parse_size_t_arg(argv[3], &disk) != 0 ||
			parse_double_arg(argv[4], &cpu) != 0 ||
			parse_size_t_arg(argv[5], &selected_cpus) != 0){
		fprintf(stderr, "Invalid command line arguments.\n");
		return 1;
	}

	if (path_exists(input_dir) == 0){
		fprintf(stderr, "Invalid directory.\n");
		return 1;
	}

	
	Resources resources = {
    .ram = ram,
    .disk = disk,
    .cpu = cpu
	};

	if(datacenter_configure(&dc, servers, &resources) != 0){
		fprintf(stderr, "Failed to configure Data Center.\n");
		return 1;
	}

	// Abre a dir de input para listar os ficheiros .conf
	DIR *dir = opendir(input_dir);

	if (dir == NULL){
		fprintf(stderr, "Failed to open input directory.\n");
		datacenter_destroy(&dc);
		return 1;
	}

	struct dirent *entry;

	char conf_files[MAX_CONF_FILES][MAX_PATH_SIZE]; // matriz
	size_t num_conf_files = 0; // contador

	while ((entry = readdir(dir)) != NULL){
		size_t len = strlen(entry->d_name);

		if (len <= 5 || strcmp(entry->d_name + len - 5, ".conf") != 0){ // filtro
			continue;
		}

		if (num_conf_files >= MAX_CONF_FILES){
			fprintf(stderr, "Too many .conf files in input directory.\n");
			closedir(dir);
			datacenter_destroy(&dc);
			return 1;
		}


		snprintf(conf_files[num_conf_files], MAX_PATH_SIZE, "%s", entry->d_name);
		num_conf_files++;

	
	}

	closedir(dir);

	qsort(conf_files, num_conf_files, MAX_PATH_SIZE, compare_names);

	for (size_t i = 0; i < num_conf_files; i++){
		char path[MAX_PATH_SIZE];
		int n = snprintf(path, sizeof(path), "%s/%s", input_dir, conf_files[i]);
		
		if (n < 0 || (size_t)n >= sizeof(path)){
			fprintf(stderr, "Path too long: %s\n", conf_files[i]);
			continue;
		}
		printf("%s\n", path); // teste temporário

		int fd = open(path, O_RDONLY);
		if (fd == -1){
			fprintf(stderr, "Failed to open %s\n", path);
			continue;
		}
		process_commands(&dc, fd);
		close(fd);
	}
	datacenter_destroy(&dc);
	return 0;

	
}