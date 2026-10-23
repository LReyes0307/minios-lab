/*
 * shell.c — SHELL INTERACTIVO DE miniOS
 *
 * Este archivo implementa el prompt `miniOS>` y el parser de comandos.
 * El main loop (shell_run) y varios comandos (help, slice, inspect, runpair,
 * log) vienen ya implementados.
 *
 * Tu trabajo es implementar las CUATRO funciones marcadas con [TODO]:
 *   1. cmd_run         — validar path y lanzar proceso con el scheduler
 *   2. cmd_ps          — mostrar la process table (process status)
 *   3. cmd_kill_proc   — matar un proceso por PID
 *   4. cmd_stats       — mostrar metricas agregadas del scheduler
 *
 * Los comandos implementados (cmd_help, cmd_slice, cmd_inspect, cmd_runpair)
 * sirven como referencia de estilo, convenciones y manejo de argumentos.
 *
 * REGLAS IMPORTANTES:
 *   - Cualquier comando que LEA process_table o ready_queue debe envolverse
 *     en block_alarm() / unblock_alarm() para evitar race con scheduler_tick.
 *   - Usa las funciones auxiliares ya disponibles:
 *       pcb_print_table()  en pcb.h
 *       pcb_state_name()   en pcb.h
 *       rq_print()         en ready_queue.h
 *       rq_remove()        en ready_queue.h
 *       timer_get_slice()  en timer.h
 */

#include "shell.h"
#include "scheduler.h"
#include "timer.h"
#include "monitor.h"
#include "pcb.h"
#include "ready_queue.h"
#include "platform/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>
#include <limits.h>

#define MAX_LINE 256

// ============================================================
// Helpers y comandos ya implementados — NO los modifiques
// ============================================================

// Block SIGALRM while modifying shared state
static sigset_t alarm_mask;

static void block_alarm(void) {
    sigprocmask(SIG_BLOCK, &alarm_mask, NULL);
}

static void unblock_alarm(void) {
    sigprocmask(SIG_UNBLOCK, &alarm_mask, NULL);
}

static void cmd_help(void) {
    printf("\nComandos disponibles:\n");
    printf("  run <binario>       Lanzar un proceso nuevo\n");
    printf("  ps                  Mostrar tabla de procesos\n");
    printf("  kill <pid>          Terminar un proceso\n");
    printf("  slice <ms>          Cambiar time slice (50-5000 ms)\n");
    printf("  inspect <pid>       Ver registros de un proceso\n");
    printf("  runpair <nombre>    Lanzar par de procesos comunicantes\n");
    printf("  stats               Mostrar metricas del scheduler\n");
    printf("  log on|off          Activar/desactivar emision de eventos JSON\n");
    printf("  help                Mostrar esta ayuda\n");
    printf("  exit                Terminar todos los procesos y salir\n");
    printf("\n");
}

static void cmd_slice(const char *arg) {
    if (!arg || strlen(arg) == 0) {
        printf("Time slice actual: %d ms\n", timer_get_slice());
        return;
    }

    int ms = atoi(arg);
    if (ms < 50 || ms > 5000) {
        printf("Error: time slice debe estar entre 50 y 5000 ms\n");
        return;
    }

    int old_ms = timer_get_slice();
    timer_set_slice(ms);
    if (scheduler_is_running()) {
        timer_start();
    }
    monitor_emit_slice_changed(old_ms, ms);
    printf("Time slice cambiado a %d ms\n", ms);
}

static void cmd_inspect(const char *arg) {
    if (!arg || strlen(arg) == 0) {
        printf("Uso: inspect <pid>\n");
        return;
    }

    int target_pid = atoi(arg);
    block_alarm();

    int found = 0;
    for (int i = 0; i < process_count; i++) {
        if (process_table[i].pid == target_pid) {
            found = 1;
            printf("\n=== PCB de PID %d (%s) ===\n", target_pid, process_table[i].name);
            printf("  Estado:           %s\n", pcb_state_name(process_table[i].state));
            printf("  CPU Time:         %.1f ms\n", process_table[i].cpu_time_ms);
            printf("  Waiting Time:     %.1f ms\n", process_table[i].wait_time_ms);
            printf("  Context Switches: %d\n", process_table[i].context_switches);

            if (platform_registers_available() && process_table[i].regs_valid) {
                printf("\n  === Registros ===\n");
                printf("  Program Counter: 0x%016llx\n", (unsigned long long)process_table[i].registers.program_counter);
                printf("  Stack Pointer:   0x%016llx\n", (unsigned long long)process_table[i].registers.stack_pointer);
                for (int r = 0; r < NUM_GENERAL_REGS; r++) {
                    printf("  Reg[%2d]:         0x%016llx\n", r,
                           (unsigned long long)process_table[i].registers.general_regs[r]);
                }
            } else if (!platform_registers_available()) {
                printf("\n  [Registros no disponibles -- SIP habilitado en macOS]\n");
                printf("  Los registros estaran disponibles en WSL2/Linux.\n");
            } else {
                printf("\n  [Registros aun no capturados]\n");
            }
            printf("\n");
            break;
        }
    }

    if (!found) {
        printf("Proceso PID %d no encontrado.\n", target_pid);
    }

    unblock_alarm();
}

static void cmd_runpair(const char *name) {
    if (!name || strlen(name) == 0) {
        printf("Uso: runpair <nombre>\n");
        printf("  Pares disponibles: ping_pong, productor_consumidor\n");
        return;
    }

    char server_path[128], client_path[128], sock_path[128];

    if (strcmp(name, "ping_pong") == 0) {
        snprintf(server_path, sizeof(server_path), "programs/bin/ping_pong_server");
        snprintf(client_path, sizeof(client_path), "programs/bin/ping_pong_client");
        snprintf(sock_path, sizeof(sock_path), "/tmp/minios_pingpong.sock");
    } else if (strcmp(name, "productor_consumidor") == 0) {
        snprintf(server_path, sizeof(server_path), "programs/bin/productor");
        snprintf(client_path, sizeof(client_path), "programs/bin/consumidor");
        snprintf(sock_path, sizeof(sock_path), "/tmp/minios_prodcons.sock");
    } else {
        printf("Par desconocido: '%s'\n", name);
        printf("  Pares disponibles: ping_pong, productor_consumidor\n");
        return;
    }

    if (access(server_path, X_OK) != 0) {
        printf("Error: '%s' no encontrado. Ejecuta 'make programs' primero.\n", server_path);
        return;
    }
    if (access(client_path, X_OK) != 0) {
        printf("Error: '%s' no encontrado. Ejecuta 'make programs' primero.\n", client_path);
        return;
    }

    unlink(sock_path);
    printf("Lanzando par '%s' con socket %s\n", name, sock_path);

    int idx1 = scheduler_create_process(server_path, sock_path);
    if (idx1 < 0) return;

    int idx2 = scheduler_create_process(client_path, sock_path);
    if (idx2 < 0) return;

    if (!scheduler_is_running()) {
        scheduler_start(timer_get_slice());
    }
}


// ============================================================
// [TODO 1/4] cmd_run
// ------------------------------------------------------------
// Parsea el comando `run <path> [arg]`, valida la ruta y crea
// un proceso a traves del scheduler. Si es el primer proceso,
// arranca el scheduler con el slice actual.
//
// Ejemplo de uso:
//   miniOS> run programs/bin/countdown 10
// ============================================================
static void cmd_run(const char *path, const char *arg) {
    // Paso 1. Si path es NULL o vacio, imprimir mensaje de uso y retornar:
    //         "Uso: run <binario> [argumento]"

    // Paso 2. Validar que el archivo existe y es ejecutable:
    //         access(path, X_OK) == 0. Si no, imprimir error y retornar.

    // Paso 3. Crear el proceso:
    //         int idx = scheduler_create_process(path, arg);
    //         Si idx < 0, retornar (el scheduler ya imprimio el error).

    // Paso 4. Si el scheduler NO esta corriendo Y la ready queue NO esta vacia,
    //         arrancar el scheduler con timer_get_slice() como slice:
    //         scheduler_start(timer_get_slice());

    if (path == NULL || path[0] == '\0') {
        printf("Uso: run <binario> [argumento]\n");
        return;
    }
    if (access(path, X_OK) == -1) {
        perror(path);
        return;
    }

    if (scheduler_create_process(path, arg) < 0) {
        return;
    }
    if (!scheduler_is_running()) {
        scheduler_start(timer_get_slice());
    }
}


// ============================================================
// [TODO 2/4] cmd_ps
// ------------------------------------------------------------
// Muestra la tabla de procesos con PCBs y la ready queue.
// Debe invocar block_alarm antes de leer la process_table y
// unblock_alarm al terminar.
//
// Formato esperado (puedes usar pcb_print_table() que ya lo hace):
//   PID    NOMBRE           ESTADO          CPU(ms) ESPERA(ms)  SWITCHES
//   -------------------------------------------------------------------
//   1234   countdown        RUNNING            1520.3      120.0       3
//   1235   primos           READY              1480.5      460.1       3
//
// Y tras la tabla, mostrar el contenido de la ready queue usando
// rq_print() (que imprime algo como "Ready Queue: PID 1235 -> PID 1234").
// ============================================================
static void cmd_ps(void) {
    block_alarm();
    if (process_count == 0) {
        printf("No hay procesos.\n");
        unblock_alarm();
        return;
    }

    printf("\n");
    pcb_print_table();
    printf("\n");
    rq_print();
    unblock_alarm();
}


// ============================================================
// [TODO 3/4] cmd_kill_proc
// ------------------------------------------------------------
// Recibe un PID como string, busca el proceso en process_table y
// lo termina con SIGKILL. Debe remover el proceso de la ready queue
// y marcar su PCB como PROC_TERMINATED.
//
// Nota: El SIGCHLD handler del scheduler recogera el proceso, pero
// es recomendable hacer waitpid aqui tambien para liberar recursos
// inmediatamente y que `ps` refleje el cambio al instante.
// ============================================================
static void cmd_kill_proc(const char *arg) {
    // Paso 1. Si arg es NULL o vacio, imprimir "Uso: kill <pid>" y retornar.

    // Paso 2. Convertir arg a entero con atoi. Si <= 0, imprimir "PID invalido"
    //         y retornar.

    // Paso 3. block_alarm() para proteger la lectura/modificacion.

    // Paso 4. Buscar el PID en process_table (loop por process_count):
    //         - Si process_table[i].pid == target_pid Y estado != PROC_TERMINATED:
    //           a) kill(target_pid, SIGKILL);
    //           b) waitpid(target_pid, &status, 0);  // limpiar zombie
    //           c) process_table[i].state = PROC_TERMINATED;
    //           d) rq_remove(i);  // sacar de la ready queue
    //           e) imprimir "Proceso PID <pid> terminado."
    //           f) break;

    // Paso 5. Si no se encontro, imprimir mensaje de error.

    // Paso 6. unblock_alarm() al terminar.

    if (arg == NULL || arg[0] == '\0') {
        printf("Uso: kill <pid>\n");
        return;
    }

    errno = 0;
    char *end = NULL;
    long parsed_pid = strtol(arg, &end, 10);
    if (errno == ERANGE || end == arg || *end != '\0' ||
        parsed_pid <= 0 || parsed_pid > INT_MAX) {
        printf("PID inválido: '%s'\n", arg);
        return;
    }

    pid_t target_pid = (pid_t)parsed_pid;
    block_alarm();

    int found = 0;
    for (int i = 0; i < process_count; i++) {
        pcb_t *pcb = &process_table[i];
        if (pcb->pid != target_pid || pcb->state == PROC_TERMINATED) {
            continue;
        }

        found = 1;
        if (kill(target_pid, SIGKILL) == -1) {
            perror("kill");
            break;
        }

        if (i == scheduler_get_running()) {
            struct timespec now;
            if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
                perror("clock_gettime");
            } else {
                pcb->cpu_time_ms += timespec_diff_ms(now, pcb->last_started);
            }
        } else {
            rq_remove(i);
        }

        pcb->state = PROC_TERMINATED;
        monitor_emit_terminated(pcb->pid, pcb->cpu_time_ms,
                                pcb->context_switches);
        printf("Proceso PID %d terminado.\n", (int)target_pid);
        break;
    }

    if (!found) {
        printf("Proceso PID %d no encontrado o ya terminado.\n",
               (int)target_pid);
    }
    unblock_alarm();
}


// ============================================================
// [TODO 4/4] cmd_stats
// ------------------------------------------------------------
// Muestra metricas agregadas del scheduler:
//   - Cantidad de procesos activos y terminados
//   - Time slice actual
//   - CPU total acumulado (suma de cpu_time_ms de todos)
//   - Context switches totales
//   - Promedios (CPU y espera) por proceso
//
// Formato sugerido:
//
//   === Estadisticas del Scheduler ===
//     Procesos activos:      2
//     Procesos terminados:   1
//     Time slice actual:     500 ms
//     CPU total acumulado:   3450.2 ms
//     Context switches:      12
//     Avg CPU por proceso:   1150.1 ms
//     Avg espera:            230.5 ms
// ============================================================
static void cmd_stats(void) {
    block_alarm();

    int active = 0;
    int terminated = 0;
    int total_switches = 0;
    double total_cpu = 0.0;
    double total_wait = 0.0;

    for (int i = 0; i < process_count; i++) {
        const pcb_t *pcb = &process_table[i];
        if (pcb->state == PROC_TERMINATED) {
            terminated++;
        } else {
            active++;
        }
        total_cpu += pcb->cpu_time_ms;
        total_wait += pcb->wait_time_ms;
        total_switches += pcb->context_switches;
    }

    printf("\n=== Estadísticas del Scheduler ===\n");
    printf("  Procesos activos:      %d\n", active);
    printf("  Procesos terminados:   %d\n", terminated);
    printf("  Time slice actual:     %d ms\n", timer_get_slice());
    printf("  CPU total acumulado:   %.1f ms\n", total_cpu);
    printf("  Context switches:      %d\n", total_switches);
    if (process_count > 0) {
        printf("  Avg CPU por proceso:   %.1f ms\n",
               total_cpu / process_count);
        printf("  Avg espera:            %.1f ms\n",
               total_wait / process_count);
    } else {
        printf("  Avg CPU por proceso:   N/A\n");
        printf("  Avg espera:            N/A\n");
    }
    unblock_alarm();
}


// ============================================================
// Main loop del shell — ya implementado, NO lo modifiques
// ============================================================
void shell_run(void) {
    char line[MAX_LINE];

    // Setup alarm mask for sigprocmask
    sigemptyset(&alarm_mask);
    sigaddset(&alarm_mask, SIGALRM);
    sigaddset(&alarm_mask, SIGCHLD);

    printf("+----------------------------------+\n");
    printf("|       miniOS v1.0                |\n");
    printf("|   Simulador de Context Switching |\n");
    printf("|   Escribe 'help' para ayuda      |\n");
    printf("+----------------------------------+\n\n");

    while (1) {
        printf("miniOS> ");
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL) {
            printf("\n");
            break;
        }

        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';

        if (strlen(line) == 0) continue;

        char *cmd = strtok(line, " \t");
        char *arg = strtok(NULL, " \t");
        char *arg2 = strtok(NULL, " \t");

        if (!cmd) continue;

        if (strcmp(cmd, "help") == 0) {
            cmd_help();
        } else if (strcmp(cmd, "run") == 0) {
            block_alarm();
            cmd_run(arg, arg2);
            unblock_alarm();
        } else if (strcmp(cmd, "ps") == 0) {
            cmd_ps();
        } else if (strcmp(cmd, "kill") == 0) {
            cmd_kill_proc(arg);
        } else if (strcmp(cmd, "slice") == 0) {
            cmd_slice(arg);
        } else if (strcmp(cmd, "inspect") == 0) {
            cmd_inspect(arg);
        } else if (strcmp(cmd, "runpair") == 0) {
            block_alarm();
            cmd_runpair(arg);
            unblock_alarm();
        } else if (strcmp(cmd, "stats") == 0) {
            cmd_stats();
        } else if (strcmp(cmd, "log") == 0) {
            if (!arg || strlen(arg) == 0) {
                printf("Log esta %s. Uso: log on|off\n",
                       monitor_is_enabled() ? "activado" : "desactivado");
            } else if (strcmp(arg, "on") == 0) {
                monitor_init(MONITOR_SOCKET_PATH);
                monitor_set_enabled(1);
            } else if (strcmp(arg, "off") == 0) {
                monitor_set_enabled(0);
                printf("Log desactivado.\n");
            } else {
                printf("Uso: log on|off\n");
            }
        } else if (strcmp(cmd, "exit") == 0) {
            block_alarm();
            scheduler_stop();
            monitor_close();
            unblock_alarm();
            printf("Hasta luego.\n");
            break;
        } else {
            printf("Comando desconocido: '%s'. Escribe 'help' para ver comandos.\n", cmd);
        }
    }
}
