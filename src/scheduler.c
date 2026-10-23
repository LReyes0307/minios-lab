/*
 * scheduler.c — ESQUELETO DEL LABORATORIO
 *
 * Este archivo contiene el núcleo del scheduler round-robin de miniOS.
 * Las funciones de infraestructura (init, getters, install_sigchld, stop,
 * timespec_diff_ms) ya están implementadas.
 *
 * Tu trabajo es implementar las CUATRO funciones marcadas con [TODO]:
 *   1. scheduler_create_process  — fork + exec + SIGSTOP + PCB init
 *   2. scheduler_start           — arrancar el primer proceso y el timer
 *   3. scheduler_tick            — handler de SIGALRM (context switch)
 *   4. scheduler_sigchld         — handler de SIGCHLD (terminación)
 *
 * Cada función viene con comentarios numerados que describen el flujo
 * paso a paso. Tu trabajo es traducir cada paso a código C usando las
 * APIs de POSIX y las funciones de infraestructura disponibles.
 *
 * APIs disponibles:
 *   - POSIX:       fork, execl, waitpid, kill, clock_gettime
 *   - platform_*:  ver src/platform/platform.h
 *   - pcb_*:       ver src/pcb.h
 *   - rq_*:        ver src/ready_queue.h
 *   - timer_*:     ver src/timer.h
 *   - monitor_*:   ver src/monitor.h
 *
 * REGLAS DE SEGURIDAD EN SEÑALES (importantes para scheduler_tick y
 * scheduler_sigchld):
 *   - NO uses printf/fprintf dentro de los handlers (no son
 *     async-signal-safe). Solo kill, waitpid, clock_gettime, write.
 *   - El shell bloquea SIGALRM con sigprocmask durante sus operaciones
 *     críticas, por lo que no necesitas mutex manual sobre process_table.
 */

#include "scheduler.h"
#include "timer.h"
#include "monitor.h"
#include "platform/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <libgen.h>
#include <errno.h>

// Estado global del scheduler
static volatile int current_running = -1;   // índice en process_table del proceso RUNNING, -1 si ninguno
static volatile int scheduler_active = 0;   // 1 si el scheduler está corriendo

static int block_scheduler_signals(sigset_t *previous_mask) {
    sigset_t mask;

    if (sigemptyset(&mask) == -1 ||
        sigaddset(&mask, SIGALRM) == -1 ||
        sigaddset(&mask, SIGCHLD) == -1) {
        return -1;
    }

    return sigprocmask(SIG_BLOCK, &mask, previous_mask);
}

static int restore_signal_mask(const sigset_t *previous_mask) {
    return sigprocmask(SIG_SETMASK, previous_mask, NULL);
}

static int dispatch_next_process(void) {
    while (!rq_is_empty()) {
        int next_idx = rq_dequeue();
        if (next_idx < 0 || next_idx >= process_count ||
            process_table[next_idx].state != PROC_READY) {
            continue;
        }

        pcb_t *next = &process_table[next_idx];
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
            static const char message[] = "miniOS: clock_gettime failed in scheduler handler\n";
            write(STDERR_FILENO, message, sizeof(message) - 1);
            rq_enqueue(next_idx);
            current_running = -1;
            scheduler_active = 0;
            timer_stop();
            return -1;
        }

        next->wait_time_ms += timespec_diff_ms(now, next->last_started);
        next->state = PROC_RUNNING;
        next->last_started = now;
        if (platform_resume_process(next->pid) == -1) {
            static const char message[] = "miniOS: failed to resume a ready process\n";
            write(STDERR_FILENO, message, sizeof(message) - 1);
            next->state = PROC_TERMINATED;
            monitor_emit_terminated(next->pid, next->cpu_time_ms,
                                    next->context_switches);
            continue;
        }

        current_running = next_idx;
        scheduler_active = 1;
        timer_start();
        return next_idx;
    }

    current_running = -1;
    scheduler_active = 0;
    timer_stop();
    return -1;
}

// ============================================================
// Helpers ya implementados — NO los modifiques
// ============================================================

double timespec_diff_ms(struct timespec end, struct timespec start) {
    double sec = (double)(end.tv_sec - start.tv_sec);
    double nsec = (double)(end.tv_nsec - start.tv_nsec);
    return sec * 1000.0 + nsec / 1000000.0;
}

void scheduler_init(void) {
    process_count = 0;
    current_running = -1;
    scheduler_active = 0;
    rq_init();
}

int scheduler_get_running(void) {
    return current_running;
}

int scheduler_is_running(void) {
    return scheduler_active;
}

void scheduler_install_sigchld(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = scheduler_sigchld;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGCHLD, &sa, NULL);
}

void scheduler_stop(void) {
    timer_stop();
    scheduler_active = 0;

    for (int i = 0; i < process_count; i++) {
        if (process_table[i].state != PROC_TERMINATED) {
            kill(process_table[i].pid, SIGKILL);
            int status;
            waitpid(process_table[i].pid, &status, 0);
            process_table[i].state = PROC_TERMINATED;
        }
    }
    current_running = -1;
}


// ============================================================
// [TODO 1/4] scheduler_create_process
// ------------------------------------------------------------
// Crea un proceso nuevo a partir de un binario, lo deja detenido
// con estado PROC_READY y lo encola en la ready queue.
//
// Retorna el índice del nuevo PCB en process_table, o -1 en error.
//
// Observable correcto: `ps aux | grep <binario>` debe mostrar el
// proceso en estado T (stopped) justo después de crearlo.
// ============================================================
int scheduler_create_process(const char *path, const char *arg) {
    if (path == NULL || path[0] == '\0') {
        fprintf(stderr, "Error: ruta del ejecutable vacía.\n");
        return -1;
    }

    char *path_copy = strdup(path);
    if (path_copy == NULL) {
        perror("strdup");
        return -1;
    }
    const char *short_name = basename(path_copy);

    sigset_t previous_mask;
    if (block_scheduler_signals(&previous_mask) == -1) {
        perror("sigprocmask");
        free(path_copy);
        return -1;
    }

    int result = -1;
    pid_t pid = -1;
    int child_created = 0;
    int status = 0;

    if (process_count >= MAX_PROCESSES) {
        fprintf(stderr, "Error: la tabla de procesos está llena.\n");
        goto cleanup;
    }

    int idx = process_count;
    pid = fork();
    if (pid == -1) {
        perror("fork");
        goto cleanup;
    }

    if (pid == 0) {
        sigset_t child_mask = previous_mask;
        sigdelset(&child_mask, SIGALRM);
        sigdelset(&child_mask, SIGCHLD);
        if (sigprocmask(SIG_SETMASK, &child_mask, NULL) == -1) {
            perror("sigprocmask");
            _exit(1);
        }

        if (platform_uses_ptrace() && platform_trace_child() == -1) {
            perror("platform_trace_child");
            _exit(1);
        }

        if (arg != NULL) {
            execl(path, path, arg, (char *)NULL);
        } else {
            execl(path, path, (char *)NULL);
        }
        perror("execl");
        _exit(1);
    }
    child_created = 1;

    if (platform_uses_ptrace()) {
        pid_t waited;
        do {
            waited = waitpid(pid, &status, 0);
        } while (waited == -1 && errno == EINTR);

        if (waited == -1) {
            perror("waitpid");
            goto cleanup;
        }
        if (!WIFSTOPPED(status)) {
            fprintf(stderr, "Error: el proceso no se detuvo tras exec.\n");
            goto cleanup;
        }
    }

    pcb_init(&process_table[idx], pid, short_name);
    process_table[idx].last_started = process_table[idx].created_at;

    if (platform_uses_ptrace()) {
        if (platform_get_registers(pid, &process_table[idx].registers) == 0) {
            process_table[idx].regs_valid = 1;
        }
        if (platform_detach(pid) == -1) {
            perror("platform_detach");
            goto cleanup;
        }
    }

    if (platform_stop_process(pid) == -1) {
        perror("SIGSTOP");
        goto cleanup;
    }

    pid_t waited;
    do {
        waited = waitpid(pid, &status, WUNTRACED);
    } while (waited == -1 && errno == EINTR);

    if (waited == -1) {
        perror("waitpid");
        goto cleanup;
    }
    if (!WIFSTOPPED(status)) {
        fprintf(stderr, "Error: el proceso terminó antes de quedar detenido.\n");
        goto cleanup;
    }

    process_table[idx].state = PROC_READY;
    if (rq_enqueue(idx) == -1) {
        fprintf(stderr, "Error: no se pudo agregar el proceso a la ready queue.\n");
        goto cleanup;
    }

    process_count++;
    monitor_emit_created(pid, process_table[idx].name);
    if (process_table[idx].regs_valid) {
        monitor_emit_registers(
            pid,
            (unsigned long long)process_table[idx].registers.program_counter,
            (unsigned long long)process_table[idx].registers.stack_pointer
        );
    }
    result = idx;

cleanup:
    if (result == -1 && child_created) {
        if (kill(pid, SIGKILL) == -1 && errno != ESRCH) {
            perror("kill");
        }

        pid_t waited;
        do {
            waited = waitpid(pid, &status, 0);
        } while (waited == -1 && errno == EINTR);
        if (waited == -1 && errno != ECHILD) {
            perror("waitpid");
        }
    }

    if (restore_signal_mask(&previous_mask) == -1) {
        perror("sigprocmask");
    }
    free(path_copy);
    return result;
}


// ============================================================
// [TODO 2/4] scheduler_start
// ------------------------------------------------------------
// Arranca el scheduler: desencola el primer proceso, lo pone en
// RUNNING, le manda SIGCONT e instala el timer que dispara el
// context switch periódicamente.
//
// Observable correcto: tras crear 1 proceso y llamar start, ese
// proceso empieza a producir salida en la terminal.
// ============================================================
void scheduler_start(int slice_ms) {
    if (scheduler_active || current_running >= 0) {
        return;
    }
    if (rq_is_empty()) {
        fprintf(stderr, "No hay procesos en la ready queue.\n");
        return;
    }

    int idx = rq_dequeue();
    if (idx < 0 || idx >= process_count ||
        process_table[idx].state != PROC_READY) {
        fprintf(stderr, "Error: entrada inválida en la ready queue.\n");
        return;
    }

    pcb_t *next = &process_table[idx];
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
        perror("clock_gettime");
        rq_enqueue(idx);
        return;
    }

    next->wait_time_ms += timespec_diff_ms(now, next->last_started);
    next->last_started = now;
    next->state = PROC_RUNNING;
    if (platform_resume_process(next->pid) == -1) {
        perror("SIGCONT");
        next->state = PROC_READY;
        rq_enqueue(idx);
        return;
    }

    current_running = idx;
    scheduler_active = 1;
    timer_init(slice_ms, scheduler_tick);
    timer_start();
}


// ============================================================
// [TODO 3/4] scheduler_tick  — handler de SIGALRM
// ------------------------------------------------------------
// Se invoca CADA vez que expira el time slice. Realiza el
// context switch: detiene al proceso actual, actualiza su PCB,
// lo manda al final de la cola, saca al siguiente y lo reanuda.
//
// ¡IMPORTANTE! Esta función corre en un signal handler.
//   - NO llames printf/fprintf/malloc.
//   - Solo kill, waitpid, clock_gettime, write son seguros.
//   - Las funciones monitor_emit_* internamente usan snprintf + write,
//     que es aceptable para este proyecto educativo.
//
// Observable correcto: el Gantt chart del dashboard muestra
// segmentos alternados entre procesos cada ~slice_ms.
// ============================================================
void scheduler_tick(int signum) {
    (void)signum;
    sigset_t previous_mask;
    if (block_scheduler_signals(&previous_mask) == -1) {
        static const char message[] = "miniOS: unable to block scheduler signals\n";
        write(STDERR_FILENO, message, sizeof(message) - 1);
        return;
    }

    int running_idx = current_running;
    if (!scheduler_active || running_idx < 0 || running_idx >= process_count) {
        goto done;
    }

    pcb_t *current = &process_table[running_idx];
    pid_t from_pid = current->pid;

    if (current->state == PROC_TERMINATED) {
        current_running = -1;
        int next_idx = dispatch_next_process();
        if (next_idx >= 0) {
            monitor_emit_switch(from_pid, process_table[next_idx].pid,
                                timer_get_slice());
        }
        goto done;
    }

    if (platform_stop_process(current->pid) == -1) {
        if (errno != ESRCH) {
            static const char message[] = "miniOS: failed to stop the running process\n";
            write(STDERR_FILENO, message, sizeof(message) - 1);
            goto done;
        }
        current->state = PROC_TERMINATED;
        monitor_emit_terminated(current->pid, current->cpu_time_ms,
                                current->context_switches);
        current_running = -1;
        int next_idx = dispatch_next_process();
        if (next_idx >= 0) {
            monitor_emit_switch(from_pid, process_table[next_idx].pid,
                                timer_get_slice());
        }
        goto done;
    }

    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
        static const char message[] = "miniOS: clock_gettime failed in scheduler tick\n";
        write(STDERR_FILENO, message, sizeof(message) - 1);
        goto done;
    }

    current->cpu_time_ms += timespec_diff_ms(now, current->last_started);
    current->last_started = now;
    current->state = PROC_READY;
    current->context_switches++;
    if (rq_enqueue(running_idx) == -1) {
        static const char message[] = "miniOS: ready queue is full\n";
        write(STDERR_FILENO, message, sizeof(message) - 1);
        current->state = PROC_RUNNING;
        goto done;
    }

    current_running = -1;
    int next_idx = dispatch_next_process();
    if (next_idx >= 0) {
        monitor_emit_switch(from_pid, process_table[next_idx].pid,
                            timer_get_slice());
    }

done:
    if (restore_signal_mask(&previous_mask) == -1) {
        static const char message[] = "miniOS: unable to restore signal mask\n";
        write(STDERR_FILENO, message, sizeof(message) - 1);
    }
}


// ============================================================
// [TODO 4/4] scheduler_sigchld  — handler de SIGCHLD
// ------------------------------------------------------------
// Se invoca cuando un proceso hijo termina. Debe:
//   - Detectar TODOS los hijos terminados (puede haber varios).
//   - Actualizar su PCB a PROC_TERMINATED.
//   - Si el proceso que terminó era el RUNNING, despachar al siguiente.
//   - Si estaba en la ready queue, removerlo con rq_remove().
//
// Observable correcto: tras `exit` en miniOS, `ps aux | grep defunct`
// no muestra zombies.
// ============================================================
void scheduler_sigchld(int signum) {
    (void)signum;
    sigset_t previous_mask;
    if (block_scheduler_signals(&previous_mask) == -1) {
        static const char message[] = "miniOS: unable to block scheduler signals\n";
        write(STDERR_FILENO, message, sizeof(message) - 1);
        return;
    }

    for (;;) {
        int status = 0;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid == 0) {
            break;
        }
        if (pid == -1) {
            if (errno == EINTR) {
                continue;
            }
            if (errno != ECHILD) {
                static const char message[] = "miniOS: waitpid failed in SIGCHLD handler\n";
                write(STDERR_FILENO, message, sizeof(message) - 1);
            }
            break;
        }
        if (!WIFEXITED(status) && !WIFSIGNALED(status)) {
            continue;
        }

        int idx;
        for (idx = 0; idx < process_count; idx++) {
            if (process_table[idx].pid == pid) {
                break;
            }
        }
        if (idx == process_count) {
            continue;
        }

        pcb_t *finished = &process_table[idx];
        int was_running = (idx == current_running);
        int already_terminated = (finished->state == PROC_TERMINATED);

        if (was_running && !already_terminated) {
            struct timespec now;
            if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
                finished->cpu_time_ms += timespec_diff_ms(now, finished->last_started);
            } else {
                static const char message[] = "miniOS: clock_gettime failed in SIGCHLD handler\n";
                write(STDERR_FILENO, message, sizeof(message) - 1);
            }
        }

        if (!already_terminated) {
            finished->state = PROC_TERMINATED;
            monitor_emit_terminated(pid, finished->cpu_time_ms,
                                    finished->context_switches);
        }

        if (was_running) {
            current_running = -1;
            int next_idx = dispatch_next_process();
            if (next_idx >= 0) {
                monitor_emit_switch(pid, process_table[next_idx].pid,
                                    timer_get_slice());
            }
        } else {
            rq_remove(idx);
        }
    }

    if (restore_signal_mask(&previous_mask) == -1) {
        static const char message[] = "miniOS: unable to restore signal mask\n";
        write(STDERR_FILENO, message, sizeof(message) - 1);
    }
}
