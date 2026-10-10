#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "threads/interrupt.h"
#include "threads/io.h"
#include "threads/synch.h"
#include "threads/thread.h"

/* See [8254] for hardware details of the 8254 timer chip. */

#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

/* Number of timer ticks since OS booted. */
static int64_t ticks;

/* Number of loops per timer tick.
   Initialized by timer_calibrate(). */
static unsigned loops_per_tick;
static struct list sleep_list;

static intr_handler_func timer_interrupt;
static bool too_many_loops (unsigned loops);
static void busy_wait (int64_t loops);
static void real_time_sleep (int64_t num, int32_t denom);

/* Sets up the 8254 Programmable Interval Timer (PIT) to
   interrupt PIT_FREQ times per second, and registers the
   corresponding interrupt. */
void
timer_init (void) {
	/* 8254 input frequency divided by TIMER_FREQ, rounded to
	   nearest. */
	uint16_t count = (1193180 + TIMER_FREQ / 2) / TIMER_FREQ;
	list_init(&sleep_list); // 리스트 일단 초기화


	outb (0x43, 0x34);    /* CW: counter 0, LSB then MSB, mode 2, binary. */
	outb (0x40, count & 0xff);
	outb (0x40, count >> 8);

	intr_register_ext (0x20, timer_interrupt, "8254 Timer");
}

/* Calibrates loops_per_tick, used to implement brief delays. */
void
timer_calibrate (void) {
	unsigned high_bit, test_bit;

	ASSERT (intr_get_level () == INTR_ON);
	printf ("Calibrating timer...  ");

	/* Approximate loops_per_tick as the largest power-of-two
	   still less than one timer tick. */
	loops_per_tick = 1u << 10;
	while (!too_many_loops (loops_per_tick << 1)) {
		loops_per_tick <<= 1;
		ASSERT (loops_per_tick != 0);
	}

	/* Refine the next 8 bits of loops_per_tick. */
	high_bit = loops_per_tick;
	for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
		if (!too_many_loops (high_bit | test_bit))
			loops_per_tick |= test_bit;

	printf ("%'"PRIu64" loops/s.\n", (uint64_t) loops_per_tick * TIMER_FREQ);
}

/* Returns the number of timer ticks since the OS booted. */
int64_t // pintos가 시작한 뒤 지금까지 지난 tick수를 안전하게 읽어서 반환하는 함수.
timer_ticks (void) {
	enum intr_level old_level = intr_disable ();
	int64_t t = ticks;
	intr_set_level (old_level);
	barrier ();
	return t;
}

/* Returns the number of timer ticks elapsed since THEN, which
   should be a value once returned by timer_ticks(). */
int64_t
timer_elapsed (int64_t then) {
	return timer_ticks () - then;
}

/* Suspends execution for approximately TICKS timer ticks. */
void
timer_sleep (int64_t ticks) {
	struct thread *cur;
	enum intr_level old_level; 

	ASSERT (intr_get_level () == INTR_ON);
	if (ticks <= 0)
		return;

	old_level = intr_disable (); // 현재 CPU가 타이머 같은 하드웨어 인터럽트를 처리하지 못하게
	// 막기전 상태를 돌려주는 함수,
	cur = thread_current (); // 이미 실행중인 스레드를 가져옴.
	cur->wake_tick = timer_ticks () + ticks; // 지금까지 틱 더하기 들어오는 틱
	list_push_back (&sleep_list, &cur->elem); //
	thread_block (); //  상태 블락으로 만들기 thread_block()지금 실행중인 스레드를 멈추고,다른 스레드에게 CPU를 넘기는 함수.
	intr_set_level (old_level); //  타이머가 이 스레드를 깨우고 다시 실행 순서가 왔을 때 그다음 줄로 돌아와서 원래 인터럽트 상태를 복원
}

/* Suspends execution for approximately MS milliseconds. */
void
timer_msleep (int64_t ms) {
	real_time_sleep (ms, 1000);
}

/* Suspends execution for approximately US microseconds. */
void
timer_usleep (int64_t us) {
	real_time_sleep (us, 1000 * 1000);
}

/* Suspends execution for approximately NS nanoseconds. */
void
timer_nsleep (int64_t ns) {
	real_time_sleep (ns, 1000 * 1000 * 1000);
}

/* Prints timer statistics. */
void
timer_print_stats (void) {
	printf ("Timer: %"PRId64" ticks\n", timer_ticks ());
}

/* Timer interrupt handler. */
static void //시간이 되면 깨우기 sleep_list를 확인하고 READY로 변경
timer_interrupt (struct intr_frame *args UNUSED) { //타이머 인터럽트가 발생할 때마다 실행되는 인터럽트 핸들러
	struct list_elem *e; // 리스트 목록 원소 가라키는 함수

	ticks++;//일단 여기에 들어오면 틱 하나 올리기

	for (e = list_begin (&sleep_list); e != list_end (&sleep_list); ) { // 슬립 리스트의 처음부터 끝까지
		struct thread *t = list_entry (e, struct thread, elem); 

		if (t->wake_tick == ticks) { // 꺠어날 시간이 되었는지 검사 
			e = list_remove (e);//현재 e가 가리키는 요소를 sleep_list에서 제거 , 다음 e가 가리키는 요소 주소 반환 그니깐 다음 노드 주소 알려줌 함수안에서
			thread_unblock (t); // 스레드 레디로 바꾸고 레디 리스트에 넣음
		} else {
			e = list_next (e);// 시간 안되면 다음 스레드로 넘어감
		}
	}

	thread_tick (); //하나의 스레드가 CPU를 계속 독점하지 못하도록 하기 위해서 -> 스케줄러가 정상적으로 작동되어야함.
	//현재 CPU를 사용 중인 스레드가 CPU를 양보할 때가 됐는지 확인하는 역할
}

/* Returns true if LOOPS iterations waits for more than one timer
   tick, otherwise false. */
static bool
too_many_loops (unsigned loops) {
	/* Wait for a timer tick. */
	int64_t start = ticks;
	while (ticks == start)
		barrier ();

	/* Run LOOPS loops. */
	start = ticks;
	busy_wait (loops);

	/* If the tick count changed, we iterated too long. */
	barrier ();
	return start != ticks;
}

/* Iterates through a simple loop LOOPS times, for implementing
   brief delays.

   Marked NO_INLINE because code alignment can significantly
   affect timings, so that if this function was inlined
   differently in different places the results would be difficult
   to predict. */
static void NO_INLINE
busy_wait (int64_t loops) {
	while (loops-- > 0)
		barrier ();
}

/* Sleep for approximately NUM/DENOM seconds. */
static void
real_time_sleep (int64_t num, int32_t denom) {
	/* Convert NUM/DENOM seconds into timer ticks, rounding down.

	   (NUM / DENOM) s
	   ---------------------- = NUM * TIMER_FREQ / DENOM ticks.
	   1 s / TIMER_FREQ ticks
	   */
	int64_t ticks = num * TIMER_FREQ / denom;

	ASSERT (intr_get_level () == INTR_ON);
	if (ticks > 0) {
		/* We're waiting for at least one full timer tick.  Use
		   timer_sleep() because it will yield the CPU to other
		   processes. */
		timer_sleep (ticks);
	} else {
		/* Otherwise, use a busy-wait loop for more accurate
		   sub-tick timing.  We scale the numerator and denominator
		   down by 1000 to avoid the possibility of overflow. */
		ASSERT (denom % 1000 == 0);
		busy_wait (loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000));
	}
}
