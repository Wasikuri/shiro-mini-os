/* Shiro Mini OS - preemptive round-robin scheduler for kernel threads.
 * The timer IRQ (irq0_stub) saves the interrupted context, calls
 * sched_timer() with the context pointer, and resumes whatever
 * context pointer it returns.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "os.h"

static inline void outb(uint16_t port, uint8_t v) {
    __asm__ volatile ("outb %0, %1" :: "a"(v), "Nd"(port));
}

#define MAX_TASKS 8
#define TASK_STACK_SIZE 16384

enum task_state { TASK_READY = 0, TASK_RUNNING = 1, TASK_EXITED = 2 };

struct task {
    uint32_t esp;
    char name[16];
    int state;
    uint32_t ticks_run;
};

static struct task tasks[MAX_TASKS];
static int task_count = 0;
static int current_task = 0;
static int sched_ready = 0;
static uint8_t task_stacks[MAX_TASKS][TASK_STACK_SIZE] __attribute__((aligned(16)));

static void task_name_copy(char *dst, const char *src) {
    int i = 0;
    while (src[i] && i < 15) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static void task_return_exit(void) {
    tasks[current_task].state = TASK_EXITED;
    for (;;) __asm__ volatile ("hlt");
}

int task_create(const char *name, void (*entry)(void)) {
    if (task_count >= MAX_TASKS) return -1;
    int id = task_count++;
    struct task *t = &tasks[id];
    task_name_copy(t->name, name);
    t->state = TASK_READY;
    t->ticks_run = 0;
    /* Build an initial stack that irq0_stub can popa/iret from:
     * [edi esi ebp esp ebx edx ecx eax][eip cs eflags][return addr] */
    uint32_t *sp = (uint32_t *)(task_stacks[id] + TASK_STACK_SIZE);
    *--sp = (uint32_t)task_return_exit; /* popped by entry's ret */
    *--sp = 0x202;                      /* EFLAGS: IF on */
    *--sp = 0x08;                       /* CS: kernel code */
    *--sp = (uint32_t)entry;            /* EIP */
    for (int i = 0; i < 8; i++) *--sp = 0; /* pusha block */
    t->esp = (uint32_t)sp;
    return id;
}

uint32_t sched_timer(uint32_t ctx) {
    timer_ticks++;
    outb(0x20, 0x20); /* EOI */
    if (!sched_ready || task_count < 2) return ctx;
    struct task *cur = &tasks[current_task];
    cur->esp = ctx;
    cur->ticks_run++;
    if (cur->state == TASK_RUNNING) cur->state = TASK_READY;
    int next = current_task;
    for (int i = 1; i <= task_count; i++) {
        int cand = (current_task + i) % task_count;
        if (tasks[cand].state == TASK_READY || tasks[cand].state == TASK_RUNNING) {
            next = cand;
            break;
        }
    }
    if (next == current_task) {
        cur->state = TASK_RUNNING;
        return ctx;
    }
    current_task = next;
    tasks[next].state = TASK_RUNNING;
    return tasks[next].esp;
}

/* ------- demo background task: counter in the top-right corner -------
 * Writes VGA text cells directly (never via print) at row 0,
 * columns 60..79, so it cannot corrupt the shell's output stream. */
static void draw_counter(uint32_t up_secs, uint32_t count) {
    if (fullscreen_ui) return; /* a full-screen UI (editor) owns the screen */
    volatile uint16_t *vga = (volatile uint16_t *)0xB8000;
    char buf[20];
    int i = 0;
    buf[i++] = 'u'; buf[i++] = 'p'; buf[i++] = ':';
    char num[10]; int n = 0;
    uint32_t v = up_secs;
    if (v == 0) num[n++] = '0';
    while (v) { num[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n) buf[i++] = num[--n];
    buf[i++] = ' '; buf[i++] = 'c'; buf[i++] = ':';
    n = 0; v = count;
    if (v == 0) num[n++] = '0';
    while (v) { num[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n) buf[i++] = num[--n];
    while (i < 20) buf[i++] = ' ';
    for (int c = 0; c < 20; c++)
        vga[60 + c] = (uint16_t)(uint8_t)buf[c] | (0x1F << 8); /* white on blue */
}

static void counter_task(void) {
    uint32_t count = 0;
    uint32_t last = timer_ticks;
    draw_counter(uptime_seconds(), count);
    for (;;) {
        while (timer_ticks == last)
            __asm__ volatile ("hlt");
        last = timer_ticks;
        /* Redraw ~10x per second. Compare 10-tick buckets instead of
         * testing last % 10 == 0: this task is only scheduled on
         * alternate ticks, so it can observe a fixed tick parity and
         * never land exactly on a multiple of 10. */
        uint32_t bucket = last / 10;
        if (bucket != count) {
            count = bucket;
            draw_counter(uptime_seconds(), count);
        }
    }
}

void sched_init(void) {
    task_count = 0;
    current_task = 0;
    struct task *t = &tasks[task_count++];
    task_name_copy(t->name, "shell");
    t->state = TASK_RUNNING;
    t->esp = 0; /* filled in on first preemption */
    t->ticks_run = 0;
    task_create("counter", counter_task);
    sched_ready = 1;
}

void sched_print_ps(void) {
    print("  ID  NAME            STATE      TICKS\n");
    for (int i = 0; i < task_count; i++) {
        print("  ");
        print_dec((uint32_t)i);
        print("   ");
        print(tasks[i].name);
        int len = 0; while (tasks[i].name[len]) len++;
        for (int s = len; s < 16; s++) term_putchar(' ');
        print(tasks[i].state == TASK_RUNNING ? "running    " :
              tasks[i].state == TASK_READY   ? "ready      " : "exited     ");
        print_dec(tasks[i].ticks_run);
        print("\n");
    }
}
