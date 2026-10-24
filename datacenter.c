#include "datacenter.h"
#include "datacenter_utils.h"

#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>

int datacenter_init(DataCenter *dc) {
  dc->servers = NULL;
  dc->num_servers = 0;

  dc->num_vm_types = 0;

  dc->num_reservations = 0;

  dc->configured = 0;

  dc->total_cpu = 0.0;

  if (pthread_mutex_init(&dc->lock, NULL) != 0){
    fprintf(stderr, "Failed to initialize Data Center lock.\n");
    return 1;
  }

  return 0;
}

void datacenter_destroy(DataCenter *dc) {
  for (size_t i = 0; i < dc->num_reservations; i++) {
    Reservation *res = &dc->reservations[i];
    if (res->state == RES_STATE_RUNNING) {
      wait_for_all_vms(res);
    }
  }

  for (size_t i = 0; i < dc->num_servers; i++) {
    for(size_t j = 0; j < dc->servers[i].num_hosted_vms; j++){
      free(dc->servers[i].hosted_vms[j]);
    }
  }
  free(dc->servers);
  pthread_mutex_destroy(&dc->lock);
}

int datacenter_configure(DataCenter *dc, size_t num_servers, Resources *resources){
  if(dc->configured){
    fprintf(stderr, "Data Center is already configured!\n");
    return 1;
  }

  if (num_servers == 0 || resources->ram == 0 || resources->disk == 0 || resources->cpu <= 0.0) {
    fprintf(stderr, "Invalid configuration values.\n");
    return 1;
  }

  dc->servers = (Server*)malloc(num_servers*sizeof(Server));

  if(dc->servers == NULL){
    fprintf(stderr, "Failed to allocate memory for servers.\n");
    return 1;
  }

  dc->total_cpu = 0.0;

  for (size_t i = 0; i < num_servers; i++) {
    dc->servers[i].id = i+1;
    dc->servers[i].total = *resources;
    dc->servers[i].available = *resources;
    dc->servers[i].num_hosted_vms = 0;
    dc->total_cpu += resources->cpu;
  }

  dc->num_servers = num_servers;
  dc->configured = 1;

  return 0;
}

static int datacenter_define_VM_unlocked(DataCenter *dc, VMType *type){
  if (!dc->configured) {
    fprintf(stderr, "Data Center needs to be configured!\n");
    return 1;
  }

  if(VMType_exists(dc, type->id) != NULL){
    fprintf(stderr, "VM type with id %s already exists.\n", type->id);
    return 1;
  }

  if (!file_exists(type->exec_path)) {
    fprintf(stderr, "Invalid executable path.\n");
    return 1;
  }

  if (!path_exists(type->input_folder)) {
    fprintf(stderr, "Invalid input folder.\n");
    return 1;
  }

  if(dc->num_vm_types >= MAX_VM_TYPES){
    fprintf(stderr, "Maximum of VM types reached.\n");
    return 1;
  }
  
  dc->vm_types[dc->num_vm_types++] = *type;

  return 0;
}

int datacenter_define_VM(DataCenter *dc, VMType *type){
  pthread_mutex_lock(&dc->lock);
  int ret = datacenter_define_VM_unlocked(dc, type);
  pthread_mutex_unlock(&dc->lock);
  return ret;
}

// Caller must hold dc->lock.
static int datacenter_reserve_unlocked(DataCenter *dc, Reservation *reservation){
  if (!dc->configured) {
    fprintf(stderr, "Data Center needs to be configured!\n");
    return 1;
  }

  if(reservation_validate(dc, reservation) != 0){
    return 1;
  }

  if (dc->num_reservations >= MAX_RESERVATIONS){
    fprintf(stderr, "Max number of reservations reached\n");
    return 1;
  }

  if (reservation_commit(dc, reservation) != 0)
    return 1;

  dc->reservations[dc->num_reservations] = *reservation;
  dc->num_reservations++;

  return 0;
}

int datacenter_reserve(DataCenter *dc, Reservation *reservation) {
  pthread_mutex_lock(&dc->lock);
  int ret = datacenter_reserve_unlocked(dc, reservation);
  pthread_mutex_unlock(&dc->lock);
  return ret;
}


static int datacenter_execute_unlocked(DataCenter *dc, const char *reservation_id) {
  if(!dc->configured){
    fprintf(stderr, "Data Center needs to be configured!\n");
    return 1;
  }

  Reservation *res = find_pending_reservation(dc, reservation_id);
  if (!res) return 1;

  if (mkdir("/tmp/CloudIST", 0755) == -1 && errno != EEXIST){
  fprintf(stderr, "Failed to create /tmp/CloudIST.\n");
  return 1;
  }
  
  for (size_t i = 0; i < res->num_vms; i++) {
    VM *vm = res->vms[i];
    
    // Create VM working directory
    char vm_dir[MAX_PATH_SIZE];
    snprintf(vm_dir, sizeof(vm_dir), "/tmp/CloudIST/%s", vm->id);
    
    if (mkdir(vm_dir, 0755) == -1 && errno != EEXIST) {
      fprintf(stderr, "Failed to create VM directory %s.\n", vm_dir);
      return 1;
    }
    
    // Copy the VM type's input folder to the VM's working directory
    char input_dst[MAX_PATH_SIZE];
    int n = snprintf(input_dst, sizeof(input_dst), "%s/input", vm_dir);

    if (n < 0 || (size_t)n >= sizeof(input_dst)) {
      fprintf(stderr, "Path truncation error for VM %s input directory.\n", vm->id);
      return 1;
    }

    if (mkdir(input_dst, 0755) == -1 && errno != EEXIST) {
      fprintf(stderr, "Failed to create input directory %s.\n", input_dst);
      return 1;
    }
    
    if (copy_directory(vm->type->input_folder, input_dst) != 0) {
      fprintf(stderr, "Failed to copy input directory for VM %s.\n", vm->id);
      return 1;
    }
  }

  if (spawn_all_vms(dc, res) != 0) return 1;

  res->state = RES_STATE_RUNNING;
/*
  wait_for_all_vms(res);

  res->state = RES_STATE_FINISHED;

  reservation_destroy(dc, res);
*/
  return 0;
}

int datacenter_execute(DataCenter *dc, const char *reservation_id){
  pthread_mutex_lock(&dc->lock);
  int ret = datacenter_execute_unlocked(dc, reservation_id);
  pthread_mutex_unlock(&dc->lock);
  return ret;
}

static int datacenter_list_unlocked(DataCenter *dc) {
  if (!dc->configured) {
    fprintf(stderr, "Data Center needs to be configured!\n");
    return 1;
  }

  printf("=================================== DATA CENTER STATUS ===================================\n");
  printf("\n[ PHYSICAL SERVERS ] (%zu total)\n", dc->num_servers);
  printf("------------------------------------------------------------------------------------------\n");

  for (size_t i = 0; i < dc->num_servers; i++) {
    Server server = dc->servers[i];

    Resources used = server.total;
    resources_sub(&used, server.available);

    printf(" Server #%zu\n", server.id);
    printf("   ├─ Resources Total     : %6.2f vCPUs | %3zu GB RAM | %3zu GB Disk\n",
            server.total.cpu, server.total.ram, server.total.disk);
    printf("   ├─ Resources Used      : %6.2f vCPUs | %3zu GB RAM | %3zu GB Disk\n",
            used.cpu, used.ram, used.disk);
    printf("   ├─ Resources Available : %6.2f vCPUs | %3zu GB RAM | %3zu GB Disk\n",
            server.available.cpu, server.available.ram, server.available.disk);
    printf("   └─ Hosted VMs (%zu):\n", server.num_hosted_vms);

    if (server.num_hosted_vms == 0) {
      printf("        (None)\n");
    } else {
      for (size_t j = 0; j < server.num_hosted_vms; j++) {
        VM *vm = server.hosted_vms[j];
        const char *prefix = (j == server.num_hosted_vms - 1) ? "└──" : "├──";

        printf("        %s VM ID : %-16s | Type: %-10s | State: %-10s",
                prefix,
                vm->id,
                vm->type ? vm->type->id : "Unknown",
                vm_state_to_string(vm->state));

        if (vm->state == VM_STATE_RUNNING) {
          printf(" | PID: %d", (int)vm->pid);
        }
        printf("\n");
      }
    }
    printf("\n");
  }

  printf("[ DEFINED VM TYPES ] (%zu registered)\n", dc->num_vm_types);
  printf("------------------------------------------------------------------------------------------\n");
  if (dc->num_vm_types == 0) {
    printf(" (None)\n\n");
  } else {
      for (size_t i = 0; i < dc->num_vm_types; i++) {
        VMType vt = dc->vm_types[i];
        printf("  * Type ID: %-12s | Requirements: %.2f vCPU, %zu GB RAM, %zu GB Disk\n",
                vt.id, vt.required.cpu, vt.required.ram, vt.required.disk);
        printf("    ├─ Executable : %s\n", vt.exec_path);
        printf("    └─ Input Dir  : %s\n\n", vt.input_folder);
      }
      printf("\n");
  }

  printf("[ RESERVATIONS ] (%zu total)\n", dc->num_reservations);
  printf("------------------------------------------------------------------------------------------\n");
  if (dc->num_reservations == 0) {
    printf(" (None)\n\n");
  } else {
    for (size_t i = 0; i < dc->num_reservations; i++) {
      Reservation res = dc->reservations[i];
      printf("  * Reservation ID: %-12s | State: %-8s | Total VMs: %zu\n",
              res.id, res_state_to_string(res.state), res.num_vms);

      printf("    ├─ Requested Pairs:\n");
      for (size_t k = 0; k < res.items_count; k++) {
        printf("    │    • %-12s : %zu unit(s)\n", res.items[k].vm_type_id, res.items[k].amount);
      }

      printf("    └─ Allocated VMs:\n");
      if (res.num_vms == 0) {
        printf("         (None)\n");
      } else {
          for (size_t k = 0; k < res.num_vms; k++) {
            VM *vm = res.vms[k];
            const char *prefix = (k == res.num_vms - 1) ? "└──" : "├──";
            printf("         %s ID: %-15s | Host Server: #%zu\n",
                    prefix,
                    vm->id,
                    vm->server->id);
          }
      }
      printf("\n");
    }
  }

  printf("================================================================--------------------------\n\n");
  return 0;
}

int datacenter_list(DataCenter *dc){
  pthread_mutex_lock(&dc->lock);
  int ret = datacenter_list_unlocked(dc);
  pthread_mutex_unlock(&dc->lock);
  return ret;
}

void datacenter_wait(unsigned int delay_ms){
  struct timespec delay = (struct timespec){delay_ms / 1000, (delay_ms % 1000) * 1000000};
  nanosleep(&delay, NULL);
}

static int datacenter_finished_unlocked(DataCenter *dc) {
  int failed = 0;
  size_t i = 0;

  while (i < dc->num_reservations) {
    Reservation *res = &dc->reservations[i];

    if (res->state != RES_STATE_RUNNING) {
      i++;
      continue;
    }

    for (size_t j = 0; j < res->num_vms; j++) {
      VM *vm = res->vms[j];

      if (vm->state != VM_STATE_RUNNING) {
        continue;
      }

      int status = 0;
      pid_t result;

      do {
        result = waitpid(vm->pid, &status, WNOHANG);
      } while (result == -1 && errno == EINTR);

      if (result == 0) {
        continue;
      }

      if (result == -1) {
        perror("waitpid");
        failed = 1;
        continue;
      }

      if (WIFEXITED(status) || WIFSIGNALED(status)) {
        vm->state = VM_STATE_TERMINATED;
      }
    }

    int all_terminated = 1;
    for (size_t j = 0; j < res->num_vms; j++) {
      if (res->vms[j]->state != VM_STATE_TERMINATED) {
        all_terminated = 0;
        break;
      }
    }

    if (all_terminated) {
      reservation_destroy(dc, res);
      // The next reservation has shifted into index i.
    } else {
      i++;
    }
  }

  return failed;
}

int datacenter_finished(DataCenter *dc){
  pthread_mutex_lock(&dc->lock);
  int ret = datacenter_finished_unlocked(dc);
  pthread_mutex_unlock(&dc->lock);
  return ret;
}